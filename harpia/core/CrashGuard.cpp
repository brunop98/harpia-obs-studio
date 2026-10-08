#include "CrashGuard.hpp"

#include "AppName.hpp"
#include "Version.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfoList>

#include <algorithm>
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
// After windows.h, which it needs. The stack and the minidump in a crash
// report come from here.
#include <dbghelp.h>
#ifdef _MSC_VER
#pragma comment(lib, "dbghelp.lib")
#endif
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
	std::fprintf(f, HARPIA_APP_DISPLAY_NAME " v%s crashed\n%s\n\n%s\n", HARPIA_VERSION_STRING, stamp,
		     reason ? reason : "(no description)");
	std::fflush(f);
	std::fclose(f);
}

#if defined(_WIN32)
// Where the minidump goes (wide: a user folder can have any name), and
// whether dbghelp found the program's symbols. Both prepared at install time.
wchar_t g_dumpPath[1024] = {0};
bool g_symsReady = false;
// The report text: big enough for a stack, and static -- a crash handler must
// not count on the heap.
char g_reportBuf[16384];

// The crashing thread's stack, one line per frame:
//
//   #3  harpia-recorder.exe!harpia::BatchExporter::run +0x1a2  (BatchExport.cpp:118)
//
// Names and lines come from the .pdb next to the program (RelWithDebInfo has
// them); without it, each frame is still its module and offset, which a .pdb
// matches later.
void appendStack(char *buf, size_t cap, const CONTEXT *start)
{
	HANDLE proc = GetCurrentProcess();
	HANDLE thread = GetCurrentThread();
	CONTEXT ctx = *start;
	STACKFRAME64 frame;
	std::memset(&frame, 0, sizeof(frame));
#if defined(_M_X64)
	const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
	frame.AddrPC.Offset = ctx.Rip;
	frame.AddrFrame.Offset = ctx.Rbp;
	frame.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64)
	const DWORD machine = IMAGE_FILE_MACHINE_ARM64;
	frame.AddrPC.Offset = ctx.Pc;
	frame.AddrFrame.Offset = ctx.Fp;
	frame.AddrStack.Offset = ctx.Sp;
#else
	(void)proc;
	(void)thread;
	return;
#endif
#if defined(_M_X64) || defined(_M_ARM64)
	frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrFrame.Mode = AddrModeFlat;
	frame.AddrStack.Mode = AddrModeFlat;
	size_t len = std::strlen(buf);
	const auto add = [&](int n) {
		if (n < 0 || size_t(n) >= cap - len)
			return false;
		len += size_t(n);
		return true;
	};
	if (!add(std::snprintf(buf + len, cap - len, "\nStack (crashing thread):\n")))
		return;
	for (int i = 0; i < 48; ++i) {
		if (!StackWalk64(machine, proc, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
				 SymGetModuleBase64, nullptr))
			break;
		const DWORD64 pc = frame.AddrPC.Offset;
		if (pc == 0)
			break;
		alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256];
		std::memset(symBuf, 0, sizeof(symBuf));
		auto *sym = reinterpret_cast<SYMBOL_INFO *>(symBuf);
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		DWORD64 disp = 0;
		const char *name = "?";
		if (g_symsReady && SymFromAddr(proc, pc, &disp, sym))
			name = sym->Name;
		char modPath[MAX_PATH] = "?";
		const DWORD64 base = SymGetModuleBase64(proc, pc);
		if (base)
			GetModuleFileNameA(reinterpret_cast<HMODULE>(base), modPath, MAX_PATH);
		const char *mod = modPath;
		for (const char *c = modPath; *c; ++c)
			if (*c == '\\' || *c == '/')
				mod = c + 1;
		IMAGEHLP_LINE64 line;
		std::memset(&line, 0, sizeof(line));
		line.SizeOfStruct = sizeof(line);
		DWORD lineDisp = 0;
		int n;
		if (g_symsReady && SymGetLineFromAddr64(proc, pc, &lineDisp, &line) && line.FileName) {
			const char *file = line.FileName;
			for (const char *c = line.FileName; *c; ++c)
				if (*c == '\\' || *c == '/')
					file = c + 1;
			n = std::snprintf(buf + len, cap - len, "  #%-2d %s!%s +0x%llx  (%s:%lu)\n", i, mod, name,
					  (unsigned long long)disp, file, (unsigned long)line.LineNumber);
		} else {
			n = std::snprintf(buf + len, cap - len, "  #%-2d %s!%s +0x%llx  [module+0x%llx]\n", i, mod,
					  name, (unsigned long long)disp, (unsigned long long)(base ? pc - base : pc));
		}
		if (!add(n))
			break;
	}
#endif
}

// A minidump beside the report: every thread's stack and registers at the
// moment of the crash, which a debugger opens with the .pdb to show exactly
// where each thread was.
bool writeMinidump(EXCEPTION_POINTERS *info)
{
	if (!g_dumpPath[0])
		return false;
	HANDLE f = CreateFileW(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return false;
	MINIDUMP_EXCEPTION_INFORMATION mei;
	mei.ThreadId = GetCurrentThreadId();
	mei.ExceptionPointers = info;
	mei.ClientPointers = FALSE;
	const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
					  MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
					  info ? &mei : nullptr, nullptr, nullptr);
	CloseHandle(f);
	return ok == TRUE;
}

// The reason, the stack of the thread that died, and where the dump went.
const char *withStack(const char *reason, const CONTEXT *ctx, EXCEPTION_POINTERS *info)
{
	std::snprintf(g_reportBuf, sizeof(g_reportBuf), "%s\n", reason ? reason : "(no description)");
	if (ctx)
		appendStack(g_reportBuf, sizeof(g_reportBuf), ctx);
	if (writeMinidump(info)) {
		const size_t len = std::strlen(g_reportBuf);
		char path[1100] = {0};
		WideCharToMultiByte(CP_UTF8, 0, g_dumpPath, -1, path, int(sizeof(path)) - 1, nullptr, nullptr);
		std::snprintf(g_reportBuf + len, sizeof(g_reportBuf) - len, "\nCrash dump: %s\n", path);
	}
	return g_reportBuf;
}
#endif

void handleSignal(int sig)
{
#if defined(_WIN32)
	// abort() -- a failed Qt check (qFatal) or a C++ library assertion --
	// arrives here rather than in the SEH filter: take the stack as it is.
	CONTEXT ctx;
	RtlCaptureContext(&ctx);
	CrashGuard::writeReport(withStack(CrashGuard::describeSignal(sig), &ctx, nullptr));
#else
	CrashGuard::writeReport(CrashGuard::describeSignal(sig));
#endif
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
#if defined(_WIN32)
	CONTEXT ctx;
	RtlCaptureContext(&ctx);
	CrashGuard::writeReport(withStack(buf, &ctx, nullptr));
#else
	CrashGuard::writeReport(buf);
#endif
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
	// Not for a stack overflow: walking a stack that has no room left is
	// how a handler crashes inside itself. The dump still has it.
	const bool walk = code != EXCEPTION_STACK_OVERFLOW && info && info->ContextRecord;
	CrashGuard::writeReport(withStack(buf, walk ? info->ContextRecord : nullptr, info));
	// The immediate dialog, as asked for. Native MessageBoxA, never Qt: the
	// process is dying and Qt's own state may be the casualty. SYSTEMMODAL so
	// it surfaces even over a full-screen recording target.
	char msg[768];
	std::snprintf(msg, sizeof(msg),
		      HARPIA_APP_DISPLAY_NAME " has crashed.\n\n%s\n\nA crash report was saved and will be shown "
		      "on the next start. Any in-progress recording can be recovered there too.",
		      buf);
	MessageBoxA(nullptr, msg, HARPIA_APP_DISPLAY_NAME " crashed", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
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
#if defined(_WIN32)
	// The dump's path and the symbols, now, while allocating is still safe.
	// Deferred loads: a module's .pdb is read only if a crash walks into it.
	{
		const QString dump =
			QDir::toNativeSeparators(QString::fromStdString(reportDir) + QStringLiteral("/crash-latest.dmp"));
		constexpr int kCap = int(sizeof(g_dumpPath) / sizeof(wchar_t));
		if (dump.size() < kCap) { // toWCharArray does not stop at the buffer's end
			const int n = dump.toWCharArray(g_dumpPath);
			g_dumpPath[std::min(n, kCap - 1)] = 0;
		}
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
		g_symsReady = SymInitialize(GetCurrentProcess(), nullptr, TRUE) == TRUE;
	}
#endif

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
