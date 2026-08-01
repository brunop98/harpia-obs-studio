#include "CrashGuard.hpp"

#include "Version.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfoList>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace harpia {

namespace {

// Fixed storage, prepared at install time. A crash handler must not allocate:
// the heap may be exactly what broke.
char g_reportPath[1024] = {0};
std::atomic<bool> g_written{false};

constexpr const char *kPendingName = "crash-pending.txt";
constexpr int kKeepArchived = 10;

// The write itself. fopen/fwrite are not strictly async-signal-safe, but they
// are the accepted practice in crash handlers (libobs's own does the same):
// the alternative is losing the report entirely, and the process is about to
// die either way.
void writePendingFile(const char *reason)
{
	if (!g_reportPath[0])
		return;
	std::FILE *f = std::fopen(g_reportPath, "w");
	if (!f)
		return;
	char stamp[64] = {0};
	const std::time_t now = std::time(nullptr);
	if (std::tm *tm = std::localtime(&now))
		std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tm);
	std::fprintf(f, "Harpia Recorder v%s crashed\n%s\n\n%s\n", HARPIA_VERSION_STRING, stamp,
		     reason ? reason : "(no description)");
	std::fflush(f);
	std::fclose(f);
}

void handleSignal(int sig)
{
	CrashGuard::writeReport(CrashGuard::describeSignal(sig));
	// Hand the signal back to the OS default so the platform's own crash
	// machinery (core dump, WER) still runs -- this report is IN ADDITION to
	// that, not a replacement for it.
	std::signal(sig, SIG_DFL);
	std::raise(sig);
}

void handleTerminate()
{
	// An exception nobody caught. Pull its message out where there is one --
	// "terminate called" with no reason is exactly the mystery this file
	// exists to end.
	char buf[1024];
	std::strcpy(buf, "Unhandled C++ exception (std::terminate)");
	if (std::exception_ptr p = std::current_exception()) {
		try {
			std::rethrow_exception(p);
		} catch (const std::exception &e) {
			std::snprintf(buf, sizeof(buf), "Unhandled C++ exception: %s", e.what());
		} catch (...) {
			std::strcpy(buf, "Unhandled C++ exception (not derived from std::exception)");
		}
	}
	CrashGuard::writeReport(buf);
	std::abort();
}

#if defined(_WIN32)
LONG WINAPI handleSeh(EXCEPTION_POINTERS *info)
{
	char buf[512];
	const unsigned long code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
	const void *addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
	std::snprintf(buf, sizeof(buf), "%s (exception 0x%08lX at %p)", CrashGuard::describeException(code),
		      code, addr);
	CrashGuard::writeReport(buf);
	// The immediate dialog, as asked for. Native MessageBoxA, never Qt: the
	// process is dying and Qt's own state may be the casualty. SYSTEMMODAL so
	// it surfaces even over a full-screen recording target.
	char msg[768];
	std::snprintf(msg, sizeof(msg),
		      "Harpia Recorder has crashed.\n\n%s\n\nA crash report was saved and will be shown "
		      "on the next start. Any in-progress recording can be recovered there too.",
		      buf);
	MessageBoxA(nullptr, msg, "Harpia Recorder crashed", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
	return EXCEPTION_EXECUTE_HANDLER; // die now, after the report
}
#endif

} // namespace

void CrashGuard::install(const std::string &reportDir)
{
	if (reportDir.empty())
		return;
	QDir().mkpath(QString::fromStdString(reportDir));
	std::snprintf(g_reportPath, sizeof(g_reportPath), "%s/%s", reportDir.c_str(), kPendingName);

	// Every fatal path there is. They cascade (abort inside a handler raises
	// SIGABRT), which is why writeReport keeps only the FIRST description.
	std::signal(SIGSEGV, handleSignal);
	std::signal(SIGABRT, handleSignal);
	std::signal(SIGFPE, handleSignal);
	std::signal(SIGILL, handleSignal);
#if !defined(_WIN32)
	std::signal(SIGBUS, handleSignal);
#endif
	std::set_terminate(handleTerminate);
#if defined(_WIN32)
	SetUnhandledExceptionFilter(handleSeh);
#endif
}

void CrashGuard::writeReport(const char *reason)
{
	// Once per process: the first handler to fire names the actual cause;
	// everything after it is fallout.
	bool expected = false;
	if (!g_written.compare_exchange_strong(expected, true))
		return;
	writePendingFile(reason);
}

const char *CrashGuard::describeSignal(int sig)
{
	switch (sig) {
	case SIGSEGV:
		return "Segmentation fault (invalid memory access)";
	case SIGABRT:
		return "Aborted (a fatal internal check failed)";
	case SIGFPE:
		return "Arithmetic error (division by zero or overflow)";
	case SIGILL:
		return "Illegal instruction (corrupted code or bad build)";
#if !defined(_WIN32)
	case SIGBUS:
		return "Bus error (misaligned or unmapped memory access)";
#endif
	default:
		return "Fatal signal";
	}
}

#if defined(_WIN32)
const char *CrashGuard::describeException(unsigned long code)
{
	switch (code) {
	case EXCEPTION_ACCESS_VIOLATION:
		return "Access violation (invalid memory access)";
	case EXCEPTION_STACK_OVERFLOW:
		return "Stack overflow";
	case EXCEPTION_ILLEGAL_INSTRUCTION:
		return "Illegal instruction";
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:
		return "Division by zero";
	case EXCEPTION_IN_PAGE_ERROR:
		return "Memory page could not be read (bad disk or network drive?)";
	default:
		return "Fatal Windows exception";
	}
}
#endif

QString CrashGuard::pendingPath(const QString &reportDir)
{
	return reportDir + QLatin1Char('/') + QLatin1String(kPendingName);
}

QString CrashGuard::takePendingReport(const QString &reportDir)
{
	QFile f(pendingPath(reportDir));
	if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text))
		return QString();
	const QString text = QString::fromUtf8(f.readAll());
	f.close();

	// Archive rather than delete -- "it crashed again, same as last week?"
	// is a question the files should be able to answer -- but keep only the
	// most recent few, because a crashing install must not also be a growing
	// one.
	const QString archived = QStringLiteral("%1/crash-%2.txt")
					 .arg(reportDir)
					 .arg(QDateTime::currentMSecsSinceEpoch());
	if (!QFile::rename(pendingPath(reportDir), archived))
		QFile::remove(pendingPath(reportDir));
	QFileInfoList olds = QDir(reportDir).entryInfoList({QStringLiteral("crash-*.txt")}, QDir::Files,
							   QDir::Name | QDir::Reversed);
	for (int i = kKeepArchived; i < olds.size(); ++i)
		if (olds[i].fileName() != QLatin1String(kPendingName))
			QFile::remove(olds[i].absoluteFilePath());

	return text.trimmed();
}

} // namespace harpia
