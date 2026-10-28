// Standalone: compile with core/Logger.cpp, -Iharpia -Ilibobs, and -pthread.
// The real logger writes real files; only libobs' global log-handler slot is
// replaced so this check needs neither a graphics context nor a running OBS.
#include "core/Logger.hpp"

#include <util/base.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
log_handler_t handler = nullptr;
void *handlerParam = nullptr;
int forwarded = 0;

void previousHandler(int, const char *, va_list, void *)
{
	++forwarded;
}

std::string contents(const std::string &path)
{
	std::ifstream in(path);
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void emitObsInfo(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	if (handler) {
		handler(LOG_INFO, format, args, handlerParam);
	}
	va_end(args);
}
} // namespace

extern "C" void base_get_log_handler(log_handler_t *outHandler, void **param)
{
	*outHandler = handler;
	*param = handlerParam;
}

extern "C" void base_set_log_handler(log_handler_t newHandler, void *param)
{
	handler = newHandler;
	handlerParam = param;
}

int main()
{
	const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto root = std::filesystem::temp_directory_path() / ("harpia-logger-flush-" + std::to_string(token));
	int failed = 0;
	const auto check = [&](bool pass, const char *label) {
		std::printf("%s %s\n", pass ? "PASS" : "FAIL", label);
		failed += !pass;
	};
	auto &logger = harpia::Logger::instance();
	base_set_log_handler(previousHandler, &forwarded);
	logger.flush(); // a snapshot can be requested before the logger starts
	logger.init(root.string());
	const std::string path = logger.sessionFilePath();
	logger.log(harpia::LogLevel::Info, "[harpia] video snapshot: 1280x720 at 30 fps");
	logger.log(harpia::LogLevel::Debug, "[harpia] crop snapshot: x=20 y=40 width=640 height=360");
	emitObsInfo("[harpia] gpu snapshot: %s", "test adapter");
	logger.flush();
	const std::string snapshot = contents(path);
	check(snapshot.find("video snapshot: 1280x720 at 30 fps") != std::string::npos,
	      "buffered app INFO snapshot is readable while logger remains open");
	check(snapshot.find("crop snapshot: x=20 y=40 width=640 height=360") != std::string::npos,
	      "buffered DEBUG crop snapshot is readable while logger remains open");
	check(snapshot.find("gpu snapshot: test adapter") != std::string::npos,
	      "OBS handler INFO reaches the same readable snapshot");
	check(forwarded == 1, "OBS handler still forwards to its previous handler");
	logger.flush();
	check(contents(path) == snapshot, "repeated flush does not duplicate entries");
	logger.log(harpia::LogLevel::Info, "later recording attempt");
	logger.flush();
	check(contents(path).find("later recording attempt") != std::string::npos,
	      "subsequent snapshots publish newly buffered lines");
	logger.log(harpia::LogLevel::Warning, "warning remains immediate");
	check(contents(path).find("warning remains immediate") != std::string::npos,
	      "warnings remain immediately available without explicit flush");
	logger.shutdown();
	const std::string closed = contents(path);
	logger.flush();
	logger.flush();
	check(contents(path) == closed, "flush after shutdown is harmless and preserves the log");
	check(handler == previousHandler && handlerParam == &forwarded,
	      "shutdown still restores the previous OBS handler");
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	return failed ? 1 : 0;
}
