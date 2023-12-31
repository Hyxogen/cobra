#include "cobra/http/server.hh"

#include "cobra/asyncio/stream_buffer.hh"
#include "cobra/config.hh"
#include "cobra/http/handler.hh"
#include "cobra/http/parse.hh"

#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <cassert>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <variant>

namespace cobra {

	http_filter::http_filter(std::shared_ptr<const config::config> config, std::size_t match_count)
		: _config(config), _match_count(match_count + config->location.size()) {
		for (const auto& sub_filter : _config->sub_configs) {
			_sub_filters.push_back(http_filter(sub_filter, _match_count));
		}
	}

	http_filter::http_filter(std::shared_ptr<const config::config> config) : http_filter(config, 0) {}

	http_filter::http_filter(std::shared_ptr<const config::config> config, std::vector<http_filter> filters)
		: _config(config), _sub_filters(std::move(filters)) , _match_count(0) {}

	http_filter* http_filter::match(const basic_socket_stream& socket, const http_request& request, const uri_abs_path& normalized) {
		if (eval(socket, request, normalized)) {
			for (auto& filter : _sub_filters) {
				auto m = filter.match(socket, request, normalized);
				if (m)
					return m;
			}
			return this;
		}
		return nullptr;
	}

	bool http_filter::eval(const basic_socket_stream& socket, const http_request& request, const uri_abs_path& normalized) const {
		if (!config().server_names.empty()) {
			if (socket.server_name()) {
				if (!config().server_names.contains(std::string(*socket.server_name()))) {
					return false;
				}
			} else {
				if (!request.has_header("host")) {
					return false;
				}
				if (!config().server_names.contains(request.header("host"))) {
					return false;
				}
			}
		}

		if (!config().location.empty()) {
			const std::size_t already_matched = match_count() - config().location.size();

			auto it = normalized.begin() + already_matched;

			for (auto& part : config().location) {
					if (it == normalized.end()) {
						eprintln("too short, expected: {}", part);
						return false;
					} else if (*it != part) {
						eprintln("{} != {}", *it, part);
						return false;
					}
				++it;
			}
		}

		if (!config().methods.empty() && !config().methods.contains(request.method())) {
			eprintln("other method");
			return false;
		}
		return true;
	}

	server::server(config::listen_address address, std::unordered_map<std::string, ssl_ctx> contexts, std::vector<http_filter> filters, executor* exec, event_loop* loop)
		: http_filter(std::shared_ptr<config::config>(new config::config()), std::move(filters)),
		  _address(std::move(address)), _contexts(std::move(contexts)), _exec(exec), _loop(loop) {}

	task<void> server::on_connect(basic_socket_stream& socket) {
		istream_buffer socket_istream(make_istream_ref(socket), 1024);
		ostream_buffer socket_ostream(make_ostream_ref(socket), 1024);
		http_server_logger logger;
		http_response_writer writer(socket_ostream, &logger);
		logger.set_socket(socket);

		http_request request("GET", parse_uri("/", "GET"));
		bool bad_request = false;
		bool unknown_error = false;

		try {
			request = co_await parse_http_request(socket_istream);
			logger.set_request(request);

			const uri_origin* org = request.uri().get<uri_origin>();
			if (!org) {
				eprintln("request did not contain uri_origin");
				co_await std::move(writer).send(HTTP_BAD_REQUEST);
				co_return;
			}
			const uri_abs_path normalized = org->path().normalize();

			auto filter = match(socket, request, normalized);

			if (!filter || !filter->config().handler) {
				if (!filter)
					std::cerr << "no filter matched" << std::endl;
				else if (!filter->config().handler)
					std::cerr << "no handler" << std::endl;
				co_await std::move(writer).send(HTTP_NOT_FOUND);
				co_return;
			}

			co_await handle_request(*filter, request, normalized, socket_istream, std::move(writer));
			// co_await (*handler)(std::move(writer), std::move(request), buffered_istream_reference(socket_istream));
			// TODO limit socket_istream to client content-length
		} catch (http_parse_error err) {
			bad_request = true;
		} catch (uri_parse_error err) {
			bad_request = true;
		} catch (const std::exception& ex) {
			unknown_error = true;
			std::cerr << ex.what() << std::endl;
		} catch (...) {
			unknown_error = true;
			std::cerr << "something went very very wrong" << std::endl;
		}

		if (bad_request) {
			co_await std::move(writer).send(HTTP_BAD_REQUEST);
			co_return;
		} else if (unknown_error) {
			co_await std::move(writer).send(HTTP_INTERNAL_SERVER_ERROR);
			co_return;
		}
	}

	task<void> server::handle_request(const http_filter& filt, const http_request& request, const uri_abs_path& normalized,
									  buffered_istream_reference in, http_response_writer writer) {
		// TODO write headers set in config
		// TODO properly match uri
		fs::path file("/");
		for (std::size_t i = filt.match_count(); i < normalized.size(); ++i) {
			file.append(normalized[i]);
		}

		// TODO do properly: https://datatracker.ietf.org/doc/html/rfc9112#name-message-body-length
		// TODO utility file for int parsing etc..
		std::size_t content_length =
			request.has_header("content-length") ? std::stoull(request.header("content-length")) : 0;
		auto limited_stream = istream_limit(std::move(in), content_length);

		//TODO do without allocations
		auto root = filt.config().root.value_or("").string();
		auto index = std::vector<std::string>(1, filt.config().index.value_or("").string());

		if (auto cfg = std::get_if<config::cgi_config>(&*filt.config().handler)) {
			co_await handle_cgi(std::move(writer),
								{_loop, _exec, root, file.string(), index, // TODO avoid duplicating strings
								 cgi_config(cgi_command(cfg->command.file())), request, limited_stream});
		} else if (auto cfg = std::get_if<config::fast_cgi_config>(&*filt.config().handler)) {
			auto service = std::format("{}", cfg->address.service());
			co_await handle_cgi(std::move(writer),
								{_loop, _exec, root, file.string(), index,
								 cgi_config(cgi_address(cfg->address.node(), service)),
								 request, limited_stream});
		} else if (auto cfg = std::get_if<config::static_file_config>(&*filt.config().handler)) {
			co_await handle_static(std::move(writer),
								   {_loop, _exec, root, file.string(), index, {}, request, limited_stream});
		} else {
			assert(0 && "unimplemented");
		}
	}

	task<void> server::start(executor* exec, event_loop* loop) {
		std::string service = std::to_string(_address.service());
		if (_contexts.empty()) {
			std::cout << _address.node() << ":" << service << std::endl;
			co_return co_await start_server(exec, loop, _address.node().data(), service.c_str(),
											[this](socket_stream socket) -> task<void> {
												co_return co_await on_connect(socket);
											});
		} else if (_contexts.size() == 1 && _contexts.begin()->first.empty()) {
			//No SNI
			//TODO DOESNT WORK PROPERLY TEST!
			eprintln("ssl {}:{}", _address.node(), service);
			co_return co_await start_ssl_server(_contexts.begin()->second, exec, loop, _address.node().data(),
												service.c_str(), [this](ssl_socket_stream socket) -> task<void> {
													co_return co_await on_connect(socket);
												});
		} else {
			//With SNI
			eprintln("ssl {}:{} SNI", _address.node(), service);
			co_return co_await start_ssl_server(_contexts, exec, loop, _address.node().data(),
												service.c_str(), [this](ssl_socket_stream socket) -> task<void> {
													co_return co_await on_connect(socket);
												});
		}
	}

	std::vector<server> server::convert(const std::vector<std::shared_ptr<config::server>>& configs,
										executor* exec, event_loop* loop) {
		std::map<config::listen_address, std::unordered_map<std::string, ssl_ctx>> contexts;
		std::map<config::listen_address, std::vector<http_filter>> filters;

		for (const auto& config : configs) {
			for (const auto& address : config->addresses) {
				filters[address].push_back(http_filter(config));
				if (config->ssl) {
					for (const auto& server_name : config->server_names) {
						contexts[address].insert({server_name, ssl_ctx::server(config->ssl->cert(), config->ssl->key())});
					}
				}
			}

		}

		std::vector<server> result;
		result.reserve(filters.size());
		for (const auto& [listen, filters] : filters) {
			std::unordered_map<std::string, ssl_ctx> ssl;
			if (contexts.contains(listen)) {
				ssl = contexts.at(listen);
			}
			result.push_back(server(listen, std::move(ssl), filters, exec, loop));
		}
		return result;
	}
} // namespace cobra
