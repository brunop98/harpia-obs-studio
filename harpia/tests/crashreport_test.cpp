// The crash report: written by a dying process, read by the next one.
//
// This is code that only ever runs at the worst possible moment, which makes
// it exactly the code that must be tested for real -- a handler with a typo
// is indistinguishable from no handler at all until the day it matters. So
// the heart of this test CRASHES REAL CHILD PROCESSES (this same binary,
// re-run with --crash-child) and reads the reports they leave behind:
//
//   * a segfault must produce a report that says "segmentation fault";
//   * an uncaught C++ exception must produce one carrying the exception's
//     own message -- "terminate called" with no reason is exactly the
//     mystery this feature exists to end;
//   * a CLEAN exit must leave nothing, or every start would cry crash;
//   * and the first description wins, because handlers cascade (abort inside
//     a signal handler) and the second one only names the fallout.
//
// The Windows SEH branch and the native MessageBox cannot run here; what is
// covered is the shared machinery -- install, write-once, pending/archive.
#include "core/CrashGuard.hpp"
#include "Version.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// The child side: install into the given dir, then die the requested way.
static int crashChild(const QString &dir, const QString &how)
{
	CrashGuard::install(dir.toStdString());
	if (how == QLatin1String("segv"))
		std::raise(SIGSEGV);
	else if (how == QLatin1String("throw"))
		throw std::runtime_error("the exact message the dialog should show");
	else if (how == QLatin1String("double")) {
		CrashGuard::writeReport("the real cause");
		CrashGuard::writeReport("the fallout");
		return 0; // clean exit; the report stays for the parent to read
	}
	return 0; // "clean": install handlers, crash not at all
}

// Run this binary as a crashing child; return its pending report ("" if none).
static QString runChild(const QString &how, bool *crashed = nullptr)
{
	QTemporaryDir tmp;
	QProcess p;
	p.start(QCoreApplication::applicationFilePath(),
		{QStringLiteral("--crash-child"), tmp.path(), how});
	p.waitForFinished(15000);
	if (crashed)
		*crashed = (p.exitStatus() == QProcess::CrashExit) || p.exitCode() != 0;
	QFile f(CrashGuard::pendingPath(tmp.path()));
	if (!f.open(QIODevice::ReadOnly))
		return QString();
	return QString::fromUtf8(f.readAll());
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	// Child mode: crash as instructed and never reach the checks below.
	const QStringList args = QCoreApplication::arguments();
	if (args.size() == 4 && args[1] == QLatin1String("--crash-child"))
		return crashChild(args[2], args[3]);

	std::printf("\n-- the descriptions are for people --\n");
	{
		const QString segv = QString::fromLatin1(CrashGuard::describeSignal(SIGSEGV));
		std::printf("     SIGSEGV -> %s\n", qUtf8Printable(segv));
		ok(segv.contains(QStringLiteral("memory"), Qt::CaseInsensitive),
		   "a segfault is explained, not numbered");
		ok(QString::fromLatin1(CrashGuard::describeSignal(SIGABRT))
			   .contains(QStringLiteral("internal check"), Qt::CaseInsensitive),
		   "and an abort says what an abort means");
	}

	std::printf("\n-- a real segfault leaves a real report --\n");
	{
		bool crashed = false;
		const QString report = runChild(QStringLiteral("segv"), &crashed);
		std::printf("     child crashed: %s; report %d bytes\n", crashed ? "yes" : "no",
			    int(report.size()));
		ok(crashed, "the child really died -- otherwise this test proves nothing");
		ok(!report.isEmpty(), "and a report was written on the way down");
		ok(report.contains(QStringLiteral("Segmentation"), Qt::CaseInsensitive),
		   "naming the segfault");
		ok(report.contains(QLatin1String(HARPIA_VERSION_STRING)),
		   "and the version, because 'which build?' is always the first question");
	}

	std::printf("\n-- an uncaught exception reports its own message --\n");
	{
		const QString report = runChild(QStringLiteral("throw"));
		std::printf("     report: %s\n",
			    qUtf8Printable(report.section(QLatin1Char('\n'), -2).trimmed()));
		ok(report.contains(QStringLiteral("the exact message the dialog should show")),
		   "the what() of the exception survives into the report");
	}

	std::printf("\n-- a clean exit leaves nothing --\n");
	{
		// The negative control. If installing the handlers alone produced a
		// report, every start would open with a crash dialog and the feature
		// would be worse than nothing.
		bool crashed = true;
		const QString report = runChild(QStringLiteral("clean"), &crashed);
		ok(!crashed, "the clean child exited cleanly");
		ok(report.isEmpty(), "and left no pending report behind");
	}

	std::printf("\n-- the first description wins --\n");
	{
		const QString report = runChild(QStringLiteral("double"));
		ok(report.contains(QStringLiteral("the real cause")), "the first write is kept");
		ok(!report.contains(QStringLiteral("the fallout")),
		   "the cascade that follows cannot overwrite it");
	}

	std::printf("\n-- pending is taken exactly once, and archived --\n");
	{
		QTemporaryDir tmp;
		CrashGuard::install(tmp.path().toStdString());
		// Write via the file rather than writeReport: the once-per-process
		// latch was spent by install-side tests? No -- writeReport was never
		// called in THIS process. Use it, which also covers the parent-side
		// write path.
		CrashGuard::writeReport("a crash from last session");
		const QString first = CrashGuard::takePendingReport(tmp.path());
		std::printf("     first take: %d bytes\n", int(first.size()));
		ok(first.contains(QStringLiteral("a crash from last session")),
		   "the report comes back on the next start");
		ok(CrashGuard::takePendingReport(tmp.path()).isEmpty(),
		   "and only once -- the second start is not haunted by the same crash");
		const QStringList archived =
			QDir(tmp.path()).entryList({QStringLiteral("crash-*.txt")}, QDir::Files);
		std::printf("     archived: %s\n", qUtf8Printable(archived.join(QLatin1Char(' '))));
		ok(archived.size() == 1 && archived.first() != QLatin1String("crash-pending.txt"),
		   "the report is archived, not destroyed -- 'same as last week?' stays answerable");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
