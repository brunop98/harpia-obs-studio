// Free space: the thresholds, the wording, and the one line on screen.
//
// Running out of disk mid-recording is this app's worst failure -- you cannot
// re-take the thing you were recording, and you find out afterwards. It is also
// entirely predictable beforehand. So the numbers that decide when to speak up
// are worth pinning down, and so is the wording, because a warning that does
// not say HOW MUCH is left is just an interruption.
//
// The second half is the folder. "Recorded to the wrong place" fails just as
// silently, so the line names the destination as well as the space.
#include "core/DiskSpace.hpp"

#include <QCoreApplication>
#include <QDir>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	constexpr qint64 MB = 1024 * 1024;
	constexpr qint64 GB = 1024 * MB;

	std::printf("\n-- where the lines are --\n");
	{
		ok(kLowDiskBytes == 2 * GB, "the warning threshold is 2 GB, as asked for");
		ok(diskLevelFor(10 * GB) == DiskLevel::Ok, "10 GB is fine");
		ok(diskLevelFor(2 * GB + 1) == DiskLevel::Ok, "just over 2 GB is fine");
		// "less than 2 GB", literally: exactly 2 GB is not less than 2 GB, so it
		// does not warn. Stated as a test because a boundary nobody wrote down
		// is a boundary that drifts.
		ok(diskLevelFor(2 * GB) == DiskLevel::Ok, "exactly 2 GB is not 'less than 2 GB'");
		ok(diskLevelFor(2 * GB - 1) == DiskLevel::Low, "one byte under is");
		ok(diskLevelFor(300 * MB) == DiskLevel::Low, "300 MB is low");
		ok(diskLevelFor(100 * MB) == DiskLevel::Critical, "100 MB is past low");
		ok(diskLevelFor(0) == DiskLevel::Critical, "an empty drive is critical");
		ok(diskLevelFor(-1) == DiskLevel::Unknown,
		   "and a drive that will not answer is UNKNOWN, not zero");
	}

	std::printf("\n-- it asks, and it says how much --\n");
	{
		ok(lowDiskPrompt({DiskLevel::Ok, 50 * GB}).isEmpty(),
		   "plenty of room: no question asked");
		ok(lowDiskPrompt({DiskLevel::Unknown, -1}).isEmpty(),
		   "an unreadable drive is not nagged about -- a guess would be worse than silence");

		const QString low = lowDiskPrompt({DiskLevel::Low, 1500 * MB});
		std::printf("     low: %s\n", qUtf8Printable(low.split(QLatin1Char('\n')).first()));
		ok(!low.isEmpty(), "under 2 GB does ask");
		ok(low.contains(QStringLiteral("1.5 GB")),
		   "and says how much is left -- a warning without the number is just noise");

		const QString crit = lowDiskPrompt({DiskLevel::Critical, 90 * MB});
		std::printf("     critical: %s\n", qUtf8Printable(crit.split(QLatin1Char('\n')).first()));
		ok(!crit.isEmpty(), "so does nearly-nothing");
		ok(crit.contains(QStringLiteral("90 MB")), "with its own number");
		ok(crit != low, "and it is worded differently -- the two situations are not the same");
	}

	std::printf("\n-- the sizes read the way a person would say them --\n");
	{
		ok(formatBytes(12345678901LL) == QStringLiteral("11.5 GB"), "11.5 GB");
		ok(formatBytes(2 * GB) == QStringLiteral("2.0 GB"), "2.0 GB, with the decimal that matters");
		ok(formatBytes(820 * MB) == QStringLiteral("820 MB"), "820 MB, whole");
		ok(formatBytes(1023 * 1024) == QStringLiteral("1023 KB"), "1023 KB");
		ok(formatBytes(-1) == QStringLiteral("unknown"), "and unknown stays unknown");
	}

	std::printf("\n-- the line on the recording screen --\n");
	{
		const QString folder = QDir::tempPath();
		const QString line = diskLine(folder, {DiskLevel::Ok, 40 * GB});
		std::printf("     %s\n", qUtf8Printable(line));
		ok(line.contains(QStringLiteral("40.0 GB")), "it states the free space");
		ok(line.contains(QDir::toNativeSeparators(folder)),
		   "AND the folder -- recording to the wrong place fails just as quietly");

		// No folder set is a different sentence, not a blank space and a dot.
		const QString none = diskLine(QString(), {});
		std::printf("     no folder -> %s\n", qUtf8Printable(none));
		ok(!none.isEmpty() && !none.contains(QStringLiteral("free")),
		   "with no folder it says so instead of quoting a number for nothing");

		// A drive that will not answer: still name the destination, since that
		// half is known and useful on its own.
		const QString unknown = diskLine(folder, {});
		std::printf("     unreadable drive -> %s\n", qUtf8Printable(unknown));
		ok(unknown.contains(QDir::toNativeSeparators(folder)),
		   "an unreadable drive still names the folder");
		ok(!unknown.contains(QStringLiteral("free")), "and claims nothing about space");
	}

	std::printf("\n-- against a real drive --\n");
	{
		// The pure functions above could all agree with each other and still be
		// wired to nothing, so this asks the actual filesystem.
		const DiskStatus st = diskStatusFor(QDir::tempPath());
		std::printf("     %s -> %s (%lld bytes)\n", qUtf8Printable(QDir::tempPath()),
			    qUtf8Printable(formatBytes(st.freeBytes)), (long long)st.freeBytes);
		ok(st.isValid(), "a real folder reports a real level");
		ok(st.freeBytes > 0, "with a real number behind it");
		ok(diskStatusFor(QString()).level == DiskLevel::Unknown, "and an empty path reports nothing");
		// A path that is not there: whatever the platform reports for it, the
		// only requirement is that asking is safe and the answer is one of the
		// levels rather than a crash or a negative masquerading as a size.
		const DiskStatus missing = diskStatusFor(QStringLiteral("/definitely/not/here/xyzzy"));
		ok(missing.freeBytes >= 0 || missing.level == DiskLevel::Unknown,
		   "a nonexistent path answers safely rather than with a nonsense size");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
