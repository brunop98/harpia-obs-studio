// The file-analysis boundary is checked independently of musical beat tracking.
// detect() is deliberately replaced with a sample probe, so a detector's timing
// tolerance cannot conceal a trim, seek, timestamp-gap or channel-layout error.
// Link this fixture with BeatAudio.cpp (not BeatDetector.cpp), Qt Core and libav.
#include "editor/BeatDetector.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>

#include <cmath>
#include <cstdlib>
#include <cstdio>

namespace {
int failures = 0;
std::vector<float> delivered;
int deliveredRate = 0;
int deliveredChannels = 0;

void check(bool condition, const char *message)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}

bool writePulseWav(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return false;
	constexpr int rate = 44100;
	constexpr int seconds = 20;
	const auto u16 = [&](quint16 value) {
		char bytes[2];
		qToLittleEndian(value, reinterpret_cast<uchar *>(bytes));
		return file.write(bytes, sizeof(bytes)) == sizeof(bytes);
	};
	const auto u32 = [&](quint32 value) {
		char bytes[4];
		qToLittleEndian(value, reinterpret_cast<uchar *>(bytes));
		return file.write(bytes, sizeof(bytes)) == sizeof(bytes);
	};
	const quint32 dataBytes = rate * seconds * 4;
	if (file.write("RIFF", 4) != 4 || !u32(36 + dataBytes) || file.write("WAVEfmt ", 8) != 8 ||
	    !u32(16) || !u16(1) || !u16(2) || !u32(rate) || !u32(rate * 4) || !u16(4) ||
	    !u16(16) || file.write("data", 4) != 4 || !u32(dataBytes))
		return false;
	QByteArray block(rate * 2, '\0'); // one half-second, in stereo
	for (int index = 0; index < rate / 2; ++index) {
		const double t = double(index) / rate;
		const double pulse = t < 0.06 ? 0.7 * std::sin(2.0 * 3.141592653589793 * 150.0 * t) * std::exp(-90.0 * t) : 0.0;
		const qint16 value = qint16(std::lround(pulse * 32767.0));
		qToLittleEndian(value, reinterpret_cast<uchar *>(block.data() + index * 4));
		qToLittleEndian(value, reinterpret_cast<uchar *>(block.data() + index * 4 + 2));
	}
	for (int index = 0; index < seconds * 2; ++index)
		if (file.write(block) != block.size())
			return false;
	return file.flush();
}

bool ffmpeg(QStringList arguments)
{
	QProcess process;
	arguments.prepend(QStringLiteral("error"));
	arguments.prepend(QStringLiteral("-v"));
	arguments.prepend(QStringLiteral("-hide_banner"));
	process.start(QStringLiteral("ffmpeg"), arguments);
	const bool completed = process.waitForFinished(20000);
	if (!completed)
		process.kill();
	if (!completed || process.exitCode() != 0) {
		std::fprintf(stderr, "%s\n", process.readAllStandardError().constData());
		return false;
	}
	return true;
}
} // namespace

namespace harpia::beat_detection {
Result detect(const std::vector<float> &pcm, int sampleRate, int channels, const Options &,
	      std::atomic<bool> *)
{
	delivered = pcm;
	deliveredRate = sampleRate;
	deliveredChannels = channels;
	Result result;
	for (size_t index = 0; index < pcm.size(); ++index) {
		if (std::abs(pcm[index]) > 0.1f) {
			result.beatsMs.append(qint64(index) * 1000 / sampleRate / channels);
			index += size_t(sampleRate / 4) * channels;
		}
	}
	return result;
}
} // namespace harpia::beat_detection

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication application(argc, argv);
	QTemporaryDir directory;
	using namespace harpia::beat_detection;
	check(directory.isValid(), "private source directory exists");
	if (!directory.isValid())
		return 1;
	const QString source = QDir(directory.path()).filePath(QString::fromUtf8("música.wav"));
	check(writePulseWav(source), "stereo PCM source with independently known pulse times is written");
	auto result = analyzeFile(source, 0, 4000);
	check(result.error.isEmpty() && !result.canceled && result.beatsMs.size() == 8,
	      "source audio is converted only through the requested interval");
	check(deliveredRate == 22050 && deliveredChannels == 1 && delivered.size() <= 22050 * 4 &&
	      delivered.size() > 22050 * 3,
	      "analysis uses bounded low-rate mono instead of retaining the whole source");
	result = analyzeFile(source, 10500, 14500);
	check(result.error.isEmpty() && result.beatsMs.size() == 8 && result.beatsMs.first() < 10,
	      "seeking to a later trimmed interval places its first pulse at the clip origin");
	const QString compressed = QDir(directory.path()).filePath(QStringLiteral("pulses.m4a"));
	const bool madeCompressed = ffmpeg({QStringLiteral("-i"), source, QStringLiteral("-c:a"),
		QStringLiteral("aac"), QStringLiteral("-y"), compressed});
	check(madeCompressed, "compressed music fixture with encoder delay is encoded");
	if (madeCompressed) {
		result = analyzeFile(compressed, 10500, 14500);
		check(result.error.isEmpty() && result.beatsMs.size() == 8 && result.beatsMs.first() < 15,
		      "seeking compressed music preserves trim alignment despite encoder delay");
	}
	const QString delayed = QDir(directory.path()).filePath(QStringLiteral("delayed.mkv"));
	const bool madeDelayed = ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
		QStringLiteral("color=size=16x16:rate=5:duration=10"), QStringLiteral("-itsoffset"), QStringLiteral("1"),
		QStringLiteral("-i"), source, QStringLiteral("-t"), QStringLiteral("10"), QStringLiteral("-c:v"),
		QStringLiteral("mpeg4"), QStringLiteral("-c:a"), QStringLiteral("pcm_s16le"), QStringLiteral("-y"), delayed});
	check(madeDelayed, "video with an audio stream delayed by one second is muxed");
	if (madeDelayed) {
		result = analyzeFile(delayed, 0, 8000);
		check(result.error.isEmpty() && result.beatsMs.size() == 14 &&
		      std::llabs(result.beatsMs.first() - 1000) <= 10,
		      "initial stream delay is preserved relative to the video's start");
		result = analyzeFile(delayed, 3000, 7000);
		check(result.error.isEmpty() && result.beatsMs.size() == 8 && result.beatsMs.first() < 10,
		      "seeked delayed-stream audio is aligned to the requested source range");
		const QString silent = QDir(directory.path()).filePath(QStringLiteral("no-audio.mkv"));
		const bool madeSilent = ffmpeg({QStringLiteral("-i"), delayed, QStringLiteral("-map"),
			QStringLiteral("0:v:0"), QStringLiteral("-c:v"), QStringLiteral("copy"), QStringLiteral("-an"),
			QStringLiteral("-y"), silent});
		check(madeSilent, "a video without audio is muxed");
		if (madeSilent) {
			result = analyzeFile(silent, 0, 1000);
			check(!result.error.isEmpty() && result.beatsMs.isEmpty(),
			      "media without an audio stream reports a useful error and no markers");
		}
	}
	const QString gap = QDir(directory.path()).filePath(QStringLiteral("gap.mkv"));
	const bool madeGap = ffmpeg({QStringLiteral("-i"), source, QStringLiteral("-af"),
		QStringLiteral("aselect=not(between(t\\,2\\,4))"), QStringLiteral("-t"), QStringLiteral("10"),
		QStringLiteral("-c:a"), QStringLiteral("pcm_s16le"), QStringLiteral("-y"), gap});
	check(madeGap, "audio with a two-second timestamp gap is muxed");
	if (madeGap) {
		result = analyzeFile(gap, 0, 8000);
		bool gapIsQuiet = true;
		for (const auto at : result.beatsMs)
			gapIsQuiet &= at < 2200 || at > 3900;
		check(result.error.isEmpty() && !result.beatsMs.isEmpty() && gapIsQuiet && result.beatsMs.last() > 7400,
		      "timestamp gaps remain silent and later music is not shifted earlier");
	}
	std::atomic<bool> cancel{true};
	result = analyzeFile(source, 0, 1000, {}, &cancel);
	check(result.canceled && result.error.isEmpty() && result.beatsMs.isEmpty(),
	      "cancellation returns no partial result or misleading decode error");
	check(!analyzeFile(source + QStringLiteral(".missing"), 0, 1000).error.isEmpty(),
	      "missing media has an actionable error");
	check(!analyzeFile(source, -1, 1000).error.isEmpty() && !analyzeFile(source, 1000, 1000).error.isEmpty(),
	      "invalid source intervals are rejected before decoding");
	check(!analyzeFile(source, 0, 31 * 60 * 1000).error.isEmpty(),
	      "excessive analysis range is bounded before allocating PCM");
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
