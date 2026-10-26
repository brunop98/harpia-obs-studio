// Discord's limit applies to the final MP4, including its headers. Real
// size-limited files must decode independently, retain every video frame and
// audio packet, and never leave a partial set on cancellation or failure.
#include "core/ShareExporter.hpp"
#include "core/ShareSplitter.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

static QByteArray command(const QString &program, const QStringList &args)
{
	QProcess process;
	process.start(program, args);
	if (!process.waitForFinished(120000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
		std::printf("  FAIL %s: %s\n", qPrintable(program), process.readAllStandardError().constData());
		++failures;
		return {};
	}
	return process.readAllStandardOutput();
}

static QJsonObject probe(const QString &path)
{
	return QJsonDocument::fromJson(command(QStringLiteral("ffprobe"),
		{"-v", "error", "-count_packets", "-show_streams", "-show_format", "-of", "json", path})).object();
}

static QStringList frameHashes(const QString &path)
{
	const QByteArray text = command(QStringLiteral("ffmpeg"),
		{"-v", "error", "-i", path, "-map", "0:v:0", "-fps_mode", "passthrough", "-f", "framemd5", "-"});
	QStringList hashes;
	for (const QByteArray &line : text.split('\n'))
		if (!line.isEmpty() && !line.startsWith('#'))
			hashes.append(QString::fromLatin1(line.split(',').last().trimmed()));
	return hashes;
}

static QByteArray fileHash(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};
	QCryptographicHash hash(QCryptographicHash::Sha256);
	hash.addData(&file);
	return hash.result();
}

static qint64 audioPackets(const QJsonObject &info)
{
	for (const QJsonValue value : info.value("streams").toArray()) {
		const QJsonObject stream = value.toObject();
		if (stream.value("codec_type").toString() == QStringLiteral("audio"))
			return stream.value("nb_read_packets").toString().toLongLong();
	}
	return 0;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	QTemporaryDir work(argc > 1 ? QString::fromLocal8Bit(argv[1]) + QStringLiteral("/shareparts-XXXXXX")
				   : QDir::tempPath() + QStringLiteral("/shareparts-XXXXXX"));
	ok(work.isValid(), "a fresh test folder is available");
	if (!work.isValid())
		return 1;
	const QString source = work.filePath(QStringLiteral("large.mp4"));
	// CBR filler makes a genuine >49 MB H.264 file quickly at a small frame
	// size. B-frames and AAC make timestamp resets and split-point sync matter.
	command(QStringLiteral("ffmpeg"), {"-y", "-v", "error", "-f", "lavfi", "-i",
		"testsrc2=size=320x180:rate=30:duration=10", "-f", "lavfi", "-i",
		"sine=frequency=700:sample_rate=48000:duration=10", "-c:v", "libx264", "-preset", "ultrafast",
		"-pix_fmt", "yuv420p", "-b:v", "60M", "-minrate", "60M", "-maxrate", "60M", "-bufsize", "60M",
		"-g", "30", "-bf", "2", "-x264-params", "nal-hrd=cbr:force-cfr=1:scenecut=0",
		"-c:a", "aac", "-b:a", "128k", "-shortest", source});
	ok(QFileInfo(source).size() > share::discordPartBytes, "the input really exceeds the 49,000,000-byte cap");
	if (failures)
		return 1;
	const QByteArray originalHash = fileHash(source);
	const QStringList originalFrames = frameHashes(source);
	ok(originalFrames.size() == 300, "all 300 original frames decode");
	const qint64 originalAudio = audioPackets(probe(source));
	ok(originalAudio > 0, "the source has audio packets to preserve");
	std::atomic<bool> cancel{false};
	const QString parts = work.filePath(QStringLiteral("parts"));
	QDir().mkdir(parts);

	std::printf("\n-- real 49 MB parts, with complete video and audio --\n");
	const auto split = share::splitMp4(source, parts, share::discordPartBytes, cancel);
	ok(split.ok && split.files.size() >= 2, "a large video becomes multiple MP4 parts");
	QStringList decoded;
	qint64 audioCount = 0;
	for (int i = 0; i < split.files.size(); ++i) {
		const QString &part = split.files[i];
		const qint64 size = QFileInfo(part).size();
		std::printf("     %s: %lld bytes\n", qPrintable(QFileInfo(part).fileName()), (long long)size);
		ok(size > 0 && size <= share::discordPartBytes, "the finalized MP4, including metadata, is within 49 MB");
		ok(QFileInfo(part).fileName() == QStringLiteral("part%1.mp4").arg(i + 1, 3, 10, QLatin1Char('0')),
		   "parts are numbered in playback order");
		decoded.append(frameHashes(part));
		const QJsonObject info = probe(part);
		audioCount += audioPackets(info);
		bool videoSeen = false, audioSeen = false;
		for (const QJsonValue value : info.value("streams").toArray()) {
			const QJsonObject stream = value.toObject();
			const QString type = stream.value("codec_type").toString();
			if (type == QStringLiteral("video")) {
				videoSeen = stream.value("codec_name").toString() == QStringLiteral("h264");
				ok(std::abs(stream.value("start_time").toString().toDouble()) < 0.001,
				   "each video starts at zero rather than its position in the original");
			} else if (type == QStringLiteral("audio")) {
				audioSeen = stream.value("codec_name").toString() == QStringLiteral("aac");
				ok(std::abs(stream.value("start_time").toString().toDouble()) < 0.04,
				   "audio starts in sync within one compressed audio packet");
			}
		}
		ok(videoSeen && audioSeen, "each part has playable H.264 video and AAC audio");
	}
	ok(decoded == originalFrames, "all parts together contain every original video frame, in order, without duplicates");
	ok(audioCount == originalAudio, "all original audio packets are preserved across the split points");
	ok(!split.files.isEmpty() && QFileInfo(split.files.last()).size() < share::discordPartBytes,
	   "the last part contains the smaller remainder");

	std::printf("\n-- cancellation, failures and existing files --\n");
	const QString canceled = work.filePath(QStringLiteral("canceled"));
	QDir().mkdir(canceled);
	const auto stopped = share::splitMp4(source, canceled, share::discordPartBytes, cancel,
		[&](int, qint64) { cancel.store(true); });
	ok(stopped.canceled && !stopped.ok && stopped.files.isEmpty(), "cancellation is reported after the first finished part");
	ok(QDir(canceled).entryList(QDir::Files).isEmpty(), "even previously completed parts are cleaned up on cancellation");
	cancel.store(false);
	const QByteArray firstPartHash = fileHash(split.files.value(0));
	const auto collision = share::splitMp4(source, parts, share::discordPartBytes, cancel);
	ok(!collision.ok && !collision.error.isEmpty(), "an existing part is never overwritten");
	ok(fileHash(split.files.value(0)) == firstPartHash, "the existing part is unchanged after a name collision");
	const QString tooSmall = work.filePath(QStringLiteral("too-small"));
	QDir().mkdir(tooSmall);
	const auto impossible = share::splitMp4(source, tooSmall, 1, cancel);
	ok(!impossible.ok && impossible.shorterGopNeeded && QDir(tooSmall).entryList(QDir::Files).isEmpty(),
	   "an oversized GOP cannot escape the size cap or leave a partial file");

	std::printf("\n-- the full sharing exporter compresses, splits and publishes a complete folder --\n");
	const auto defaults = ShareExporter::optionsFor(ShareExporter::Level::Discord);
	ok(defaults.maxPartBytes == 49'000'000, "the Discord menu's default is exactly 49 decimal MB");
	ShareExporter exporter;
	bool success = false, wasCanceled = false;
	int finished = 0;
	QString error;
	QObject::connect(&exporter, &ShareExporter::finished, [&](bool pass, bool stop, const QString &message) {
		success = pass; wasCanceled = stop; error = message; ++finished;
	});
	auto options = defaults;
	options.maxPartBytes = 60'000; // exercise multiple parts without encoding a huge noisy video
	const QString folder = work.filePath(QStringLiteral("discord"));
	exporter.run(source, folder, options);
	if (!success)
		std::printf("     export error: %s\n", qPrintable(error));
	ok(success && !wasCanceled && finished == 1 && exporter.outputFiles().size() > 1,
	   "one success signal publishes a complete set of multiple parts");
	int frames = 0;
	for (const QString &part : exporter.outputFiles()) {
		ok(QFileInfo(part).exists() && QFileInfo(part).size() <= options.maxPartBytes,
		   "every compressed part obeys the configured cap");
		frames += frameHashes(part).size();
	}
	ok(frames == 300, "the sharing transcode and split retain the full video");
	const QStringList published = exporter.outputFiles();
	exporter.run(source, folder, options);
	ok(!success && exporter.outputFiles().isEmpty() && QFileInfo::exists(published.value(0)),
	   "reusing an existing folder fails safely and keeps its prior parts");
	bool retried = false;
	QObject::connect(&exporter, &ShareExporter::statusChanged, [&](const QString &message) {
		retried = retried || message.contains(QStringLiteral("smaller video frame groups"));
	});
	options.maxPartBytes = 1;
	const QString impossibleFolder = work.filePath(QStringLiteral("impossible-export"));
	exporter.run(source, impossibleFolder, options);
	ok(retried && !success && !error.isEmpty() && !QFileInfo::exists(impossibleFolder),
	   "an oversized GOP triggers the independent-frame retry, then fails safely if even one frame cannot fit");
	const QString canceledFolder = work.filePath(QStringLiteral("cancel-export"));
	QObject::connect(&exporter, &ShareExporter::statusChanged, &exporter, [&](const QString &) { exporter.cancel(); });
	exporter.run(source, canceledFolder, defaults);
	ok(!success && wasCanceled && !QFileInfo::exists(canceledFolder), "a canceled export publishes no partial folder");
	ok(fileHash(source) == originalHash, "the original recording is byte-for-byte unchanged");
	ok(QDir(work.path()).entryList({".harpia-discord-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(),
	   "temporary optimized copies and staging folders are removed");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
