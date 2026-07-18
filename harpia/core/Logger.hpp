#pragma once

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace harpia {

enum class LogLevel { Error, Warning, Info, Debug };

// Captures runtime log output (libobs via base_set_log_handler, and Qt via a
// forwarded message handler) and writes it to a per-session log file, flushing
// each line so entries survive an unexpected crash. Log files persist across
// launches under <config>/harpia-recorder/logs, so previous sessions' crashes
// stay reviewable.
//
// Qt-free by design: main() forwards Qt messages in via log(); the ErrorLogsPanel
// reads the on-disk files directly for display.
class Logger {
public:
	static Logger &instance();

	// Open this session's log file and install the libobs handler (chaining to
	// the previous one so console output continues). Call once, early in main().
	void init(const std::string &logDir);
	void shutdown();

	// Add an entry from an external source (e.g. the Qt message handler).
	void log(LogLevel level, const std::string &text);

	std::string logDir() const;
	std::string sessionFilePath() const;

	// All session log files in the dir (full paths), newest first.
	std::vector<std::string> sessionFiles() const;

	// Delete every stored log file except the current session's.
	void clearAll();

private:
	using RawLogHandler = void (*)(int lvl, const char *msg, va_list args, void *param);
	static void obsLogHandler(int lvl, const char *msg, va_list args, void *param);
	void writeLine(LogLevel level, const std::string &text);

	mutable std::mutex mutex_;
	std::string logDir_;
	std::string filePath_;
	std::FILE *file_ = nullptr;
	RawLogHandler prevHandler_ = nullptr;
	void *prevParam_ = nullptr;
	bool initialized_ = false;
};

const char *logLevelName(LogLevel level);

} // namespace harpia
