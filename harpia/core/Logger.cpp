#include "Logger.hpp"

#include "Version.hpp"

#include <util/base.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>

namespace fs = std::filesystem;

namespace harpia {

namespace {
constexpr int kMaxSessionFiles = 20; // keep this many rotated logs

long long nowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::system_clock::now().time_since_epoch())
		.count();
}

std::string timestamp(long long ms, const char *fmt)
{
	std::time_t secs = (std::time_t)(ms / 1000);
	std::tm tmv {};
#if defined(_WIN32)
	localtime_s(&tmv, &secs);
#else
	localtime_r(&secs, &tmv);
#endif
	char buf[64];
	std::strftime(buf, sizeof(buf), fmt, &tmv);
	return buf;
}

LogLevel fromObsLevel(int lvl)
{
	if (lvl <= LOG_ERROR)
		return LogLevel::Error;
	if (lvl <= LOG_WARNING)
		return LogLevel::Warning;
	if (lvl <= LOG_INFO)
		return LogLevel::Info;
	return LogLevel::Debug;
}
} // namespace

const char *logLevelName(LogLevel level)
{
	switch (level) {
	case LogLevel::Error:
		return "ERROR";
	case LogLevel::Warning:
		return "WARNING";
	case LogLevel::Info:
		return "INFO";
	case LogLevel::Debug:
		return "DEBUG";
	}
	return "INFO";
}

Logger &Logger::instance()
{
	static Logger logger;
	return logger;
}

void Logger::init(const std::string &logDir)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (initialized_)
		return;

	logDir_ = logDir;
	std::error_code ec;
	fs::create_directories(logDir_, ec);

	// Prune old sessions (keep the most recent kMaxSessionFiles-1 before opening
	// a new one).
	std::vector<fs::path> logs;
	if (fs::exists(logDir_, ec)) {
		for (const auto &e : fs::directory_iterator(logDir_, ec)) {
			if (e.path().extension() == ".log")
				logs.push_back(e.path());
		}
	}
	std::sort(logs.begin(), logs.end()); // filenames are timestamped → chronological
	while ((int)logs.size() >= kMaxSessionFiles) {
		fs::remove(logs.front(), ec);
		logs.erase(logs.begin());
	}

	const long long startMs = nowMs();
	filePath_ = logDir_ + "/harpia-" + timestamp(startMs, "%Y-%m-%d_%H-%M-%S") + ".log";
	file_ = std::fopen(filePath_.c_str(), "a");

	base_get_log_handler(reinterpret_cast<log_handler_t *>(&prevHandler_), &prevParam_);
	base_set_log_handler(&Logger::obsLogHandler, this);

	initialized_ = true;

	// Session header line.
	if (file_) {
		std::fprintf(file_, "===== Harpia Recorder v%s — session started %s =====\n",
			     HARPIA_VERSION_STRING, timestamp(startMs, "%Y-%m-%d %H:%M:%S").c_str());
		std::fflush(file_);
	}
}

void Logger::shutdown()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!initialized_)
		return;
	base_set_log_handler(reinterpret_cast<log_handler_t>(prevHandler_), prevParam_);
	if (file_) {
		std::fclose(file_);
		file_ = nullptr;
	}
	initialized_ = false;
}

void Logger::writeLine(LogLevel level, const std::string &text)
{
	if (!file_)
		return;
	const std::string ts = timestamp(nowMs(), "%H:%M:%S");
	std::fprintf(file_, "[%s] %-7s %s\n", ts.c_str(), logLevelName(level), text.c_str());
	std::fflush(file_); // crash-safe: get it to disk immediately
}

void Logger::obsLogHandler(int lvl, const char *msg, va_list args, void *param)
{
	auto *self = static_cast<Logger *>(param);

	char buf[4096];
	va_list cp;
	va_copy(cp, args);
	std::vsnprintf(buf, sizeof(buf), msg, cp);
	va_end(cp);

	{
		std::lock_guard<std::mutex> lock(self->mutex_);
		self->writeLine(fromObsLevel(lvl), buf);
	}

	// Chain to the previous handler so normal console logging still happens.
	if (self->prevHandler_)
		self->prevHandler_(lvl, msg, args, self->prevParam_);
}

void Logger::log(LogLevel level, const std::string &text)
{
	std::lock_guard<std::mutex> lock(mutex_);
	writeLine(level, text);
}

std::string Logger::logDir() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return logDir_;
}

std::string Logger::sessionFilePath() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return filePath_;
}

std::vector<std::string> Logger::sessionFiles() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<std::string> out;
	std::error_code ec;
	if (!fs::exists(logDir_, ec))
		return out;
	std::vector<fs::path> logs;
	for (const auto &e : fs::directory_iterator(logDir_, ec)) {
		if (e.path().extension() == ".log")
			logs.push_back(e.path());
	}
	std::sort(logs.begin(), logs.end(), std::greater<>()); // newest first
	for (const auto &p : logs)
		out.push_back(p.string());
	return out;
}

void Logger::clearAll()
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::error_code ec;
	if (!fs::exists(logDir_, ec))
		return;
	// Compare as normalized paths, not raw strings — filePath_ is built with
	// '/' while directory_iterator yields native separators ('\' on Windows),
	// so a string compare could match nothing and delete the live log.
	const fs::path current = fs::path(filePath_).lexically_normal();
	for (const auto &e : fs::directory_iterator(logDir_, ec)) {
		if (e.path().extension() == ".log" && e.path().lexically_normal() != current)
			fs::remove(e.path(), ec);
	}
}

} // namespace harpia
