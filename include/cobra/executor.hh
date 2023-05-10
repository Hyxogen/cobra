#ifndef COBRA_EXECUTOR_HH
#define COBRA_EXECUTOR_HH

#include "cobra/function.hh"

#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace cobra {
	class context;

	class executor {
	protected:
		context* ctx;
	public:
		executor(context* ctx);

		virtual ~executor();
		virtual void exec(function<void>&& func) = 0;
		virtual bool done() const = 0;
	};

	class sequential_executor : public executor {
	public:
		sequential_executor(context* ctx);

		void exec(function<void>&& func) override;
		bool done() const override;
	};

	class thread_pool_executor : public executor {
	private:
		std::queue<function<void>> funcs;
		std::vector<std::thread> threads;
		std::condition_variable condition_variable;
		bool stopped;
		std::size_t count;
	public:
		thread_pool_executor(context* ctx, std::size_t size);
		~thread_pool_executor();

		void exec(function<void>&& func) override;
		bool done() const override;
	};
}

#endif
