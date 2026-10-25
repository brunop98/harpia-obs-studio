// Actual files through every editor export path, checked by an independent
// FFmpeg decoder. Native Mac runs also exercise Auto's real VT/fallback choice.
#include "core/ShareExporter.hpp"
#include "editor/ClipExporter.hpp"

#include <QGuiApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}
bool decodes(const QString &path, int frames)
{
	QProcess probe;
	probe.start(QStringLiteral("ffprobe"), {"-v", "error", "-count_frames", "-select_streams", "v:0",
		"-show_entries", "stream=codec_name,nb_read_frames,width,height", "-of", "json", path});
	if (!probe.waitForFinished(30000) || probe.exitCode())
		return false;
	const auto streams = QJsonDocument::fromJson(probe.readAllStandardOutput()).object().value("streams").toArray();
	if (streams.isEmpty())
		return false;
	const auto stream = streams.first().toObject();
	if (stream.value("codec_name").toString() != QStringLiteral("h264") ||
	    stream.value("nb_read_frames").toString().toInt() != frames ||
	    stream.value("width").toInt() != 160 || stream.value("height").toInt() != 90)
		return false;
	QProcess decode;
	decode.start(QStringLiteral("ffmpeg"), {"-v", "error", "-xerror", "-i", path, "-f", "null", "-"});
	return decode.waitForFinished(30000) && decode.exitCode() == 0;
}
}

int main(int argc, char **argv)
{
	QGuiApplication app(argc, argv);
	const QString media = argc > 1 ? QString::fromLocal8Bit(argv[1]) : qEnvironmentVariable("HARPIA_TEST_MEDIA");
	const QString input = QDir(media).filePath(QStringLiteral("av_red.mp4"));
	const QString second = QDir(media).filePath(QStringLiteral("av_green.mp4"));
	QTemporaryDir folder;
	check(QFileInfo::exists(input) && QFileInfo::exists(second) && folder.isValid(), "media fixtures are available");
	if (failures)
		return 1;
	for (int path = 0; path < 4; ++path) {
		ClipExporter::Options options;
		options.encoding = path % 2 ? export_video::Encoding::Software : export_video::Encoding::Automatic;
		options.effort = ClipExporter::Options::Effort::Fast;
		options.startMs = 0;
		// End between source frames so the existing inclusive trim endpoint
		// does not add a frame exactly on the next output second.
		options.endMs = 999;
		options.outWidth = 160;
		options.outHeight = 90;
		options.outFps = 15;
		if (path == 1 || path == 2)
			options.cuts = {{0, 499, 1.0, 0}, {500, 999, 1.0, path == 2 ? 1 : 0}};
		if (path == 2)
			options.inputs = {input.toStdString(), second.toStdString()};
		if (path == 3) {
			TlClip clip;
			clip.type = TlClip::Type::Video;
			clip.sourceId = 1;
			clip.srcEndMs = 1000;
			TlTrack track;
			track.clips.push_back(clip);
			options.timeline.tracks.push_back(track);
			options.timelineSources[1] = input.toStdString();
			options.canvasW = 320;
			options.canvasH = 180;
			options.timelineFps = 30;
		}
		const QString output = folder.filePath(QStringLiteral("path-%1.mp4").arg(path));
		ClipExporter exporter;
		bool success = false;
		int finished = 0;
		QString encoding, error;
		QObject::connect(&exporter, &ClipExporter::finished, [&](bool ok, bool canceled, const QString &reason) {
			success = ok && !canceled; error = reason; ++finished;
		});
		QObject::connect(&exporter, &ClipExporter::encodingChanged,
			[&](const QString &description) { encoding = description; });
		exporter.run(input, output, options);
		if (!success)
			std::printf("  export error: %s\n", qPrintable(error));
		check(success && finished == 1 && !encoding.isEmpty(), "export succeeds once and reports its actual encoder");
		check(success && decodes(output, 15), "all output frames decode with requested resolution and cadence");
		if (options.encoding == export_video::Encoding::Software)
			check(encoding.contains(QStringLiteral("libx264")), "explicit software is honoured in cuts and timeline exports");
	}
	for (const auto mode : {export_video::Encoding::Software, export_video::Encoding::Automatic}) {
		ShareExporter exporter;
		auto options = ShareExporter::optionsFor(ShareExporter::Level::Balanced);
		options.maxHeight = 90;
		options.encoding = mode;
		bool success = false;
		QString status;
		QObject::connect(&exporter, &ShareExporter::finished,
			[&](bool ok, bool canceled, const QString &) { success = ok && !canceled; });
		QObject::connect(&exporter, &ShareExporter::statusChanged,
			[&](const QString &text) { status = text; });
		const QString output = folder.filePath(QStringLiteral("sharing-%1.mp4").arg(int(mode)));
		exporter.run(input, output, options);
		check(success && !status.isEmpty(), "sharing exports report their actual encoding choice");
		check(success && decodes(output, 120), "sharing retains all video frames and a decodable audio track");
	}
#ifndef Q_OS_MACOS
	{
		ClipExporter exporter;
		ClipExporter::Options options;
		options.encoding = export_video::Encoding::AppleHardware;
		options.endMs = 1000;
		bool failedClearly = false;
		QObject::connect(&exporter, &ClipExporter::finished, [&](bool ok, bool canceled, const QString &error) {
			failedClearly = !ok && !canceled && error.contains(QStringLiteral("macOS"));
		});
		exporter.run(input, folder.filePath(QStringLiteral("unavailable.mp4")), options);
		check(failedClearly, "an explicitly unavailable hardware encoder cannot become a silent software success");
	}
#endif
	return failures ? 1 : 0;
}
