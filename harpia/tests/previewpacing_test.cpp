// Short, equally trimmed clips must begin at their source in-points. The old
// playback provider painted its bounded decoder catch-up (eight source frames
// per preview tick), visibly rushing through earlier footage at each cut.
// Drive the real editor and read frame numbers encoded into real video pixels.
#include "editor/EditorWidgets.hpp"
#include "editor/FrameSeeker.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineJson.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <array>
#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool condition, const char *what)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		++failures;
}

static bool makeCounter(const QString &path, int fps)
{
	QProcess ff;
	ff.start(QStringLiteral("ffmpeg"), {
		QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
		QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
		QStringLiteral("nullsrc=s=64x64:r=%1:d=12,format=yuv420p,"
				   "geq=lum='16+219*mod(floor(N/pow(2\\,floor(X/8)))\\,2)':cb=128:cr=128")
			.arg(fps),
		QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-preset"), QStringLiteral("ultrafast"),
		QStringLiteral("-g"), QStringLiteral("600"), QStringLiteral("-keyint_min"), QStringLiteral("600"),
		QStringLiteral("-sc_threshold"), QStringLiteral("0"), QStringLiteral("-crf"), QStringLiteral("8"),
		QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), QStringLiteral("-y"), path});
	return ff.waitForFinished(30000) && ff.exitStatus() == QProcess::NormalExit && ff.exitCode() == 0;
}

static int counterOf(const QImage &image)
{
	if (image.isNull() || image.width() != 64 || image.height() != 64)
		return -1;
	const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
	int count = 0;
	for (int bit = 0; bit < 8; ++bit)
		if (qGray(rgb.pixel(bit * 8 + 4, 32)) > 128)
			count |= 1 << bit;
	return count;
}

static bool writeProject(const QString &path, const QStringList &sources, int fps)
{
	TimelineModel model;
	TlTrack empty;
	empty.name = QStringLiteral("Upper lane");
	model.tracks.append(empty);
	TlTrack video;
	video.name = QStringLiteral("Equal-duration cuts");
	const std::array<qint64, 3> starts = {5000, 8000, 4000};
	for (int i = 0; i < 3; ++i) {
		TlClip clip;
		clip.sourceId = i == 1 ? 2 : 1;
		clip.srcStartMs = starts[i];
		clip.srcEndMs = starts[i] + 2000;
		clip.outStartMs = i * 1000;
		video.clips.append(clip);
	}
	model.tracks.append(video);
	QJsonObject root;
	root[QStringLiteral("harpiaProject")] = 2;
	root[QStringLiteral("editMode")] = QStringLiteral("full");
	root[QStringLiteral("canvasW")] = 64;
	root[QStringLiteral("canvasH")] = 64;
	root[QStringLiteral("fps")] = fps;
	QJsonArray sourceArray;
	for (int i = 0; i < sources.size(); ++i) {
		QJsonObject source;
		source[QStringLiteral("id")] = i + 1;
		source[QStringLiteral("path")] = sources[i];
		source[QStringLiteral("name")] = QStringLiteral("Counter %1").arg(i + 1);
		sourceArray.append(source);
	}
	root[QStringLiteral("sources")] = sourceArray;
	timelineToProject(model, root);
	QFile file(path);
	const QByteArray json = QJsonDocument(root).toJson();
	return file.open(QIODevice::WriteOnly) && file.write(json) == json.size();
}

static bool invoke(VideoEditorWindow &editor, const char *method)
{
	return QMetaObject::invokeMethod(&editor, method, Qt::DirectConnection);
}

static void stopPlaybackTimer(VideoEditorWindow &editor)
{
	// Keep the production playback state and clock, but invoke ticks ourselves.
	// Queued scrub-cache frames cannot replace the playback picture in between.
	for (QTimer *timer : editor.findChildren<QTimer *>())
		if (timer->isActive() && timer->interval() == 33)
			timer->stop();
}

static bool pictureMatches(VideoEditorWindow &, TimelineView &timeline, PreviewCanvas &canvas,
			   std::array<FrameSeeker, 2> &reference)
{
	const qint64 outMs = timeline.playhead();
	const TlTrack &track = timeline.model().tracks[1];
	const int index = track.clipAt(outMs);
	if (index < 0)
		return false;
	const TlClip &clip = track.clips[index];
	const int fileIndex = index == 1 ? 1 : 0;
	const int expected = counterOf(reference[fileIndex].frameAt(clip.srcAtOutput(outMs), 64, 64));
	const int actual = counterOf(canvas.currentFrame());
	if (expected < 0 || actual != expected) {
		std::printf("     at output %lld ms, clip %d: frame %d, wanted %d\n",
			    (long long)outMs, index + 1, actual, expected);
		return false;
	}
	return true;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QTemporaryDir scratch;
	if (!scratch.isValid())
		return 2;

	for (int fps : {30, 10}) {
		std::printf("\n-- %d fps sources, three adjacent one-second clips --\n", fps);
		const QString sourceA = scratch.filePath(QStringLiteral("counter-%1-a.mp4").arg(fps));
		const QString sourceB = scratch.filePath(QStringLiteral("counter-%1-b.mp4").arg(fps));
		if (!makeCounter(sourceA, fps) || !QFile::copy(sourceA, sourceB)) {
			ok(false, "generated numbered source videos with a long GOP");
			continue;
		}
		const QString project = scratch.filePath(QStringLiteral("pacing-%1.harpiaproj").arg(fps));
		ok(writeProject(project, {sourceA, sourceB}, fps), "created the multi-source project");
		VideoEditorWindow editor{QString()};
		ok(editor.openProjectAt(project), "the real editor opens the project");
		auto *timeline = editor.findChild<TimelineView *>();
		auto *canvas = editor.findChild<PreviewCanvas *>();
		if (!timeline || !canvas) {
			ok(false, "the editor exposes its real timeline and preview");
			continue;
		}
		timeline->selectClip(1, 0);
		timeline->addToSelection(1, 1);
		timeline->addToSelection(1, 2);
		ok(timeline->trimSelectedVideosToDuration(1000) == 3,
		   "batch duration trims all three clips to one second");
		const TimelineModel before = timeline->model();
		std::array<FrameSeeker, 2> reference;
		if (!reference[0].open(sourceA) || !reference[1].open(sourceB)) {
			ok(false, "the independent reference decoders open");
			continue;
		}

		for (qint64 start : {qint64(0), qint64(1000), qint64(2000)}) {
			// Leave a hover request in the scrub cooldown queue before Play.
			emit timeline->hoverScrub(1500);
			emit timeline->hoverScrub(2500);
			timeline->setPlayhead(start);
			ok(invoke(editor, "onPlayPause"), "the production Play action starts playback");
			stopPlaybackTimer(editor);
			ok(pictureMatches(editor, *timeline, *canvas, reference),
			   "Play immediately shows the clip's actual in-point, without catch-up footage");
			ok(invoke(editor, "onPreviewTick") && pictureMatches(editor, *timeline, *canvas, reference),
			   "a queued hover cannot overwrite the first playback frame");
			bool accurate = true;
			for (int sample = 0; sample < 6; ++sample) {
				QThread::msleep(20);
				accurate = invoke(editor, "onPlayTick") &&
					   pictureMatches(editor, *timeline, *canvas, reference) && accurate;
			}
			ok(accurate && timeline->playhead() >= start + 100,
			   "the first 120 milliseconds advance at the source's normal cadence");
			invoke(editor, "onPlayPause");
		}

		// Cross both cuts without pausing. The second source is initially cold;
		// returning to the first source then jumps backward inside a long GOP.
		timeline->setPlayhead(0);
		invoke(editor, "onPlayPause");
		stopPlaybackTimer(editor);
		bool accurateJoin = true;
		bool crossedForward = false;
		bool crossedBackward = false;
		QElapsedTimer timeout;
		timeout.start();
		while (timeline->playhead() < 2140 && timeout.elapsed() < 3500) {
			QThread::msleep(20);
			invoke(editor, "onPlayTick");
			crossedForward = crossedForward || timeline->playhead() >= 1000;
			crossedBackward = crossedBackward || timeline->playhead() >= 2000;
			accurateJoin = pictureMatches(editor, *timeline, *canvas, reference) && accurateJoin;
		}
		ok(crossedForward && crossedBackward && accurateJoin,
		   "uninterrupted cuts to another source and back to an earlier in-point show the correct frames");
		invoke(editor, "onPlayPause");
		ok(timeline->model() == before, "preview playback leaves clip speeds, durations and edits unchanged");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
