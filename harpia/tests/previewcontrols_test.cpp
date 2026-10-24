// Changing editing modes or resetting the Full Editing playhead must repaint
// the picture for that mode and position. Old paced hover requests must not
// overwrite the new picture after the switch.
#include "editor/EditorWidgets.hpp"
#include "editor/TrackEditor.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cmath>
#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool condition, const char *what)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		++failures;
}

static void settle(int ms = 120)
{
	QElapsedTimer clock;
	clock.start();
	while (clock.elapsed() < ms) {
		QApplication::processEvents();
		QThread::msleep(5);
	}
}

static QPushButton *modeButton(VideoEditorWindow &editor, const QString &name)
{
	for (QPushButton *button : editor.findChildren<QPushButton *>())
		if (button->text() == name)
			return button;
	return nullptr;
}

static bool switchMode(VideoEditorWindow &editor, const QString &name)
{
	if (QPushButton *button = modeButton(editor, name)) {
		button->click();
		return true;
	}
	return false;
}

static bool redPicture(const QImage &image)
{
	if (image.isNull())
		return false;
	int red = 0;
	for (int y = 0; y < image.height(); y += 8)
		for (int x = 0; x < image.width(); x += 8) {
			const QColor c = image.pixelColor(x, y);
			if (c.red() > 180 && c.green() < 40 && c.blue() < 40)
				++red;
		}
	const int sampled = ((image.width() + 7) / 8) * ((image.height() + 7) / 8);
	return red > sampled * 0.95;
}

static bool invoke(VideoEditorWindow &editor, const char *method)
{
	return QMetaObject::invokeMethod(&editor, method, Qt::DirectConnection);
}

static bool sourceHover(VideoEditorWindow &editor, qint64 ms)
{
	return QMetaObject::invokeMethod(&editor, "onHoverScrub", Qt::DirectConnection, Q_ARG(qint64, ms));
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QTemporaryDir scratch;
	if (!scratch.isValid())
		return 2;
	const QString media = scratch.filePath(QStringLiteral("red.mp4"));
	QProcess ff;
	ff.start(QStringLiteral("ffmpeg"), {QStringLiteral("-y"), QStringLiteral("-loglevel"),
		QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
		QStringLiteral("color=c=red:s=320x180:r=30:d=2"), QStringLiteral("-c:v"),
		QStringLiteral("libx264"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), media});
	if (!ff.waitForFinished(30000) || ff.exitStatus() != QProcess::NormalExit || ff.exitCode() != 0) {
		std::printf("could not generate preview-control media\n");
		return 2;
	}

	VideoEditorWindow editor(media);
	settle();
	auto *canvas = editor.findChild<PreviewCanvas *>();
	auto *timeline = editor.findChild<TimelineView *>();
	auto *cuts = editor.findChild<TrackEditor *>();
	ok(editor.isValid() && canvas && timeline && cuts, "the real editor opens its video and timeline widgets");
	if (!canvas || !timeline || !cuts)
		return 1;
	ok(redPicture(canvas->currentFrame()), "Simple Trim starts on the source video");
	ok(switchMode(editor, QStringLiteral("Full Editing")), "Full Editing is available");
	TlClip first;
	first.type = TlClip::Type::Text;
	first.srcEndMs = 1000;
	first.text.text = QStringLiteral("FIRST FRAME");
	first.text.fontPx = 48;
	TlClip second = first;
	second.outStartMs = 1000;
	second.srcEndMs = 3000;
	second.text.text = QStringLiteral("SECOND FRAME");
	TlTrack text;
	text.clips = {first, second};
	TimelineModel model;
	model.tracks = {text};
	timeline->setModelAndCommit(model);
	timeline->setPlayhead(0);
	emit timeline->scrub(0);
	settle();
	const QImage fullStart = canvas->currentFrame();
	ok(!fullStart.isNull() && !redPicture(fullStart), "Full Editing renders its own caption composition");

	switchMode(editor, QStringLiteral("Simple Trim"));
	settle();
	ok(redPicture(canvas->currentFrame()), "switching Full Editing to Simple Trim redraws the source picture");
	switchMode(editor, QStringLiteral("Full Editing"));
	settle();
	const int sourceId = editor.activeSourceIdForTest();
	cuts->setSegments({CutSegment{500, 1500, 1.0, sourceId}});
	cuts->setPlayhead(0);
	switchMode(editor, QStringLiteral("Multi-Cut"));
	settle();
	ok(redPicture(canvas->currentFrame()), "switching Full Editing to Multi-Cut redraws the assembled output");

	// Request two source times inside one cooldown; the second remains queued
	// until the paced preview timer runs. Then change mode before that happens.
	switchMode(editor, QStringLiteral("Simple Trim"));
	settle();
	ok(sourceHover(editor, 1000) && sourceHover(editor, 1200), "a source hover is queued through the real preview path");
	switchMode(editor, QStringLiteral("Full Editing"));
	const QImage afterSwitch = canvas->currentFrame();
	invoke(editor, "onPreviewTick");
	settle();
	ok(afterSwitch == fullStart && canvas->currentFrame() == fullStart,
	   "a queued source hover cannot overwrite the Full Editing picture after a mode switch");
	emit timeline->hoverScrub(0);
	emit timeline->hoverScrub(1500);
	switchMode(editor, QStringLiteral("Simple Trim"));
	invoke(editor, "onPreviewTick");
	settle();
	ok(redPicture(canvas->currentFrame()), "a queued timeline hover cannot overwrite the Simple Trim picture");

	switchMode(editor, QStringLiteral("Full Editing"));
	timeline->setPlayhead(1500);
	emit timeline->scrub(1500);
	settle();
	ok(canvas->currentFrame() != fullStart, "the Full Editing playhead can show a later caption");
	QPushButton *reset = nullptr;
	for (QPushButton *button : editor.findChildren<QPushButton *>())
		if (button->toolTip() == QStringLiteral("Move the playhead back to the start"))
			reset = button;
	ok(reset != nullptr, "the real toolbar reset control is present");
	if (reset)
		reset->click();
	settle();
	ok(timeline->playhead() == 0, "the reset control returns the Full Editing playhead to zero");
	ok(canvas->currentFrame() == fullStart, "reset also redraws the first Full Editing frame");
	ok(timeline->model() == model, "preview controls leave all clip edits unchanged");

	// The picture and the visible edit grips must describe the same animated
	// pose. The canvas does not hide these grips while playback is running.
	TlClip moving;
	moving.sourceId = sourceId;
	moving.srcEndMs = 1800;
	TlTransform startPose;
	startPose.posX = 0.25;
	startPose.scale = 0.4;
	TlTransform endPose = startPose;
	endPose.posX = 0.75;
	moving.setKeyframeAt(0, startPose, TlEase::Linear);
	moving.setKeyframeAt(1000, endPose, TlEase::Linear);
	ComponentInstance mask;
	mask.typeId = QStringLiteral("harpia.mask");
	mask.instanceId = QStringLiteral("animated-mask");
	mask.props.insert(QStringLiteral("width"), 0.4);
	mask.props.insert(QStringLiteral("height"), 0.4);
	PropKey maskStart;
	maskStart.tMs = 0;
	maskStart.v = 0.2;
	maskStart.ease = TlEase::Linear;
	PropKey maskEnd = maskStart;
	maskEnd.tMs = 1000;
	maskEnd.v = 0.8;
	mask.keys.insert(QStringLiteral("centreX"), {maskStart, maskEnd});
	moving.components = {mask};
	TlTrack animatedTrack;
	animatedTrack.clips = {moving};
	TimelineModel animated;
	animated.tracks = {animatedTrack};
	timeline->setModelAndCommit(animated);
	timeline->selectClip(0, 0);
	emit timeline->scrub(0);
	settle();
	const QVector<QPointF> initialHandles = canvas->handlePointsForTest();
	const QImage initialPicture = canvas->currentFrame();
	ok(canvas->transformMode() && initialHandles.size() == 9 && canvas->maskEdit().on,
	   "the selected clip exposes transform and mask grips before playback");
	invoke(editor, "onPlayPause");
	for (QTimer *timer : editor.findChildren<QTimer *>())
		if (timer->isActive() && timer->interval() == 33)
			timer->stop();
	QThread::msleep(150);
	invoke(editor, "onPlayTick");
	const qint64 playedMs = timeline->playhead();
	const QVector<QPointF> playingHandles = canvas->handlePointsForTest();
	ok(playedMs >= 100 && canvas->currentFrame() != initialPicture,
	   "the real playback tick advances the clip's animated picture");
	bool handlesFollow = initialHandles.size() == 9 && playingHandles.size() == 9;
	if (handlesFollow) {
		const double actual = ((playingHandles[0] + playingHandles[7]) / 2.0).x() -
				      ((initialHandles[0] + initialHandles[7]) / 2.0).x();
		const double expected = (moving.transformAt(playedMs).posX - startPose.posX) *
					canvas->displaySize().width();
		handlesFollow = std::abs(actual - expected) < 1.5;
	}
	ok(handlesFollow, "visible transform grips follow the keyframed clip during playback");
	const ComponentType *maskType = ComponentRegistry::instance().find(mask.typeId);
	const double expectedMask = maskType ? propAt(*maskType, mask, QStringLiteral("centreX"), playedMs).toDouble() : -1;
	ok(canvas->maskEdit().on && std::abs(canvas->maskEdit().cx - expectedMask) < 1e-6,
	   "visible mask grips follow the keyframed mask during playback");
	invoke(editor, "onPlayPause");
	ok(timeline->model() == animated, "following animated grips does not edit the clip's keys");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
