#pragma once

// What happens when Harpia dies without permission.
//
// A crash used to leave nothing but a truncated session log and, since
// v0.1.241, the orphaned-recording dialog on the next start -- which says a
// crash HAPPENED but nothing about what it was. This adds the missing half:
//
//   * AT CRASH TIME, process-wide handlers (fatal signals, unhandled C++
//     exceptions, Windows SEH, libobs bcrash via main.cpp) write one small
//     report -- version, time, and a description of what went wrong -- to a
//     fixed "pending" file. On Windows a native message box shows the same
//     description immediately; native, because the process is dying and Qt
//     cannot be trusted inside it (its heap may be the thing that broke).
//   * ON THE NEXT LAUNCH, MainWindow finds the pending report, shows a real
//     dialog with the description, and archives the file beside it.
//
// Everything here is Qt-Core-only and libobs-free, so the whole path -- the
// handlers included -- is exercised by crashreport_test, which crashes real
// child processes and reads the reports they leave behind.

#include <QString>

#include <string>

namespace harpia {

class CrashGuard {
public:
	// Install the process-wide handlers. reportDir is created if missing.
	// Call once, as early as possible -- before libobs, before any window.
	static void install(const std::string &reportDir);

	// Write the pending report. Called by the handlers; public so other
	// last-gasp paths (libobs bcrash, Qt fatal) can feed their message in.
	// Writes at most once per process: the FIRST description is the one that
	// names the real cause, and handlers cascade (abort inside a signal
	// handler would otherwise overwrite it with "Aborted").
	static void writeReport(const char *reason);

	// Human names for what killed the process. Pure; pinned by the test.
	static const char *describeSignal(int sig);
#if defined(_WIN32)
	static const char *describeException(unsigned long code);
#endif

	// Startup side: the pending report's text, or empty when the last exit
	// was clean. Archives the pending file (crash-<epoch>.txt) so history
	// survives, keeping only the most recent few.
	static QString takePendingReport(const QString &reportDir);
	static QString pendingPath(const QString &reportDir);
};

} // namespace harpia
