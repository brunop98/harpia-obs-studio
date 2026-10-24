// The loading wait must let the GUI repaint while decoding, hand move-only
// resources back only after the worker has stopped, and recover from errors.
#include "editor/LoadingTask.hpp"

#include <QCoreApplication>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace harpia;

static int failures = 0;
static void ok(bool condition, const char *what)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		++failures;
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	const std::thread::id guiThread = std::this_thread::get_id();
	std::promise<void> guiTick;
	std::future<void> tick = guiTick.get_future();
	bool timerRanOnGui = false;
	QTimer::singleShot(0, &app, [&]() {
		timerRanOnGui = std::this_thread::get_id() == guiThread;
		guiTick.set_value();
	});
	bool taskRanOnWorker = false;
	bool timerRanDuringTask = false;
	auto resource = runLoadingTask([input = std::make_unique<int>(42), &tick, &taskRanOnWorker,
					 &timerRanDuringTask, guiThread]() mutable {
		taskRanOnWorker = std::this_thread::get_id() != guiThread;
		// A blocked GUI would fail this handshake. The bound makes the
		// failure a test result instead of leaving the test hung forever.
		timerRanDuringTask = tick.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
		return std::move(input);
	});
	ok(taskRanOnWorker, "the task executes off the application thread");
	ok(timerRanOnGui && timerRanDuringTask, "GUI timers run before the task completes");
	ok(resource && *resource == 42, "move-only input and result transfer to their new owner");

	bool voidFinished = false;
	runLoadingTask([&]() { voidFinished = true; });
	ok(voidFinished, "void tasks finish before returning");

	std::atomic<bool> unwound{false};
	bool propagated = false;
	try {
		runLoadingTask([&]() -> int {
			struct MarkExit {
				std::atomic<bool> &flag;
				~MarkExit() { flag.store(true); }
			} mark{unwound};
			throw std::runtime_error("decoder failed");
		});
	} catch (const std::runtime_error &error) {
		propagated = std::string(error.what()) == "decoder failed";
	}
	ok(propagated && unwound.load(), "worker errors propagate after its stack has unwound");

	bool immediateResults = true;
	for (int i = 0; i < 32; ++i)
		immediateResults &= runLoadingTask([i]() { return i; }) == i;
	ok(immediateResults, "immediate completion and subsequent tasks do not lose their wakeup");
	std::printf("%s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
