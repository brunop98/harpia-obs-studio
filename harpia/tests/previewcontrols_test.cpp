// Changing editing modes or resetting the Full Editing playhead must repaint
// the picture for that mode and position. Old paced hover requests must not
// overwrite the new picture after the switch.
#include <QtWidgets>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>
#include "editor/EditorWidgets.hpp"
#include "editor/PreviewDecoder.hpp"
// Observe the cooldown boundary to deliver an old completion while a newer
// real hover is pending, independently of worker scheduling on this machine.
#define private public
#include "editor/TrackEditor.hpp"
#include "editor/VideoEditorWindow.hpp"
#undef private
#include "editor/component/ComponentRegistry.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMimeData>
#include <QMouseEvent>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>

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

static bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 3000)
{
	QElapsedTimer clock;
	clock.start();
	while (!predicate() && clock.elapsed() < timeoutMs) {
		QApplication::processEvents();
		QThread::msleep(2);
	}
	return predicate();
}

static qint64 hoverTrack(TrackEditor *cuts, const QRect &rect, double fraction)
{
	const QPoint point(rect.left() + int(rect.width() * fraction), rect.center().y());
	QMouseEvent hover(QEvent::MouseMove, QPointF(point), cuts->mapToGlobal(point),
		Qt::NoButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(cuts, &hover);
	return cuts->hoverMs_;
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

	// Choosing another Multi-Cut source owns the preview immediately. An old
	// Source-track scrub queued in the 20 ms cooldown must not put the previous
	// video's picture back over the newly selected video.
	const QString blueMedia = scratch.filePath(QStringLiteral("blue.mp4"));
	ff.start(QStringLiteral("ffmpeg"), {QStringLiteral("-y"), QStringLiteral("-loglevel"),
		QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
		QStringLiteral("color=c=blue:s=320x180:r=30:d=2"), QStringLiteral("-c:v"),
		QStringLiteral("libx264"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), blueMedia});
	const bool blueReady = ff.waitForFinished(30000) && ff.exitStatus() == QProcess::NormalExit && ff.exitCode() == 0;
	ok(blueReady, "a second, visibly different video is available for source switching");
	if (blueReady) {
		switchMode(editor, QStringLiteral("Multi-Cut"));
		settle();
		const QVector<CutSegment> keptCuts = cuts->segments();
		QMimeData mime;
		mime.setUrls({QUrl::fromLocalFile(blueMedia)});
		QDragEnterEvent enter(QPoint(2, 2), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(&editor, &enter);
		QDropEvent drop(QPointF(2, 2), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(&editor, &drop);
		settle();
		const int blueId = editor.activeSourceIdForTest();
		auto *sourcePicker = editor.findChild<QComboBox *>(QStringLiteral("multiCutSourceSelector"));
		ok(drop.isAccepted() && blueId != sourceId && sourcePicker && sourcePicker->count() == 2,
		   "adding a video exposes both Multi-Cut sources");
		if (sourcePicker && blueId != sourceId) {
			auto selectSource = [&](int id) {
				const int index = sourcePicker->findData(id);
				sourcePicker->setCurrentIndex(index);
				return index >= 0 && QMetaObject::invokeMethod(sourcePicker, "activated",
					Qt::DirectConnection, Q_ARG(int, index));
			};
			selectSource(sourceId);
			settle();
			ok(redPicture(canvas->currentFrame()), "the original source is red before switching videos");
			// A real Source-track hover stamps its source id before the two
			// scrub signals queue work through the editor's paced path.
			const QRect sourceRect = cuts->sourceRectForTest();
			const QPoint sourcePoint(sourceRect.left() + sourceRect.width() / 4, sourceRect.center().y());
			QMouseEvent hover(QEvent::MouseMove, QPointF(sourcePoint), cuts->mapToGlobal(sourcePoint),
				Qt::NoButton, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(cuts, &hover);
			emit cuts->scrubSource(1000);
			emit cuts->scrubSource(1200);
			ok(cuts->scrubSourceId() == sourceId && selectSource(blueId),
			   "the dropdown selects the second video while an old-source scrub is queued");
			invoke(editor, "onPreviewTick");
			settle();
			const QImage picture = canvas->currentFrame();
			const QColor centre = picture.isNull() ? QColor() : picture.pixelColor(picture.width() / 2, picture.height() / 2);
			ok(editor.activeSourceIdForTest() == blueId && centre.blue() > 180 && centre.red() < 40,
			   "queued old-source previews cannot overwrite the chosen blue video");
			ok(cuts->segments() == keptCuts, "switching source videos preserves every kept cut");
		}
	}

	// A completion for the picture currently displayed must never replace the
	// newer hover waiting behind the 20ms preview cooldown. With several videos
	// those completions arrive more often while the pointer crosses clips; the
	// old handler could leave the preview stuck until another mouse movement.
	if (blueReady) {
		const QString changingMedia = scratch.filePath(QStringLiteral("red-then-green.mp4"));
		ff.start(QStringLiteral("ffmpeg"), {"-y", "-loglevel", "error", "-f", "lavfi", "-i",
			"color=c=red:s=320x180:r=30:d=2", "-f", "lavfi", "-i", "color=c=lime:s=320x180:r=30:d=2",
			"-filter_complex", "[0:v][1:v]concat=n=2:v=1:a=0[v]", "-map", "[v]", "-c:v", "libx264",
			"-preset", "ultrafast", "-threads", "1", "-pix_fmt", "yuv420p", changingMedia});
		const bool changingReady = ff.waitForFinished(30000) && ff.exitStatus() == QProcess::NormalExit && ff.exitCode() == 0;
		const int changingId = changingReady ? editor.addSource(changingMedia) : -1;
		EditorSource *blue = editor.sourceByPath(blueMedia);
		ok(changingId >= 0 && blue, "a temporal color source is available for pending-hover regressions");
		if (changingId >= 0 && blue) {
			const int blueId = blue->id;
			const QVector<CutSegment> segments{{0, 4000, 1.0, changingId}, {0, 1800, 1.0, blueId}};
			cuts->setSegments(segments);
			editor.setActiveSource(changingId);
			ok(waitUntil([&]() { return editor.shownSource_ == changingId && editor.shownExact_ && redPicture(canvas->currentFrame()); }),
			   "the temporal source initially displays its exact red frame");
			waitUntil([&]() { return !editor.previewTimer_->isActive(); });
			const qint64 oldTime = hoverTrack(cuts, cuts->sourceRectForTest(), 0.31);
			const qint64 blueTarget = hoverTrack(cuts, cuts->segmentRects()[1], 0.66);
			ok(!editor.shownExact_ && editor.shownSource_ == changingId && editor.shownMs_ == oldTime &&
			   editor.pendingSource_ == blueId && editor.pendingMs_ == blueTarget,
			   "real Source and Output hovers establish an old displayed frame and a newer pending video");
			emit editor.previewDecoder_->frameReady(changingId, oldTime);
			ok(editor.pendingSource_ == blueId && editor.pendingMs_ == blueTarget,
			   "an old decoder completion preserves the pending Output hover source and time");
			ok(waitUntil([&]() {
				const QImage &image = canvas->currentFrame();
				const QColor pixel = image.isNull() ? QColor() : image.pixelColor(image.width() / 2, image.height() / 2);
				return editor.shownSource_ == blueId && editor.shownMs_ == blueTarget && editor.shownExact_ &&
					pixel.blue() > 180 && pixel.red() < 40;
			}), "the pending Output hover reaches its actual blue frame without another pointer movement");
			editor.setActiveSource(changingId);
			waitUntil([&]() { return editor.shownSource_ == changingId && editor.shownExact_; });
			waitUntil([&]() { return !editor.previewTimer_->isActive(); });
			const qint64 earlier = hoverTrack(cuts, cuts->sourceRectForTest(), 0.37);
			const qint64 later = hoverTrack(cuts, cuts->sourceRectForTest(), 0.83);
			ok(!editor.shownExact_ && editor.shownMs_ == earlier && editor.pendingMs_ == later,
			   "two real Source hovers establish a newer pending time in the same video");
			emit editor.previewDecoder_->frameReady(changingId, earlier);
			ok(editor.pendingSource_ == changingId && editor.pendingMs_ == later,
			   "an older completion cannot roll back the same source's newer pending time");
			ok(waitUntil([&]() {
				const QImage &image = canvas->currentFrame();
				const QColor pixel = image.isNull() ? QColor() : image.pixelColor(image.width() / 2, image.height() / 2);
				return editor.shownSource_ == changingId && editor.shownMs_ == later && editor.shownExact_ &&
					pixel.green() > 180 && pixel.red() < 40 && pixel.blue() < 40;
			}), "the latest Source hover reaches its actual green frame without further pointer movement");
			ok(cuts->segments() == segments, "pending-hover convergence never changes the kept cuts");
		}
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
