// Marker interaction on the real timeline: navigation never edits clips,
// dragging is one undoable change, and an explicit end remains editable.
#include "editor/timeline/TimelineView.hpp"

#include <QAction>
#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace harpia;

static int failures = 0;
static void check(bool value, const char *message)
{
	std::printf("  %s %s\n", value ? "PASS" : "FAIL", message);
	if (!value)
		++failures;
}

static TimelineModel fixture()
{
	TimelineModel model;
	for (int lane = 0; lane < 3; ++lane) {
		TlTrack track;
		track.kind = lane == 2 ? TlTrack::Kind::Audio : TlTrack::Kind::Video;
		track.name = QStringLiteral("%1%2").arg(lane == 2 ? QStringLiteral("A") : QStringLiteral("V")).arg(lane + 1);
		TlClip clip;
		clip.type = TlClip::Type::Video;
		clip.sourceId = lane + 1;
		clip.srcEndMs = 20000;
		if (lane == 0) {
			clip.srcEndMs = 4000;
			track.clips.append(clip);
			clip.outStartMs = 12000;
			clip.srcEndMs = 8000;
		} else if (lane == 1) {
			clip.type = TlClip::Type::Text;
			clip.outStartMs = 2000;
			clip.srcEndMs = 6000;
			clip.text.text = QStringLiteral("A caption");
		} else {
			clip.peaks.fill(0.4f, 80);
		}
		track.clips.append(clip);
		model.tracks.append(track);
	}
	return model;
}

static void mouse(TimelineView &view, QEvent::Type type, QPoint point,
		  Qt::MouseButton button = Qt::LeftButton, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
	const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : button;
	QMouseEvent event(type, QPointF(point), view.mapToGlobal(point),
			 type == QEvent::MouseMove ? Qt::NoButton : button, held, modifiers);
	QApplication::sendEvent(&view, &event);
}

static void click(TimelineView &view, QPoint point)
{
	mouse(view, QEvent::MouseButtonPress, point);
	mouse(view, QEvent::MouseButtonRelease, point);
}

static qint64 pixelTolerance(const TimelineView &view)
{
	const int x = view.contentRectForTest().left();
	return std::max<qint64>(1, std::abs(view.msForX(x + 2) - view.msForX(x)));
}

struct MenuItems {
	bool shown = false;
	bool add = false;
	bool end = false;
	bool rename = false;
	bool remove = false;
	bool timedOut = false;
};

static MenuItems inspectMenu(TimelineView &view, QPoint point)
{
	MenuItems result;
	QTimer driver;
	driver.setInterval(5);
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (!menu)
			return;
		result.shown = true;
		for (const QAction *action : menu->actions()) {
			const QString text = action->text().toLower().remove(QLatin1Char('&'));
			if (!action->isEnabled())
				continue;
			result.add |= text.contains(QStringLiteral("add marker"));
			result.end |= text.contains(QStringLiteral("video end"));
			result.rename |= text.contains(QStringLiteral("rename"));
			result.remove |= text.contains(QStringLiteral("delete")) || text.contains(QStringLiteral("remove"));
		}
		menu->close();
	});
	QTimer watchdog;
	watchdog.setSingleShot(true);
	QObject::connect(&watchdog, &QTimer::timeout, [&]() {
		result.timedOut = true;
		for (QWidget *widget : QApplication::topLevelWidgets())
			if (auto *menu = qobject_cast<QMenu *>(widget))
				menu->close();
	});
	driver.start();
	watchdog.start(2000);
	mouse(view, QEvent::MouseButtonPress, point, Qt::RightButton);
	driver.stop();
	watchdog.stop();
	return result;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	TimelineView view;
	view.resize(1200, 290);
	view.show();
	QApplication::processEvents();
	QSignalSpy commits(&view, &TimelineView::editCommitted);
	QSignalSpy updates(&view, &TimelineView::clipsChanged);
	QSignalSpy scrubs(&view, &TimelineView::scrub);
	const TimelineModel original = fixture();
	const auto reset = [&]() {
		view.setModel(original);
		view.setSnapEnabled(true);
		view.clearPlayhead();
		QApplication::processEvents();
		commits.clear();
		updates.clear();
		scrubs.clear();
	};

	std::printf("\n-- named markers are project edits, rather than clip edits --\n");
	reset();
	view.addMarker(9000, QStringLiteral("Main section"));
	view.addMarker(3000, QStringLiteral("Intro"));
	check(view.model().markers == QVector<qint64>({3000, 9000}), "markers are kept in time order");
	check(view.model().markerLabels.value(3000) == QStringLiteral("Intro"), "the marker keeps its label");
	check(commits.count() == 2 && updates.count() == 2, "each addition is one finished edit");
	check(view.model().tracks == original.tracks, "adding markers preserves all video, text and audio clips");
	commits.clear();
	updates.clear();
	check(view.renameMarker(3000, QStringLiteral("Opening")), "an existing marker can be renamed");
	check(view.model().markerLabels.value(3000) == QStringLiteral("Opening"), "the new name replaces the old one");
	check(commits.count() == 1 && updates.count() == 1, "renaming is one finished edit");
	check(!view.renameMarker(3000, QStringLiteral("Opening")) && commits.count() == 1,
	      "repeating the same name does not create an undo entry");
	check(view.removeMarker(3000), "a marker can be deleted");
	check(!view.model().markers.contains(3000) && !view.model().markerLabels.contains(3000),
	      "deleting removes its time and label together");
	const int afterDelete = commits.count();
	check(!view.removeMarker(3000) && commits.count() == afterDelete, "deleting a missing marker is a no-op");

	std::printf("\n-- the video end changes output duration while retaining editable content --\n");
	reset();
	const QRect clipBeforeEnd = view.clipRectForTest(0, 1);
	view.setVideoEnd(7000);
	check(view.model().hasVideoEnd() && view.durationMs() == 7000, "the explicit end controls video duration");
	check(view.model().contentDurationMs() == 20000 && view.model().tracks == original.tracks,
	      "clips after the end remain in the project");
	check(view.clipRectForTest(0, 1) == clipBeforeEnd, "shortening the end keeps later clips visible and the axis stable");
	check(commits.count() == 1 && updates.count() == 1, "setting the end is one edit");
	view.setVideoEnd(7000);
	check(commits.count() == 1, "setting an unchanged end is a no-op");
	view.resetVideoEnd();
	check(!view.model().hasVideoEnd() && view.durationMs() == 20000, "resetting restores the automatic content end");
	check(commits.count() == 2 && view.model().tracks == original.tracks, "resetting the end is one non-destructive edit");

	std::printf("\n-- clicking a marker navigates; dragging commits once --\n");
	reset();
	view.addMarker(3000, QStringLiteral("Opening"));
	commits.clear();
	updates.clear();
	click(view, view.markerHandleRectForTest(3000).center());
	check(!scrubs.isEmpty() && scrubs.last().first().toLongLong() == 3000, "clicking the label seeks to the marker's time");
	check(commits.isEmpty() && updates.isEmpty(), "navigation does not change the project");
	view.setSnapEnabled(false);
	const QPoint from = view.markerHandleRectForTest(3000).center();
	const QPoint to = from + QPoint(view.xForMs(7000) - view.xForMs(3000), 0);
	mouse(view, QEvent::MouseButtonPress, from);
	for (int step = 1; step <= 5; ++step)
		mouse(view, QEvent::MouseMove, from + (to - from) * step / 5);
	check(view.model().markers == QVector<qint64>({3000}) && commits.isEmpty(),
	      "a drag previews its new position without changing saved data or undo history");
	mouse(view, QEvent::MouseButtonRelease, to);
	const qint64 moved = view.model().markers.value(0, -1);
	check(std::abs(moved - 7000) <= pixelTolerance(view), "the marker follows the grabbed label horizontally");
	check(view.model().markerLabels.value(moved) == QStringLiteral("Opening") &&
		      !view.model().markerLabels.contains(3000), "moving preserves the name and removes its old mapping");
	check(commits.count() == 1 && updates.count() == 1, "all drag moves become one finished edit");
	check(view.model().tracks == original.tracks, "marker dragging leaves every clip unchanged");

	std::printf("\n-- escape cancels a marker drag --\n");
	reset();
	view.addMarker(3000, QStringLiteral("Opening"));
	commits.clear();
	updates.clear();
	const QPoint cancelFrom = view.markerHandleRectForTest(3000).center();
	const QPoint cancelTo = cancelFrom + QPoint(100, 0);
	mouse(view, QEvent::MouseButtonPress, cancelFrom);
	mouse(view, QEvent::MouseMove, cancelTo);
	QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
	QApplication::sendEvent(&view, &escape);
	mouse(view, QEvent::MouseButtonRelease, cancelTo);
	check(view.model().markers == QVector<qint64>({3000}) &&
		      view.model().markerLabels.value(3000) == QStringLiteral("Opening"), "cancel restores the complete marker");
	check(commits.isEmpty() && updates.isEmpty(), "cancel has no edit to undo");

	std::printf("\n-- clips and markers share the magnet --\n");
	reset();
	view.addMarker(3000, QStringLiteral("Beat"));
	view.setVideoEnd(13500);
	const qint64 near = std::max<qint64>(1, pixelTolerance(view));
	check(view.snapForTest(3000 + near) == 3000, "clip edges can snap to an ordinary marker");
	check(view.snapForTest(13500 + near) == 13500, "clip edges can snap to the video end");
	view.setSnapEnabled(false);
	check(view.snapForTest(3000 + near) != 3000 && view.snapForTest(13500 + near) != 13500,
	      "turning snapping off disables both targets");
	view.setSnapEnabled(true);
	commits.clear();
	const QPoint snapFrom = view.markerHandleRectForTest(3000).center();
	const QPoint snapTo = snapFrom + QPoint(view.xForMs(12000) - view.xForMs(3000) + 2, 0);
	mouse(view, QEvent::MouseButtonPress, snapFrom);
	mouse(view, QEvent::MouseMove, snapTo);
	mouse(view, QEvent::MouseButtonRelease, snapTo);
	check(view.model().markers == QVector<qint64>({12000}), "a dragged marker snaps to an existing clip edge");
	check(commits.count() == 1, "a snapped drag remains one edit");
	reset();
	view.addMarker(3000, QStringLiteral("Beat"));
	commits.clear();
	const QPoint fineFrom = view.markerHandleRectForTest(3000).center();
	const QPoint fineTo = fineFrom + QPoint(view.xForMs(12000) - view.xForMs(3000) + 4, 0);
	const qint64 viewBeforeFine = view.viewStartMs();
	mouse(view, QEvent::MouseButtonPress, fineFrom, Qt::LeftButton, Qt::AltModifier);
	mouse(view, QEvent::MouseMove, fineTo, Qt::LeftButton, Qt::AltModifier);
	mouse(view, QEvent::MouseButtonRelease, fineTo, Qt::LeftButton, Qt::AltModifier);
	check(view.model().markers.value(0) > 12000 && view.viewStartMs() == viewBeforeFine,
	      "Alt dragging a marker bypasses the magnet instead of panning the timeline");
	check(commits.count() == 1, "fine marker movement remains one edit");

	std::printf("\n-- an automatic end does not pin its own last clip --\n");
	TimelineModel single;
	TlTrack singleTrack;
	singleTrack.kind = TlTrack::Kind::Video;
	TlClip singleClip;
	singleClip.type = TlClip::Type::Video;
	singleClip.sourceId = 1;
	singleClip.outStartMs = 5000;
	singleClip.srcEndMs = 5000;
	singleTrack.clips.append(singleClip);
	single.tracks.append(singleTrack);
	view.setModel(single);
	view.clearPlayhead();
	view.setSnapEnabled(true);
	commits.clear();
	const QPoint lastClipFrom = view.clipRectForTest(0, 0).center();
	const QPoint lastClipTo = lastClipFrom + QPoint(6, 0);
	mouse(view, QEvent::MouseButtonPress, lastClipFrom);
	mouse(view, QEvent::MouseMove, lastClipTo);
	mouse(view, QEvent::MouseButtonRelease, lastClipTo);
	check(view.model().tracks[0].clips[0].outStartMs > 5000,
	      "a small last-clip drag does not snap back to its own automatic end");
	check(commits.count() == 1, "the last-clip movement is still one finished edit");
	single.videoEndMs = 10000;
	view.setModel(single);
	view.clearPlayhead();
	const QPoint explicitClipFrom = view.clipRectForTest(0, 0).center();
	const QPoint explicitClipTo = explicitClipFrom + QPoint(6, 0);
	mouse(view, QEvent::MouseButtonPress, explicitClipFrom);
	mouse(view, QEvent::MouseMove, explicitClipTo);
	mouse(view, QEvent::MouseButtonRelease, explicitClipTo);
	check(view.model().tracks[0].clips[0].outStartMs == 5000,
	      "the same drag can deliberately snap to an explicitly placed video end");

	std::printf("\n-- the automatic end can be grabbed and made explicit --\n");
	reset();
	view.setSnapEnabled(false);
	const QPoint endFrom = view.markerHandleRectForTest(20000, true).center();
	const QPoint endTo = endFrom + QPoint(view.xForMs(10000) - view.xForMs(20000), 0);
	check(view.markerBandRectForTest().contains(endFrom), "the automatic end has a reachable ruler handle");
	mouse(view, QEvent::MouseButtonPress, endFrom);
	mouse(view, QEvent::MouseMove, endTo);
	check(!view.model().hasVideoEnd() && commits.isEmpty(), "end dragging is preview-only until release");
	mouse(view, QEvent::MouseButtonRelease, endTo);
	check(view.model().hasVideoEnd() && std::abs(view.durationMs() - 10000) <= pixelTolerance(view),
	      "dropping the end handle sets the video's end");
	check(commits.count() == 1 && view.model().tracks == original.tracks, "end dragging commits once and keeps all clips");
	reset();
	const QPoint zeroEndFrom = view.markerHandleRectForTest(20000, true).center();
	const QPoint zeroEndTo(0, zeroEndFrom.y());
	mouse(view, QEvent::MouseButtonPress, zeroEndFrom, Qt::LeftButton, Qt::AltModifier);
	mouse(view, QEvent::MouseMove, zeroEndTo, Qt::LeftButton, Qt::AltModifier);
	mouse(view, QEvent::MouseButtonRelease, zeroEndTo, Qt::LeftButton, Qt::AltModifier);
	check(view.model().hasVideoEnd() && view.durationMs() == 1,
	      "grabbing the left-sided End label and dragging into the gutter reaches the minimum time");
	check(commits.count() == 1 && view.model().tracks == original.tracks,
	      "the minimum end is one edit and retains the content for further editing");

	std::printf("\n-- the upper ruler adds markers; the lower ruler still scrubs --\n");
	reset();
	const QPoint addAt(view.xForMs(6000), view.markerBandRectForTest().center().y());
	mouse(view, QEvent::MouseButtonDblClick, addAt);
	mouse(view, QEvent::MouseButtonRelease, addAt);
	check(view.model().markers.size() == 1 &&
		      std::abs(view.model().markers.first() - 6000) <= pixelTolerance(view),
	      "double-clicking an empty marker row adds a marker at that position");
	check(commits.count() == 1, "the double click creates one undo entry");
	const int markerCount = view.model().markers.size();
	commits.clear();
	scrubs.clear();
	const QPoint scrubAt(view.xForMs(9000), view.markerBandRectForTest().bottom() + 4);
	click(view, scrubAt);
	check(!scrubs.isEmpty() && std::abs(scrubs.last().first().toLongLong() - 9000) <= pixelTolerance(view),
	      "the time-label band still seeks normally");
	check(view.model().markers.size() == markerCount && commits.isEmpty(), "ordinary scrubbing adds no marker or undo entry");

	std::printf("\n-- marker controls are discoverable from the ruler menu --\n");
	reset();
	view.addMarker(3000, QStringLiteral("Intro"));
	commits.clear();
	scrubs.clear();
	const MenuItems empty = inspectMenu(view, QPoint(view.xForMs(9000), view.markerBandRectForTest().center().y()));
	check(empty.shown && empty.add && empty.end && !empty.timedOut, "an empty ruler menu offers marker and video-end placement");
	const MenuItems existing = inspectMenu(view, view.markerHandleRectForTest(3000).center());
	check(existing.shown && existing.rename && existing.remove && !existing.timedOut, "an existing marker's menu offers rename and delete");
	check(commits.isEmpty() && scrubs.isEmpty(), "opening a menu neither edits nor seeks");

	std::printf("\n-- divider lines remain visible across every track --\n");
	reset();
	view.addMarker(3000, QStringLiteral("Introduction"));
	view.addMarker(10000, QStringLiteral("Main section"));
	view.setVideoEnd(16000);
	view.clearPlayhead();
	QEvent leave(QEvent::Leave);
	QApplication::sendEvent(&view, &leave);
	QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	view.render(&image);
	const int markerX = view.xForMs(3000);
	for (int lane = 0; lane < original.tracks.size(); ++lane) {
		const QRect track = view.clipRectForTest(lane, 0);
		int painted = 0;
		for (int y = track.top() + 1; y < track.bottom(); ++y)
			for (int x = markerX - 1; x <= markerX + 1; ++x)
				painted += image.pixelColor(x, y) == view.colors().marker;
		check(painted >= 3, "the marker divides a video, text or audio lane");
	}
	const int endX = view.xForMs(16000);
	int distinctEnd = 0;
	const QRect content = view.contentRectForTest();
	for (int y = content.top() + 2; y < content.bottom() - 2; ++y) {
		const QColor color = image.pixelColor(endX, y);
		if (color != view.colors().marker && color != image.pixelColor(endX - 3, y) &&
		    color != image.pixelColor(endX + 3, y))
			++distinctEnd;
	}
	check(distinctEnd > content.height() / 3, "the video end is a distinct continuous divider");
	const QString screenshot = qEnvironmentVariable("HARPIA_MARKERS_SCREENSHOT");
	if (!screenshot.isEmpty())
		check(image.save(screenshot), "the marker preview screenshot was saved");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
