#ifndef COBRA_ASYNCIO_TASK_HH
#define COBRA_ASYNCIO_TASK_HH

#include "cobra/asyncio/coroutine.hh"
#include "cobra/asyncio/promise.hh"

#include <atomic>

namespace cobra {
	template <class T>
	class task_promise;

	template <class T>
	class [[nodiscard]] task : public coroutine<task_promise<T>> {
	public:
		bool await_ready() const noexcept {
			coroutine<task_promise<T>>::handle().resume();
			bool flag = coroutine<task_promise<T>>::handle().promise().flag().test();
			return flag;
		}

		void await_suspend(std::coroutine_handle<> handle) const noexcept {
			coroutine<task_promise<T>>::handle().promise().set_next(handle);

			if (coroutine<task_promise<T>>::handle().promise().flag().test_and_set()) {
				coroutine<task_promise<T>>::handle().promise().next().resume();
			}
		}

		T await_resume() {
			return coroutine<task_promise<T>>::handle().promise().result().get_value_move();
		}
	};

	class task_final_suspend {
	public:
		bool await_ready() const noexcept {
			return false;
		}

		template <class T>
		void await_suspend(std::coroutine_handle<task_promise<T>> handle) const noexcept {
			if (handle.promise().flag().test_and_set()) {
				handle.promise().next().resume();
			}
		}

		void await_resume() const noexcept {
			return;
		}
	};

	template <class T>
	class task_promise : public promise<T> {
		std::atomic_flag _flag;

	public:
		task<T> get_return_object() noexcept {
			return {*this};
		}

		auto initial_suspend() const noexcept {
			return std::suspend_always();
		}

		auto final_suspend() const noexcept {
			return task_final_suspend();
		}

		std::atomic_flag& flag() noexcept {
			return _flag;
		}
	};
} // namespace cobra

#endif
