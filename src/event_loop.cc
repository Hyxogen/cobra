#include "cobra/event_loop.hh"

#include <functional>
#include <ios>
#include <system_error>
#include <stdexcept>
#include <iostream>
#include <cerrno>
#include <cstring>

extern "C" {
#include <sys/epoll.h>
#include <unistd.h>
}

namespace cobra {

	event_loop::event_loop(event_loop&&) noexcept {}
	event_loop::~event_loop() {}
	//event_loop& event_loop::operator=(event_loop&&) noexcept { return *this; }

	void event_loop::on_read_ready(int fd, callback_type callback) {
		on_ready(fd, listen_type::read, callback);
	}

	void event_loop::on_write_ready(int fd, callback_type callback) {
		on_ready(fd, listen_type::write, callback);
	}

	epoll_event_loop::epoll_event_loop() {
		epoll_fd = epoll_create(1);

		if (epoll_fd == -1) {
			throw std::system_error(std::make_error_code(static_cast<std::errc>(errno)));
		}
	}

	epoll_event_loop::~epoll_event_loop() {
		int rc = close(epoll_fd);
		if (rc == -1) {
			std::cerr << "Failed to properly close epoll_fd " << epoll_fd << ": "
				<< std::strerror(errno) << std::endl;
		}
	}

	void epoll_event_loop::run() {
		while (!done()) {
			std::vector<std::pair<int, listen_type>> events = poll(1);

			for (auto&& event : events) {
				optional<callback_type> callback = get_callback(event);

				if (callback) {
					(*callback)();
				}

				remove_callback(event);
			}
		}
	}

	std::vector<epoll_event_loop::event_type> epoll_event_loop::poll(std::size_t count) {
		std::vector<epoll_event> events(count);
		int ready_count = 0;

		while (true) {
			int rc = epoll_wait(epoll_fd, events.data(), count, -1);

			if (rc == -1) {
				if (errno != EINTR) {
					throw std::system_error(std::make_error_code(static_cast<std::errc>(errno)));
				}
			} else {
				ready_count = rc;
				break;
			}
		}

		std::vector<event_type> result;
		result.reserve(ready_count);

		for (int idx = 0; idx < ready_count; ++idx) {
			const epoll_event& event = events[idx];
			result.push_back(std::make_pair(event.data.fd, get_type(event.events)));
		}
		return result;
	}

	bool epoll_event_loop::done() const {
		return read_callbacks.empty() && write_callbacks.empty();
	}

	int epoll_event_loop::get_events(listen_type type) const {
		switch (type) {
		case listen_type::write:
			return EPOLLOUT;
		case listen_type::read:
			return EPOLLIN;
		}
		std::terminate();
	}

	epoll_event_loop::listen_type epoll_event_loop::get_type(int event) const {
		if (event == EPOLLIN) {
			return listen_type::read;
		} else if (event == EPOLLOUT) {
			return listen_type::write;
		}
		throw std::domain_error("Invalid event");
	}

	epoll_event_loop::callback_map& epoll_event_loop::get_map(listen_type type) {
		if (type == listen_type::read)
			return read_callbacks;
		return write_callbacks;
	}

	optional<epoll_event_loop::callback_type> epoll_event_loop::get_callback(std::pair<int, listen_type> event) {
		using return_type = optional<callback_type>;
		callback_map& map = get_map(event.second);

		std::lock_guard<std::mutex> guard(mtx);
		auto it = map.find(event.first);

		if (it == map.end())
			return return_type();
		return optional<callback_type>(it->second);
	}

	bool epoll_event_loop::remove_callback(event_type event) {
		callback_map& map = get_map(event.second);

		std::lock_guard<std::mutex> guard(mtx);
		return map.erase(event.first) == 1;
	}

	void epoll_event_loop::on_ready(int fd, listen_type type, callback_type callback) {
		epoll_event event;
		event.events = get_events(type);
		event.data.fd = fd;

		std::lock_guard<std::mutex> guard(mtx);

		callback_map& callbacks = get_map(type);
		if (!callbacks.insert(std::make_pair(fd, callback)).second) {
			throw std::invalid_argument("A callback for this operation was already registered for this fd");
		}

		int rc = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
		if (rc == -1) {
			callbacks.erase(fd);
			throw std::system_error(std::make_error_code(static_cast<std::errc>(errno)));
		}
	}
}