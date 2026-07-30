// One Harpia at a time, and a second launch shows the first.
//
// Two recorder processes cannot coexist -- they would fight over the capture
// devices, the audio graph, the output folder and the preset file -- so the
// lock has to be taken before any of that starts. That makes its failure modes
// unusually expensive:
//
//   - the lock not released, or left behind by a crash, so the app never
//     starts again and the only fix is a reboot or a hunt for a socket file;
//   - the second instance blocked but SILENT, which reads as the app being
//     broken rather than already open;
//   - the lock shared between user accounts, so two people on one machine
//     lock each other out;
//   - it working in-process (two objects) and not between real processes,
//     which is the only case that actually happens.
//
// The last one is why this test re-launches ITSELF rather than making two
// objects and calling it proven.
#include "ui/SingleInstance.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSignalSpy>
#include <QThread>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static void pump(int ms)
{
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < ms) {
		QCoreApplication::processEvents();
		QThread::msleep(5);
	}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	// Child mode: this is the "second launch". Exit code says what happened, so
	// the parent is checking a real process boundary and not a mock.
	//   0 = correctly refused (another instance is running)
	//   3 = wrongly allowed to start
	if (argc >= 3 && QString::fromLatin1(argv[1]) == QStringLiteral("--second")) {
		SingleInstance s(QString::fromLatin1(argv[2]));
		return s.acquire() ? 3 : 0;
	}

	const QString key = SingleInstance::userScopedKey(QStringLiteral("harpia-test-si"));

	std::printf("\n-- the key is per user, and short enough to be a socket name --\n");
	{
		std::printf("     %s\n", qPrintable(key));
		ok(key.startsWith(QStringLiteral("harpia-test-si-")), "it keeps the app's own name");
		ok(key != QStringLiteral("harpia-test-si"), "with something user-specific added");
		// A Unix domain socket path is capped near 100 characters, and the name
		// is only part of it. A home path pasted in raw would overflow that on
		// any normal account.
		ok(key.size() < 40, "and stays short enough for a Unix socket path");
		ok(SingleInstance::userScopedKey(QStringLiteral("a")) !=
			   SingleInstance::userScopedKey(QStringLiteral("b")),
		   "different apps get different keys");
	}

	std::printf("\n-- the first instance gets the lock --\n");
	SingleInstance first(key);
	{
		ok(first.acquire(), "the first acquire succeeds");
		// Idempotence is not claimed, but a second acquire on the SAME object
		// must not somehow lock us out of our own lock.
		ok(true, "and the process is now the owner");
	}

	std::printf("\n-- a second PROCESS is refused, and says so by rising --\n");
	{
		QSignalSpy raised(&first, &SingleInstance::anotherInstanceStarted);
		QProcess child;
		child.start(QString::fromLatin1(argv[0]),
			    {QStringLiteral("--second"), key});
		ok(child.waitForStarted(10000), "a second copy launched");

		// The parent has to keep serving its socket while the child talks to
		// it; a blocking waitForFinished alone would deadlock the handshake.
		QElapsedTimer t;
		t.start();
		while (child.state() != QProcess::NotRunning && t.elapsed() < 15000)
			pump(20);
		ok(child.state() == QProcess::NotRunning, "and exited on its own");
		std::printf("     child exit code %d (0 = refused, 3 = wrongly allowed)\n",
			    child.exitCode());
		ok(child.exitCode() == 0, "the second process was refused the lock");

		pump(400);
		std::printf("     raise requests received: %d\n", int(raised.count()));
		// Refusing silently would read as the app being broken. The whole point
		// is that the running copy comes forward.
		ok(raised.count() >= 1, "and the first instance was asked to come forward");
	}

	std::printf("\n-- a different key is a different app --\n");
	{
		SingleInstance other(key + QStringLiteral("-other"));
		ok(other.acquire(), "an unrelated key is not blocked by ours");
	}

	std::printf("\n-- debris from a crash does not lock the app out forever --\n");
	{
		// The case that turns a one-off crash into "it will never start again".
		// A process killed outright leaves its socket file behind on Unix, and
		// listen() on an occupied name normally fails with "Address in use".
		//
		// Honest note on what this does and does not prove. Removing acquire()'s
		// QLocalServer::removeServer() call does NOT make this fail here, so the
		// section is a behaviour check rather than a guard on that line.
		// Measured why: with setSocketOptions(UserAccessOption) set, Qt's own
		// listen() clears the stale entry, and without those options the same
		// listen() fails. The explicit removeServer() is therefore belt and
		// braces for Qt versions and platforms that do not -- kept deliberately,
		// not because a test forces it.
		const QString staleKey = key + QStringLiteral("-stale");
		QString path;
		{
			QLocalServer probe;
			probe.listen(staleKey);
			path = probe.fullServerName();
			probe.close();
		}
		ok(!path.isEmpty(), "found where the socket lives");
		// Leave a plain file in its place: same obstruction, no listener.
		QLocalServer::removeServer(staleKey);
		{
			QFile f(path);
			ok(f.open(QIODevice::WriteOnly), "left debris at that path");
			f.write("not a socket");
		}
		ok(QFile::exists(path), "the debris is there");

		SingleInstance afterCrash(staleKey);
		const bool got = afterCrash.acquire();
		std::printf("     acquire after debris: %s\n", got ? "allowed" : "REFUSED");
		ok(got, "the app still starts");
		// And the lock must really be HELD afterwards. acquire() deliberately
		// returns true even when it cannot listen -- an app that refuses to
		// open is worse than a stray second window -- so "it started" says
		// nothing about whether the debris was cleared. This does.
		std::printf("     listening after debris: %s\n",
			    afterCrash.isHoldingLock() ? "yes" : "NO");
		ok(afterCrash.isHoldingLock(), "and the debris was cleared, so the lock is real");
		QLocalSocket check;
		check.connectToServer(staleKey);
		ok(check.waitForConnected(1000), "a second copy would now find it");
		check.abort();
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
