// Recording guidance must distinguish bits from bytes and keep Auto honest:
// numeric CBR targets can estimate storage; quality-based Auto cannot.
#include "ui/RecordingBitrateGuide.hpp"

#include <QCoreApplication>
#include <QLocale>
#include <QSet>

#include <cstdio>

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
	QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	struct Example {
		int kbps;
		qint64 bytesPerMinute;
		const char *size;
	};
	const Example listed[] = {
		{4000, 30000000, "30 MB per minute / 1.8 GB per hour"},
		{6000, 45000000, "45 MB per minute / 2.7 GB per hour"},
		{8000, 60000000, "60 MB per minute / 3.6 GB per hour"},
		{10000, 75000000, "75 MB per minute / 4.5 GB per hour"},
		{12000, 90000000, "90 MB per minute / 5.4 GB per hour"},
		{16000, 120000000, "120 MB per minute / 7.2 GB per hour"},
		{20000, 150000000, "150 MB per minute / 9 GB per hour"},
		{30000, 225000000, "225 MB per minute / 13.5 GB per hour"},
		{40000, 300000000, "300 MB per minute / 18 GB per hour"},
		{50000, 375000000, "375 MB per minute / 22.5 GB per hour"},
		{80000, 600000000, "600 MB per minute / 36 GB per hour"},
		{100000, 750000000, "750 MB per minute / 45 GB per hour"},
	};
	QSet<QString> uses;
	for (const Example &example : listed) {
		const QString guide = recordingBitrateGuide(example.kbps);
		const QString use = recordingBitrateUse(example.kbps);
		std::printf("\n-- %d kbps --\n", example.kbps);
		ok(recordingVideoBytesPerMinute(example.kbps) == example.bytesPerMinute,
		   "the bitrate gives the expected decimal video bytes per minute");
		ok(guide.contains(QString::fromLatin1(example.size)),
		   "the selected rate shows correct per-minute and per-hour sizes");
		ok(!use.isEmpty() && guide.contains(use), "the selected rate explains a recording use case");
		uses.insert(use);
	}
	ok(uses.size() == int(sizeof(listed) / sizeof(listed[0])),
	   "every listed bitrate offers distinct guidance");

	std::printf("\n-- custom values and their range limits --\n");
	const Example custom[] = {
		{500, 3750000, "3.75 MB per minute / 0.225 GB per hour"},
		{7500, 56250000, "56.25 MB per minute / 3.375 GB per hour"},
		{500000, 3750000000LL, "3,750 MB per minute / 225 GB per hour"},
	};
	for (const Example &example : custom) {
		ok(recordingVideoBytesPerMinute(example.kbps) == example.bytesPerMinute,
		   "custom bitrate arithmetic stays accurate, including beyond 32-bit file sizes");
		const QString guide = recordingBitrateGuide(example.kbps);
		ok(guide.contains(QString::fromLatin1(example.size)) &&
			   guide.contains(recordingBitrateUse(example.kbps)),
		   "custom values receive matching size estimates and use-case guidance");
	}

	std::printf("\n-- Auto has no fixed size target --\n");
	const QString automatic = recordingBitrateGuide(0);
	ok(automatic.contains(QStringLiteral("quality-based")) &&
		   automatic.contains(QStringLiteral("cannot be predicted")),
	   "Auto describes quality-based recording and its unpredictable file size");
	ok(!automatic.contains(QStringLiteral("MB per minute")) &&
		   !automatic.contains(QStringLiteral("GB per hour")),
	   "Auto does not present a made-up fixed storage estimate");
	ok(recordingVideoBytesPerMinute(0) == 0 && recordingVideoBytesPerMinute(-1) == 0,
	   "Auto and an unresolved custom choice do not produce negative size estimates");

	std::printf("\n-- estimates state their scope and quality tradeoff --\n");
	const QString guide = recordingBitrateGuide(20000);
	ok(guide.contains(QStringLiteral("Video-only")) && guide.contains(QStringLiteral("decimal MB/GB")) &&
		   !guide.contains(QStringLiteral("MiB")),
	   "decimal video-only units are explicit");
	ok(guide.contains(QStringLiteral("Audio")) && guide.contains(QStringLiteral("container overhead")),
	   "audio and container overhead are described as additional storage");
	ok(guide.contains(QStringLiteral("H.264 starting points")) &&
		   guide.contains(QStringLiteral("HEVC/AV1")) &&
		   guide.contains(QStringLiteral("same fixed bitrate")),
	   "quality recommendations identify codec assumptions without promising a size reduction at fixed bitrate");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
