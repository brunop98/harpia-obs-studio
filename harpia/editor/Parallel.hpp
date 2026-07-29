// A fan-out-and-wait over the global thread pool, without QtConcurrent.
//
// This is deliberately not Qt6Concurrent. The two things the app uses it for --
// splitting a pixel pass into row bands, and loading several waveforms at once
// -- are both "run these N independent jobs, wait for all of them", which is a
// dozen lines against QThreadPool from Qt6Core. Depending on the Concurrent
// module for that meant shipping another Qt DLL, and a release where that one
// DLL was bad failed to start at all with "not designed to run on Windows",
// while everything it was needed for was an optimisation that could have run
// serially. One fewer file in the package is one fewer file that can be wrong.
//
// The calling thread runs job 0 itself rather than queueing every job and
// idling: it costs nothing, and it means a saturated pool still makes progress.
#pragma once

#include <QRunnable>
#include <QSemaphore>
#include <QThreadPool>
#include <QVector>

#include <functional>
#include <utility>

namespace harpia {

namespace detail {
// QRunnable over a std::function. Qt6Core has no such adapter of its own (the
// QThreadPool::start(std::function) overload does exist, but it gives no way to
// know when the job is done, which is the whole point here).
class FnRunnable : public QRunnable {
public:
	FnRunnable(std::function<void()> fn, QSemaphore *done) : fn_(std::move(fn)), done_(done)
	{
		setAutoDelete(true);
	}
	void run() override
	{
		fn_();
		done_->release();
	}

private:
	std::function<void()> fn_;
	QSemaphore *done_;
};
} // namespace detail

// Calls fn(i) for every i in [0, n), on several threads, and does not return
// until all of them have finished -- so fn may safely capture by reference.
//
// fn must not throw and must not itself call blockingFor: nesting could park
// every pool thread on a semaphore that only a queued job can release. Neither
// call site does, and QtConcurrent::blockingMap had exactly the same rule.
template <typename F> void blockingFor(int n, F fn)
{
	if (n <= 0)
		return;
	if (n == 1) {
		fn(0);
		return;
	}
	QThreadPool *pool = QThreadPool::globalInstance();
	QSemaphore done;
	int queued = 0;
	for (int i = 1; i < n; ++i) {
		pool->start(new detail::FnRunnable([&fn, i]() { fn(i); }, &done));
		++queued;
	}
	fn(0);
	done.acquire(queued);
}

// The mapped form: results[i] = fn(items[i]), same ordering, same guarantees.
template <typename T, typename F>
auto blockingMapped(const QVector<T> &items, F fn) -> QVector<decltype(fn(items.first()))>
{
	QVector<decltype(fn(items.first()))> out(items.size());
	blockingFor(items.size(), [&](int i) { out[i] = fn(items[i]); });
	return out;
}

} // namespace harpia
