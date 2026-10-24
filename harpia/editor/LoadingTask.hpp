#pragma once

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QThread>

#include <functional>
#include <future>
#include <thread>
#include <type_traits>
#include <utility>

namespace harpia {

// Run a file read/decode away from the GUI, while a loading dialog can paint.
// The caller must be on the application thread, and its editor state must be
// stable throughout this wait: timers and queued callbacks still run. The task
// owns its inputs and must never access widgets or mutable editor state.
//
// Joining before returning also makes ownership transfer of a decoder or other
// move-only result safe: the worker has finished using it before the caller can.
template<typename F> decltype(auto) runLoadingTask(F &&fn)
{
	Q_ASSERT(QCoreApplication::instance());
	Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
	using Task = std::decay_t<F>;
	using Result = std::invoke_result_t<Task &>;
	QEventLoop wait;
	std::promise<Result> answer;
	std::future<Result> ready = answer.get_future();
	std::thread worker([task = Task(std::forward<F>(fn)), answer = std::move(answer), &wait]() mutable {
		try {
			if constexpr (std::is_void_v<Result>) {
				std::invoke(task);
				answer.set_value();
			} else {
				answer.set_value(std::invoke(task));
			}
		} catch (...) {
			answer.set_exception(std::current_exception());
		}
		// Queued to a stack-owned GUI object. Even an immediate result posts
		// completion for the event loop below, rather than losing an early quit.
		QMetaObject::invokeMethod(&wait, &QEventLoop::quit, Qt::QueuedConnection);
	});
	wait.exec(QEventLoop::ExcludeUserInputEvents);
	worker.join();
	return ready.get(); // also propagates a task's exception, after joining
}

} // namespace harpia
