// A real pause removed from linked camera/audio tracks must disappear in both
// the preview mix and exported MP4. Numbered frames and distinct tones expose
// source-clock mistakes that a model-only duration check cannot detect.
#include "editor/ClipExporter.hpp"
#include "editor/FrameSeeker.hpp"
#include "editor/SilenceTrim.hpp"
#include "editor/TimelineAudio.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace harpia;

static int failures = 0;
static constexpr int kRate = 48000;
static constexpr double kPi = 3.14159265358979323846;

static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

static QByteArray run(const QString &program, const QStringList &arguments, bool *success = nullptr)
{
	QProcess process;
	process.start(program, arguments);
	const bool started = process.waitForStarted(5000);
	const bool finished = started && process.waitForFinished(30000);
	if (!finished) {
		process.kill();
		process.waitForFinished(1000);
	}
	const bool ok = finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
	if (success)
		*success = ok;
	if (!ok)
		std::fprintf(stderr, "%s: %s\n", qPrintable(program), process.readAllStandardError().constData());
	return process.readAllStandardOutput();
}

static bool makeSource(const QString &path)
{
	// Frame N stores N in eight binary bands. Its audio is 440Hz / silence /
	// 880Hz / 1320Hz, one second each. AAC padding is deliberately included.
	bool success = false;
	run(QStringLiteral("ffmpeg"),
	    {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
	     QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
	     QStringLiteral("nullsrc=s=320x180:r=30:d=4,format=yuv420p,"
			    "geq=lum='16+219*mod(floor(N/pow(2\\,floor(X/40)))\\,2)':cb=128:cr=128"),
	     QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
	     QStringLiteral("aevalsrc=exprs='if(lt(t,1),0.2*sin(2*PI*440*t),"
			    "if(lt(t,2),0,if(lt(t,3),0.2*sin(2*PI*880*t),0.2*sin(2*PI*1320*t))))':s=48000:d=4"),
	     QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-crf"), QStringLiteral("12"),
	     QStringLiteral("-preset"), QStringLiteral("veryfast"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
	     QStringLiteral("-c:a"), QStringLiteral("aac"), QStringLiteral("-b:a"), QStringLiteral("128k"),
	     QStringLiteral("-shortest"), QStringLiteral("-y"), path}, &success);
	return success && QFileInfo(path).size() > 0;
}

static int frameNumber(const QImage &image)
{
	if (image.width() != 320 || image.height() != 180)
		return -1;
	const auto rgb = image.convertToFormat(QImage::Format_RGB32);
	int number = 0;
	for (int bit = 0; bit < 8; ++bit)
		if (qGray(rgb.pixel(bit * 40 + 20, 90)) > 128)
			number |= 1 << bit;
	return number;
}

static QByteArray fileHash(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};
	return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

static std::vector<float> decodeAudio(const QString &path)
{
	bool success = false;
	const QByteArray bytes = run(QStringLiteral("ffmpeg"),
	    {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"), path,
	     QStringLiteral("-map"), QStringLiteral("0:a:0"), QStringLiteral("-ac"), QStringLiteral("1"),
	     QStringLiteral("-ar"), QStringLiteral("48000"), QStringLiteral("-f"), QStringLiteral("f32le"),
	     QStringLiteral("-")}, &success);
	if (!success || bytes.isEmpty() || bytes.size() % int(sizeof(float)))
		return {};
	std::vector<float> samples(size_t(bytes.size()) / sizeof(float));
	std::memcpy(samples.data(), bytes.constData(), size_t(bytes.size()));
	return samples;
}

static double toneAt(const std::vector<float> &samples, int channels, qint64 milliseconds)
{
	const size_t first = size_t(milliseconds) * kRate / 1000;
	const size_t count = size_t(kRate * 0.12);
	if (milliseconds < 0 || channels < 1 || (first + count) * size_t(channels) > samples.size())
		return -1;
	double energy = 0;
	for (size_t i = 0; i < count; ++i) {
		const double sample = samples[(first + i) * size_t(channels)];
		energy += sample * sample;
	}
	if (std::sqrt(energy / count) < 0.03)
		return 0;
	double bestFrequency = 0, bestPower = 0;
	for (double frequency : {440.0, 880.0, 1320.0}) {
		const double coefficient = 2.0 * std::cos(2.0 * kPi * frequency / kRate);
		double previous = 0, older = 0;
		for (size_t i = 0; i < count; ++i) {
			const double value = samples[(first + i) * size_t(channels)] + coefficient * previous - older;
			older = previous;
			previous = value;
		}
		const double power = previous * previous + older * older - coefficient * previous * older;
		if (power > bestPower) {
			bestPower = power;
			bestFrequency = frequency;
		}
	}
	return bestFrequency;
}

static qint64 longestQuiet(const std::vector<float> &samples, int channels, qint64 duration)
{
	qint64 longest = 0, quiet = 0;
	const size_t window = kRate / 100;
	for (qint64 milliseconds = 300; milliseconds + 10 < duration - 300; milliseconds += 10) {
		const size_t first = size_t(milliseconds) * kRate / 1000;
		if ((first + window) * size_t(channels) > samples.size())
			break;
		double energy = 0;
		for (size_t i = 0; i < window; ++i) {
			const double sample = samples[(first + i) * size_t(channels)];
			energy += sample * sample;
		}
		quiet = std::sqrt(energy / window) < 0.005 ? quiet + 10 : 0;
		longest = std::max(longest, quiet);
	}
	return longest;
}

// Independent inverse of the reviewed deletion ranges, without reading the
// rewritten clips' in-points or speeds.
static qint64 originalTime(qint64 output, const QVector<TlSpan> &cuts)
{
	qint64 source = output, removed = 0;
	for (const auto &cut : cuts) {
		if (output < cut.fromMs - removed)
			break;
		source += cut.durationMs();
		removed += cut.durationMs();
	}
	return source;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);
	QTemporaryDir directory;
	const QString source = directory.filePath(QStringLiteral("numbered-and-spoken.mp4"));
	check(directory.isValid() && makeSource(source), "generated numbered video with an AAC tone/silence/tone soundtrack");
	if (!QFileInfo::exists(source))
		return 2;
	const QByteArray originalHash = fileHash(source);
	TimelineModel original;
	TlTrack camera;
	camera.name = QStringLiteral("Camera");
	TlClip clip;
	clip.sourceId = 1;
	clip.srcEndMs = 4000;
	clip.volume = 0; // a separate linked audio track supplies the soundtrack
	clip.link = 19;
	camera.clips = {clip};
	original.tracks.append(camera);
	TlTrack voice;
	voice.kind = TlTrack::Kind::Audio;
	voice.name = QStringLiteral("Voice");
	clip.volume = 1;
	voice.clips = {clip};
	original.tracks.append(voice);
	const auto lookup = [source](int id) { return id == 1 ? source : QString(); };
	const auto originalPcm = TimelineAudio::mixToBuffer(original, lookup);
	check(!originalPcm.empty() && longestQuiet(originalPcm, 2, 4000) >= 800,
	      "the actual preview mix contains the intended long pause");
	const auto cuts = silence_trim::detect(originalPcm, TimelineAudio::kRate, TimelineAudio::kChannels);
	check(cuts.size() == 1 && cuts.first().fromMs >= 1000 && cuts.first().fromMs <= 1150 &&
	      cuts.first().toMs >= 1850 && cuts.first().toMs <= 2000,
	      "analysis finds the encoded pause while retaining room around both spoken edges");
	if (cuts.size() != 1)
		return 1;
	const auto edit = silence_trim::apply(original, cuts);
	if (!edit.error.isEmpty())
		std::fprintf(stderr, "%s\n", qPrintable(edit.error));
	check(edit.ok() && edit.model.durationMs() == 4000 - edit.removedMs,
	      "the linked camera and voice tracks accept the same reviewed removal");
	if (!edit.ok())
		return 1;
	const auto previewPcm = TimelineAudio::mixToBuffer(edit.model, lookup);
	check(!previewPcm.empty() && longestQuiet(previewPcm, 2, edit.model.durationMs()) <= 250,
	      "the trimmed preview mix has no leftover long pause");
	const QString output = directory.filePath(QStringLiteral("trimmed.mp4"));
	ClipExporter exporter;
	bool succeeded = false, wasCanceled = false;
	int finishes = 0;
	QString error;
	QObject::connect(&exporter, &ClipExporter::finished, [&](bool ok, bool canceled, const QString &message) {
		++finishes;
		succeeded = ok;
		wasCanceled = canceled;
		error = message;
	});
	ClipExporter::Options options;
	options.timeline = edit.model;
	options.timelineSources = {{1, source.toStdString()}};
	options.canvasW = 320;
	options.canvasH = 180;
	options.timelineFps = 30;
	options.keepAudio = true;
	options.videoCrf = 16;
	options.effort = ClipExporter::Options::Effort::Fast;
	exporter.run(source, output, options);
	if (!error.isEmpty())
		std::fprintf(stderr, "%s\n", qPrintable(error));
	check(finishes == 1 && succeeded && !wasCanceled && QFileInfo(output).size() > 0,
	      "the trimmed timeline exports once with video and mixed audio");
	if (!succeeded)
		return 1;
	bool probed = false;
	const auto metadata = QJsonDocument::fromJson(run(QStringLiteral("ffprobe"),
	    {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
	     QStringLiteral("format=duration"), QStringLiteral("-of"), QStringLiteral("json"), output}, &probed)).object();
	const double seconds = metadata.value(QStringLiteral("format")).toObject().value(QStringLiteral("duration")).toString().toDouble();
	check(probed && std::abs(seconds * 1000.0 - edit.model.durationMs()) < 60,
	      "the finished file duration matches the edited timeline within frame/AAC rounding");
	const auto exportedPcm = decodeAudio(output);
	check(!exportedPcm.empty() && longestQuiet(exportedPcm, 1, edit.model.durationMs()) <= 250,
	      "independently decoded exported audio has no leftover long pause");
	FrameSeeker frames;
	check(frames.open(output), "the finished MP4 video can be decoded");
	const qint64 afterJoinFrame = qint64(std::ceil((cuts.first().fromMs + 250) * 30.0 / 1000.0));
	const qint64 laterFrame = qint64(std::ceil(silence_trim::mapTime(3500, cuts) * 30.0 / 1000.0));
	for (const auto &sample : QVector<QPair<qint64, double>>{{12, 440.0}, {afterJoinFrame, 880.0}, {laterFrame, 1320.0}}) {
		const qint64 atMs = qint64(std::llround(sample.first * 1000.0 / 30.0));
		const qint64 originalMs = originalTime(atMs, cuts);
		const int expectedFrame = int(std::floor((originalMs + 1.0) * 30.0 / 1000.0));
		const int actualFrame = frameNumber(frames.frameAt(atMs, 320, 180));
		std::printf("    output %lldms maps to source %lldms: frame %d, expected %d\n",
			    static_cast<long long>(atMs), static_cast<long long>(originalMs), actualFrame, expectedFrame);
		check(actualFrame == expectedFrame, "the exported picture shows the expected source frame before/after the cut");
		check(toneAt(previewPcm, 2, atMs) == sample.second && toneAt(exportedPcm, 1, atMs) == sample.second,
		      "preview and exported sound match the source moment shown by the video");
	}
	check(fileHash(source) == originalHash, "silence trimming and exporting leave the original recording unchanged");
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
