// Count pictures, not just container metadata: high-FPS exports must use every
// available retimed source frame, and hold slower sources on a steady clock.
#include "editor/ClipExporter.hpp"

#include <QGuiApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
void check(bool okay, const QString &description)
{
	std::printf("%s %s\n", okay ? "PASS" : "FAIL", description.toUtf8().constData());
	failures += !okay;
}
QByteArray command(const QString &program, const QStringList &arguments, bool *okay = nullptr)
{
	QProcess process;
	process.start(program, arguments);
	const bool good = process.waitForFinished(60000) && process.exitCode() == 0;
	if (okay)
		*okay = good;
	if (!good)
		std::printf("%s failed: %s\n", qPrintable(program), process.readAllStandardError().constData());
	return process.readAllStandardOutput();
}
bool counter(const QString &path, int offset = 0, bool wholeFrameClock = false)
{
	QStringList arguments = {"-v", "error", "-f", "lavfi", "-i",
		QStringLiteral("nullsrc=s=64x64:r=30:d=4,format=yuv420p,"
			"geq=lum='16+219*mod(floor((N+%1)/pow(2\\,floor(X/8)))\\,2)':cb=128:cr=128").arg(offset)};
	if (!wholeFrameClock)
		arguments << "-f" << "lavfi" << "-i" << "sine=frequency=440:sample_rate=48000:duration=4";
	arguments << "-c:v" << (wholeFrameClock ? "ffv1" : "libx264") << "-g" << "1";
	if (!wholeFrameClock)
		arguments << "-crf" << "8" << "-c:a" << "aac" << "-b:a" << "128k";
	arguments << "-y" << path;
	bool okay = false;
	command("ffmpeg", arguments, &okay);
	return okay && QFileInfo::exists(path);
}
bool exportVideo(const QString &source, const QString &output, const ClipExporter::Options &options)
{
	ClipExporter exporter;
	bool done = false, good = false;
	QObject::connect(&exporter, &ClipExporter::finished, [&](bool okay, bool canceled, const QString &error) {
		done = true;
		good = okay && !canceled;
		if (!good)
			std::printf("export failed: %s\n", qPrintable(error));
	});
	exporter.run(source, output, options);
	return done && good;
}
QVector<int> frames(const QString &path)
{
	bool okay = false;
	const QByteArray raw = command("ffmpeg", {"-v", "error", "-i", path, "-map", "0:v:0", "-vf", "scale=64:64",
		"-vsync", "0", "-pix_fmt", "gray", "-f", "rawvideo", "pipe:1"}, &okay);
	QVector<int> result;
	if (!okay || raw.size() % (64 * 64))
		return result;
	for (qsizetype at = 0; at < raw.size(); at += 64 * 64) {
		int number = 0;
		for (int bit = 0; bit < 8; ++bit)
			if (static_cast<unsigned char>(raw[at + 32 * 64 + bit * 8 + 4]) > 128)
				number |= 1 << bit;
		result.push_back(number);
	}
	return result;
}
QJsonObject probe(const QString &path, const QString &selector, const QString &entries)
{
	QStringList arguments = {"-v", "error"};
	if (!selector.isEmpty())
		arguments << "-select_streams" << selector;
	arguments << "-show_entries" << entries << "-of" << "json" << path;
	return QJsonDocument::fromJson(command("ffprobe", arguments)).object();
}
QVector<int> expected(double start, double speed, double fps, double seconds, int offset = 0)
{
	QVector<int> result;
	for (int i = 0; double(i) / fps < seconds - 1e-9; ++i)
		result.push_back(offset + int(std::floor((start + double(i) / fps * speed) * 30.0 + 1e-6)));
	return result;
}
void inspect(const QString &path, const QVector<int> &want, double fps, const QString &name, bool audio = false)
{
	const QVector<int> got = frames(path);
	if (got != want) {
		std::printf("%s: %d frames, wanted %d\n", qPrintable(name), int(got.size()), int(want.size()));
		for (int i = 0, printed = 0; i < std::min(got.size(), want.size()) && printed < 5; ++i)
			if (got[i] != want[i]) {
				std::printf("  frame %d: got %d, wanted %d\n", i, got[i], want[i]);
				++printed;
			}
	}
	check(got == want, name + QStringLiteral(": correct source frame at every output tick"));
	const QJsonObject metadata = probe(path, "v:0", "stream=avg_frame_rate,duration:frame=best_effort_timestamp_time");
	const QJsonObject stream = metadata["streams"].toArray().at(0).toObject();
	const QStringList rate = stream["avg_frame_rate"].toString().split('/');
	const double actualFps = rate.value(1).toDouble() > 0 ? rate.value(0).toDouble() / rate.value(1).toDouble() : 0.0;
	const QJsonArray timestamps = metadata["frames"].toArray();
	bool steady = timestamps.size() == want.size() && std::abs(actualFps - fps) < 0.001;
	double last = -1.0;
	for (const QJsonValue &value : timestamps) {
		const double pts = value.toObject()["best_effort_timestamp_time"].toString().toDouble();
		if (last >= 0.0)
			steady &= std::abs(pts - last - 1.0 / fps) < 0.00001;
		last = pts;
	}
	const double duration = stream["duration"].toString().toDouble();
	check(steady && std::abs(duration - double(want.size()) / fps) < 0.0001,
	      name + QStringLiteral(": exact CFR timestamps, frame count and video duration"));
	if (audio) {
		const QJsonArray streams = probe(path, "a:0", "stream=duration")["streams"].toArray();
		const double audioDuration = streams.at(0).toObject()["duration"].toString().toDouble();
		check(!streams.isEmpty() && std::abs(audioDuration - duration) < 0.075,
		      name + QStringLiteral(": audio retains the same timeline duration"));
	}
}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);
	QTemporaryDir dir;
	const QString first = dir.filePath("first.mp4"), second = dir.filePath("second.mp4");
	const QString coarse = dir.filePath("coarse.mkv"), whole = dir.filePath("whole.avi");
	check(counter(first) && counter(second, 128) && counter(whole, 0, true),
	      QStringLiteral("made numbered 30fps MP4/AAC sources and an all-keyframe 1/30-clock AVI"));
	bool remuxed = false;
	command("ffmpeg", {"-v", "error", "-i", first, "-map", "0:v:0", "-an", "-c:v", "copy", "-y", coarse}, &remuxed);
	check(remuxed && probe(coarse, "v:0", "stream=time_base")["streams"].toArray().at(0).toObject()["time_base"] == "1/1000" &&
	      probe(whole, "v:0", "stream=time_base")["streams"].toArray().at(0).toObject()["time_base"] == "1/30",
	      QStringLiteral("coarse fixtures use real 1/1000 and whole-frame 1/30 container clocks"));
	ClipExporter::Options base;
	base.keepAudio = false;
	base.effort = ClipExporter::Options::Effort::Fast;
	base.videoCrf = 8;
	int serial = 0;
	auto run = [&](const QString &name, const QString &input, ClipExporter::Options options,
		       const QVector<int> &want, bool audio = false) {
		const QString output = dir.filePath(QStringLiteral("output%1.mp4").arg(++serial));
		check(exportVideo(input, output, options), name + QStringLiteral(": exported successfully"));
		inspect(output, want, options.outFps, name, audio);
	};
	for (double fps : {60.0, 120.0}) {
		ClipExporter::Options options = base;
		options.outFps = fps;
		options.keepAudio = true;
		run(QStringLiteral("normal 30fps at %1fps").arg(fps), first, options, expected(0, 1, fps, 4), true);
		options.keepAudio = false;
		options.speed = fps / 30.0;
		run(QStringLiteral("30fps sped %1x at %2fps").arg(options.speed).arg(fps), first, options,
		    expected(0, options.speed, fps, 4 / options.speed));
		run(QStringLiteral("coarse MKV sped %1x at %2fps").arg(options.speed).arg(fps), coarse, options,
		    expected(0, options.speed, fps, 4 / options.speed));
	}
	ClipExporter::Options options = base;
	options.outFps = 120;
	options.speed = 2;
	run("30fps sped 2x at 120fps holds each available frame twice", first, options, expected(0, 2, 120, 2));
	options.speed = 1;
	options.outFps = 15;
	run("explicit lower FPS still drops evenly", first, options, expected(0, 1, 15, 4));
	options.outFps = 60000.0 / 1001.0;
	run("fractional 59.94fps keeps an exact output clock", first, options, expected(0, 1, options.outFps, 4));
	options.outFps = 120;
	options.speed = 4;
	options.outWidth = options.outHeight = 32;
	run("resized high-FPS frames retain independent pixels and duration metadata", first, options, expected(0, 4, 120, 1));
	options.outWidth = options.outHeight = 0;
	options.speed = 1;
	const QString audioOverhang = dir.filePath("audio-overhang.mp4");
	bool overhangMade = false;
	command("ffmpeg", {"-v", "error", "-i", first, "-f", "lavfi", "-i", "sine=sample_rate=48000:duration=5",
		"-map", "0:v:0", "-map", "1:a:0", "-c:v", "copy", "-c:a", "aac", "-y", audioOverhang}, &overhangMade);
	check(overhangMade, QStringLiteral("made a source whose audio outlasts its video"));
	run("video EOF ignores a longer audio/container tail", audioOverhang, options, expected(0, 1, 120, 4));
	options.outFps = 120;
	options.startMs = 20;
	options.endMs = 1020;
	run("whole-frame clock off-grid trim seeks before its inpoint", whole, options, expected(0.02, 1, 120, 1));
	options = base;
	options.outFps = 120;
	options.keepAudio = true;
	options.cuts = {{125, 1125, 2, 0}, {2000, 4000, 4, 0}};
	QVector<int> mixed = expected(0.125, 2, 120, 0.5);
	mixed += expected(2, 4, 120, 0.5);
	run("single-source mixed-speed cuts and off-grid inpoint", first, options, mixed, true);
	options.cuts = {{20, 1020, 2, 0}};
	options.keepAudio = false;
	run("whole-frame clock off-grid cut", whole, options, expected(0.02, 2, 120, 0.5));
	options.inputs = {whole.toStdString(), coarse.toStdString()};
	options.cuts = {{20, 1020, 2, 0}, {1000, 3000, 4, 1}};
	mixed = expected(0.02, 2, 120, 0.5);
	mixed += expected(1, 4, 120, 0.5);
	run("multi-source mixed clocks and speeds", QString(), options, mixed);
	options = base;
	options.outFps = 60;
	options.keepAudio = true;
	options.inputs = {first.toStdString(), second.toStdString()};
	options.cuts = {{1000, 3000, 2, 1}, {0, 1000, 1, 0}};
	mixed = expected(1, 2, 60, 1, 128);
	mixed += expected(0, 1, 60, 1);
	run("multi-source audio cuts replace the frame exactly at each boundary", QString(), options, mixed, true);
	for (double fps : {60.0, 120.0}) {
		options = base;
		options.outFps = fps;
		options.canvasW = options.canvasH = 64;
		TlClip clip;
		clip.sourceId = 1;
		clip.srcEndMs = 4000;
		clip.speed = fps / 30.0;
		TlTrack track;
		track.clips = {clip};
		options.timeline.tracks = {track};
		options.timelineSources = {{1, first.toStdString()}};
		run(QStringLiteral("Full Editing 30fps sped %1x at %2fps").arg(clip.speed).arg(fps), QString(), options,
		    expected(0, clip.speed, fps, 4 / clip.speed));
	}
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
