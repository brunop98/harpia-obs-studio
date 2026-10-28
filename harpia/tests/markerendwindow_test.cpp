// Exercise the real editor's end marker, transport, history and project file.
// Stills make the frame at the boundary deterministic without a video decoder.
#include "editor/AudioPreview.hpp"
#include "editor/EditorWidgets.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineJson.hpp"
#include "editor/timeline/TimelineView.hpp"
#include "editor/tts/TtsAudio.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool condition, const char *what)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		++failures;
}

static void settle(int ms = 30)
{
	QElapsedTimer clock;
	clock.start();
	while (clock.elapsed() < ms) {
		QApplication::processEvents();
		QThread::msleep(2);
	}
}

static bool invoke(VideoEditorWindow &editor, const char *method)
{
	return QMetaObject::invokeMethod(&editor, method, Qt::DirectConnection);
}

static QPushButton *button(VideoEditorWindow &editor, const QString &text)
{
	for (QPushButton *candidate : editor.findChildren<QPushButton *>())
		if (candidate->text() == text)
			return candidate;
	return nullptr;
}

static QTimer *playbackTimer(VideoEditorWindow &editor)
{
	for (QTimer *timer : editor.findChildren<QTimer *>())
		if (timer->interval() == 33 && !timer->isSingleShot())
			return timer;
	return nullptr;
}

static bool redPicture(const QImage &image)
{
	if (image.isNull())
		return false;
	const QColor color = image.pixelColor(image.width() / 2, image.height() / 2);
	return color.red() > 200 && color.green() < 40 && color.blue() < 40;
}

static TimelineModel imageTimeline()
{
	TlClip red;
	red.type = TlClip::Type::Image;
	red.sourceId = 1;
	red.srcEndMs = 200;
	TlClip blue = red;
	blue.sourceId = 2;
	blue.outStartMs = 200;
	blue.srcEndMs = 800;
	TlTrack video;
	video.kind = TlTrack::Kind::Video;
	video.clips = {red, blue};
	TimelineModel model;
	model.tracks = {video};
	model.markers = {100, 800};
	model.markerLabels.insert(100, QStringLiteral("Intro"));
	model.markerLabels.insert(800, QStringLiteral("Later section"));
	model.videoEndMs = 200;
	return model;
}

static bool writeProject(const QString &path, const TimelineModel &model, const QStringList &sources)
{
	QJsonObject root;
	root[QStringLiteral("harpiaProject")] = 3;
	root[QStringLiteral("editMode")] = QStringLiteral("full");
	root[QStringLiteral("canvasW")] = 64;
	root[QStringLiteral("canvasH")] = 64;
	root[QStringLiteral("fps")] = 10;
	QJsonArray list;
	for (int i = 0; i < sources.size(); ++i) {
		QJsonObject source;
		source[QStringLiteral("id")] = i + 1;
		source[QStringLiteral("path")] = sources[i];
		list.append(source);
	}
	root[QStringLiteral("sources")] = list;
	timelineToProject(model, root);
	QFile file(path);
	const QByteArray json = QJsonDocument(root).toJson();
	return file.open(QIODevice::WriteOnly) && file.write(json) == json.size();
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir scratch;
	if (!scratch.isValid())
		return 2;
	qputenv("XDG_CONFIG_HOME", scratch.filePath(QStringLiteral("config")).toUtf8());
	qputenv("XDG_CACHE_HOME", scratch.filePath(QStringLiteral("cache")).toUtf8());
	qputenv("XDG_DATA_HOME", scratch.filePath(QStringLiteral("data")).toUtf8());
	QApplication app(argc, argv);
	QStandardPaths::setTestModeEnabled(true);
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch.path());
	QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, scratch.path());
#ifndef Q_OS_MACOS
	QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, scratch.path());
	QSettings::setPath(QSettings::NativeFormat, QSettings::SystemScope, scratch.path());
#endif
	const QString red = scratch.filePath(QStringLiteral("red.png"));
	const QString blue = scratch.filePath(QStringLiteral("blue.png"));
	QImage image(64, 64, QImage::Format_RGB32);
	image.fill(Qt::red);
	const bool redSaved = image.save(red);
	image.fill(Qt::blue);
	const bool blueSaved = image.save(blue);
	const QString project = scratch.filePath(QStringLiteral("markers.harpiaproj"));
	ok(redSaved && blueSaved && writeProject(project, imageTimeline(), {red, blue}),
	   "created self-contained still-image project with named markers and an output end");

	VideoEditorWindow editor{QString()};
	ok(editor.openProjectAt(project), "the editor opens a project with an explicit video end");
	auto *timeline = editor.findChild<TimelineView *>();
	auto *canvas = editor.findChild<PreviewCanvas *>();
	auto *timer = playbackTimer(editor);
	if (!timeline || !canvas || !timer) {
		ok(false, "the real timeline, preview and playback timer are available");
		return 1;
	}
	const TimelineModel initial = timeline->model();
	ok(initial.videoEndMs == 200 && initial.contentDurationMs() == 1000 &&
	   initial.markerLabels.value(100) == QStringLiteral("Intro"),
	   "opening restores labels and the end marker without trimming later clips");

	std::printf("\n-- the marked end is a transport boundary --\n");
	timeline->setPlayhead(150);
	ok(invoke(editor, "onPlayPause") && timer->isActive(), "Play starts before the marked end");
	QThread::msleep(90); // Keep the real clock; run its next tick deterministically.
	ok(invoke(editor, "onPlayTick") && !timer->isActive() && timeline->playhead() == 200,
	   "playback stops on the explicit end instead of looping to the beginning");
	settle();
	ok(redPicture(canvas->currentFrame()), "the stopped preview holds the last in-range frame, not the next clip");
	if (timer->isActive())
		invoke(editor, "onPlayPause");
	timeline->setVideoEnd(900);
	timeline->setPlayhead(600);
	invoke(editor, "onPlayPause");
	ok(timer->isActive(), "playback can start inside the longer marked output");
	timeline->setVideoEnd(300);
	ok(!timer->isActive() && timeline->playhead() == 300,
	   "moving the end behind the playing position immediately stops at the new end");
	if (timer->isActive())
		invoke(editor, "onPlayPause");
	timeline->resetVideoEnd();
	timeline->setPlayhead(950);
	invoke(editor, "onPlayPause");
	QThread::msleep(90);
	invoke(editor, "onPlayTick");
	ok(timer->isActive() && timeline->playhead() < 200,
	   "resetting the end restores the existing automatic-end loop");
	if (timer->isActive())
		invoke(editor, "onPlayPause");

	std::printf("\n-- project markers share the editor's history --\n");
	timeline->setModelAndCommit(initial);
	timeline->addMarker(500, QStringLiteral("Middle"));
	ok(timeline->model().markerLabels.value(500) == QStringLiteral("Middle"), "a named marker is added");
	ok(invoke(editor, "undo") && !timeline->model().markers.contains(500), "Undo removes the added marker");
	ok(invoke(editor, "redo") && timeline->model().markerLabels.value(500) == QStringLiteral("Middle"),
	   "Redo restores the marker and its label");
	timeline->renameMarker(500, QStringLiteral("Chapter two"));
	ok(invoke(editor, "undo") && timeline->model().markerLabels.value(500) == QStringLiteral("Middle"),
	   "renaming a marker is a separate undo step");
	ok(invoke(editor, "redo") && timeline->model().markerLabels.value(500) == QStringLiteral("Chapter two"),
	   "Redo restores the edited label");
	timeline->setVideoEnd(450);
	ok(invoke(editor, "undo") && timeline->model().videoEndMs == 200, "Undo restores the previous video end");
	ok(invoke(editor, "redo") && timeline->model().videoEndMs == 450, "Redo restores the chosen video end");
	timeline->selectClip(0, 1);
	timeline->nudgeSelection(50);
	ok(timeline->model().tracks[0].clips[1].outStartMs == 250 &&
	   timeline->model().tracks[0].clips[1].outDurationMs() == 800 && timeline->model().videoEndMs == 450,
	   "clips after the output end stay editable and retain their full duration");
	const QString saved = scratch.filePath(QStringLiteral("saved.harpiaproj"));
	ok(editor.saveProjectTo(saved, true).isEmpty(), "the edited project saves without dialogs");
	VideoEditorWindow reopened{QString()};
	ok(reopened.openProjectAt(saved), "the saved project reopens");
	const auto *loaded = reopened.findChild<TimelineView *>();
	ok(loaded && loaded->model().videoEndMs == 450 &&
	   loaded->model().markerLabels.value(500) == QStringLiteral("Chapter two") &&
	   loaded->model().markers == timeline->model().markers &&
	   loaded->model().contentDurationMs() == timeline->model().contentDurationMs(),
	   "save/open preserves named markers, output boundary and untrimmed later clips");

	std::printf("\n-- editing chapter labels retains cached audio --\n");
	tts_audio::Pcm tone;
	tone.samples.fill(4000, 48000 * 2);
	const QString wav = scratch.filePath(QStringLiteral("tone.wav"));
	TimelineModel withAudio = imageTimeline();
	withAudio.videoEndMs = 900;
	TlTrack audioTrack;
	audioTrack.kind = TlTrack::Kind::Audio;
	TlClip audioClip;
	audioClip.sourceId = 3;
	audioClip.srcEndMs = 1000;
	audioTrack.clips = {audioClip};
	withAudio.tracks.append(audioTrack);
	const QString audioProject = scratch.filePath(QStringLiteral("audio.harpiaproj"));
	ok(tts_audio::writeWav16(wav, tone) && writeProject(audioProject, withAudio, {red, blue, wav}) &&
	   editor.openProjectAt(audioProject), "opened a real audio source for the marker-edit regression");
	auto *audio = editor.findChild<AudioPreview *>();
	if (audio)
		audio->setMuted(false);
	emit timeline->clipsChanged();
	QElapsedTimer waiting;
	waiting.start();
	while (audio && !audio->hasAudio() && waiting.elapsed() < 6000)
		settle(10);
	ok(audio && audio->hasAudio(), "the real asynchronous timeline premix completes");
	if (audio && audio->hasAudio()) {
		const qint64 duration = audio->durationMs();
		audio->setMuted(true); // A physical output device is not needed to check buffer lifetime.
		timeline->setPlayhead(0);
		invoke(editor, "onPlayPause");
		timeline->renameMarker(100, QStringLiteral("Renamed while playing"));
		ok(timer->isActive() && audio->hasAudio() && audio->durationMs() == duration,
		   "renaming a chapter during playback retains the already-built audio buffer");
		timeline->setVideoEnd(700);
		ok(!audio->hasAudio(), "changing the video end invalidates the old bounded audio mix");
		if (timer->isActive())
			invoke(editor, "onPlayPause");
	}

	// The source-to-full bridge appends after all editable clips, even when the
	// output boundary is earlier. The standard harness already generates this video.
	const QString media = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
	const QString video = QDir(media).filePath(QStringLiteral("av_red.mp4"));
	if (!media.isEmpty() && QFile::exists(video)) {
		VideoEditorWindow bridge(video);
		QPushButton *full = button(bridge, QStringLiteral("Full Editing"));
		QPushButton *trim = button(bridge, QStringLiteral("Simple Trim"));
		QPushButton *send = button(bridge, QStringLiteral("Send to Full Editing →"));
		auto *bridgeTimeline = bridge.findChild<TimelineView *>();
		if (full && trim && send && bridgeTimeline) {
			full->click();
			const qint64 contentEnd = bridgeTimeline->model().contentDurationMs();
			bridgeTimeline->setVideoEnd(100);
			trim->click();
			send->click();
			bool appendedAfterContent = false;
			for (const TlTrack &track : bridgeTimeline->model().tracks)
				for (const TlClip &clip : track.clips)
					appendedAfterContent = appendedAfterContent || clip.outStartMs == contentEnd;
			ok(contentEnd > 100 && appendedAfterContent && bridgeTimeline->model().videoEndMs == 100,
			   "Send to Full Editing appends after content, not inside the marked output");
		} else {
			ok(false, "the source-to-full editing bridge is available");
		}
	}
	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
