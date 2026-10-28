// A speed change can ripple its own lane without compacting pre-existing gaps
// or overlaps. Exercise the same edit paths used by the Inspector and track menu.
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTimer>

#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *description)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
	failures += !condition;
}
TlClip video(qint64 start, qint64 length, int id)
{
	TlClip clip;
	clip.sourceId = id;
	clip.outStartMs = start;
	clip.srcEndMs = length;
	return clip;
}
TimelineModel fixture()
{
	TimelineModel model;
	TlTrack enabled;
	enabled.rippleSpeed = true;
	// Storage order deliberately differs from timeline order. The fourth
	// clip overlaps A by 200 ms; the fifth shares A's exact starting time.
	enabled.clips = {video(8000, 2000, 10), video(1000, 2000, 11), video(4000, 2000, 12),
			 video(2800, 1000, 13), video(1000, 500, 14)};
	model.tracks.push_back(enabled);
	TlTrack disabled;
	disabled.ripple = true; // deletion ripple is an independent preference
	disabled.clips = {video(1000, 2000, 20), video(4000, 2000, 21)};
	model.tracks.push_back(disabled);
	TlTrack locked = disabled;
	locked.rippleSpeed = true;
	locked.locked = true;
	model.tracks.push_back(locked);
	TlTrack audio = disabled;
	audio.kind = TlTrack::Kind::Audio;
	audio.rippleSpeed = true;
	model.tracks.push_back(audio);
	model.tracks[0].clips[1].link = model.tracks[3].clips[0].link = 7;
	model.markers = {3333};
	model.markerLabels.insert(3333, QStringLiteral("Keep here"));
	model.videoEndMs = 12000;
	return model;
}
void selectGroup(TimelineView &view)
{
	view.selectClip(0, 2);
	view.addToSelection(0, 1);
	view.addToSelection(0, 1); // selecting a member again must not double its ripple
	view.addToSelection(1, 0);
	view.addToSelection(2, 0);
}
QVector<qint64> starts(const TimelineView &view, int track)
{
	QVector<qint64> result;
	for (const TlClip &clip : view.model().tracks[track].clips)
		result.push_back(clip.outStartMs);
	return result;
}
struct MenuResult {
	bool found = false, checkable = false, checked = false, explained = false, timedOut = false;
};
MenuResult toggleMenu(TimelineView &view)
{
	MenuResult result;
	QTimer driver, watchdog;
	driver.setInterval(5);
	watchdog.setSingleShot(true);
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (!menu)
			return;
		for (QAction *action : menu->actions()) {
			if (action->objectName() != QStringLiteral("timelineTrackRippleSpeed"))
				continue;
			result.found = action->text() == QStringLiteral("Auto ripple on speed change");
			result.checkable = action->isCheckable();
			result.checked = action->isChecked();
			result.explained = action->toolTip().contains(QStringLiteral("following clips")) &&
				action->toolTip().contains(QStringLiteral("slower"));
			menu->setActiveAction(action);
			QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
			QApplication::sendEvent(menu, &enter);
			return;
		}
		menu->close();
	});
	QObject::connect(&watchdog, &QTimer::timeout, [&]() {
		result.timedOut = true;
		for (QWidget *widget : QApplication::topLevelWidgets())
			if (auto *menu = qobject_cast<QMenu *>(widget))
				menu->close();
	});
	driver.start();
	watchdog.start(3000);
	const QPoint at(15, view.clipRectForTest(0, 1).center().y());
	QMouseEvent right(QEvent::MouseButtonPress, QPointF(at), view.mapToGlobal(at), Qt::RightButton,
			  Qt::RightButton, Qt::NoModifier);
	QApplication::sendEvent(&view, &right);
	return result;
}
void mouse(TimelineView &view, QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons held)
{
	QMouseEvent event(type, QPointF(at), view.mapToGlobal(at), button, held, Qt::NoModifier);
	QApplication::sendEvent(&view, &event);
}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	TimelineView view;
	view.resize(1100, 400);
	view.setModel(fixture());
	view.show();
	QApplication::processEvents();
	QSignalSpy changed(&view, &TimelineView::clipsChanged);
	QSignalSpy committed(&view, &TimelineView::editCommitted);
	QSignalSpy selected(&view, &TimelineView::selectionChanged);
	selectGroup(view);
	const TlSelection selection = view.selectionState();
	selected.clear();
	view.applyToSelection([](TlClip &clip) { clip.speed = 2.0; });
	check(starts(view, 0) == QVector<qint64>{6000, 1000, 3000, 1800, 1000},
	      "multi-selected speed changes apply cumulative shifts once despite unordered clip storage");
	check(view.model().tracks[0].clips[3].outStartMs - view.model().tracks[0].clips[1].outEndMs() == -200 &&
	      view.model().tracks[0].clips[0].outStartMs - view.model().tracks[0].clips[2].outEndMs() == 2000,
	      "an incoming crossfade overlap and an intentional gap retain their original lengths");
	check(view.selectionState() == selection && selected.isEmpty() && view.model().tracks[0].clips[0].sourceId == 10,
	      "speed ripple preserves every clip index and the complete selection");
	check(changed.size() == 1 && committed.isEmpty(),
	      "a multi-clip Inspector edit emits one change for the existing continuous undo path");
	check(starts(view, 1) == QVector<qint64>{1000, 4000} && view.model().tracks[1].clips[0].speed == 2.0 &&
	      starts(view, 2) == QVector<qint64>{1000, 4000} && view.model().tracks[2].clips[0].speed == 1.0,
	      "opting out keeps later clips fixed and locked tracks reject the whole speed edit");
	check(starts(view, 3) == QVector<qint64>{1000, 4000} && view.model().markers == QVector<qint64>{3333} &&
	      view.model().videoEndMs == 12000 && view.model().markerLabels.value(3333) == QStringLiteral("Keep here"),
	      "linked clips on other tracks, markers and the explicit video end stay fixed");
	view.applyToSelection([](TlClip &clip) { clip.speed = 0.5; });
	check(starts(view, 0) == QVector<qint64>{12000, 1000, 6000, 4800, 1000},
	      "slowing selected clips pushes subsequent clips later by their duration growth");
	view.applyToSelection([](TlClip &clip) { clip.speed = 1.0; });
	check(starts(view, 0) == QVector<qint64>{8000, 1000, 4000, 2800, 1000},
	      "restoring the original speed restores the original timing without drift");
	for (int i = 0; i < 10; ++i)
		for (double speed : {2.0, 4.0, 0.5, 1.0})
			view.applyToSelection([speed](TlClip &clip) { clip.speed = speed; });
	check(starts(view, 0) == QVector<qint64>{8000, 1000, 4000, 2800, 1000} && view.selectionState() == selection,
	      "repeated slider updates use each current duration instead of repeatedly applying an old delta");
	const auto unchanged = starts(view, 0);
	view.applyToSelection([](TlClip &clip) { clip.speed = 1.0; });
	check(starts(view, 0) == unchanged, "a no-op speed assignment cannot move any clip");
	view.applyToSelection([](TlClip &clip) { clip.srcEndMs -= 100; });
	check(starts(view, 0) == unchanged, "a source trim through the generic selection edit never invokes speed ripple");

	view.setModel(fixture());
	view.selectClip(0, 1);
	TlClip copy = *view.selectedClipPtr();
	copy.speed = 2.0;
	changed.clear();
	view.updateSelectedClip(copy);
	check(starts(view, 0) == QVector<qint64>{7000, 1000, 3000, 1800, 1000} && changed.size() == 1,
	      "the Inspector's copied single-clip update uses the same speed-ripple path");
	copy = *view.selectedClipPtr();
	copy.srcEndMs -= 100;
	const auto beforeTrim = starts(view, 0);
	view.updateSelectedClip(copy);
	check(starts(view, 0) == beforeTrim, "a copied single-clip trim leaves all subsequent positions untouched");
	view.selectClip(2, 0);
	copy = *view.selectedClipPtr();
	copy.speed = 4.0;
	changed.clear();
	view.updateSelectedClip(copy);
	check(view.model().tracks[2].clips[0].speed == 1.0 && changed.isEmpty(),
	      "the copied Inspector path also honors a locked track without recording an edit");

	view.setModel(fixture());
	changed.clear();
	const int edited = view.editClipsAt({{0, 2}, {0, 1}}, [](const QPair<int, int> &, TlClip &clip) {
		clip.speed = 2.0;
	});
	check(edited == 2 && starts(view, 0) == QVector<qint64>{6000, 1000, 3000, 1800, 1000} && changed.size() == 1,
	      "indexed generic clip edits batch every duration delta before shifting recipients");
	const auto videoBefore = starts(view, 0);
	view.editClips({{3, 0}}, [](TlClip &clip) { clip.speed = 2.0; });
	check(starts(view, 3) == QVector<qint64>{1000, 3000} && starts(view, 0) == videoBefore,
	      "audio clips ripple only their own enabled track even when linked to video elsewhere");

	TimelineModel clamp;
	TlTrack clampTrack;
	clampTrack.rippleSpeed = true;
	clampTrack.clips = {video(0, 2000, 1), video(1800, 1000, 2), video(2000, 1000, 3)};
	clamp.tracks.push_back(clampTrack);
	view.setModel(clamp);
	view.selectClip(0, 0);
	view.applyToSelection([](TlClip &clip) { clip.speed = 20.0; });
	check(starts(view, 0) == QVector<qint64>{0, 0, 100},
	      "an overlap larger than the shortened clip clamps at zero instead of creating negative clip times");

	TimelineModel coincident;
	TlTrack coincidentTrack;
	coincidentTrack.rippleSpeed = true;
	coincidentTrack.clips = {video(0, 2000, 1), video(0, 500, 2), video(2000, 1000, 3)};
	coincident.tracks.push_back(coincidentTrack);
	view.setModel(coincident);
	view.selectClip(0, 1);
	view.applyToSelection([](TlClip &clip) { clip.speed = 2.0; });
	check(starts(view, 0) == QVector<qint64>{0, 0, 2000} && view.model().tracks[0].clips[1].outDurationMs() == 250,
	      "retiming a shorter coincident clip cannot pull following clips through an unchanged longer member");
	view.setModel(coincident);
	view.selectClip(0, 0);
	view.addToSelection(0, 1);
	view.applyToSelection([](TlClip &clip) { clip.speed = 2.0; });
	check(starts(view, 0) == QVector<qint64>{0, 0, 1000},
	      "retiming a coincident selection ripples its envelope duration once instead of summing every member");

	view.setModel(fixture());
	const auto beforeFit = starts(view, 0);
	committed.clear();
	check(view.fitClipToTimeline(0, 1) && view.model().tracks[0].clips[1].outStartMs == 0 &&
	      view.model().tracks[0].clips[1].outEndMs() == 10000 &&
	      view.model().tracks[0].clips[0].outStartMs == beforeFit[0] &&
	      view.model().tracks[0].clips[2].outStartMs == beforeFit[2] && committed.size() == 1,
	      "Fit to whole timeline retains its explicit placement without pushing its target end away");

	TimelineModel trim;
	TlTrack trimTrack;
	trimTrack.rippleSpeed = true;
	trimTrack.clips = {video(0, 2000, 1), video(4000, 2000, 2)};
	trim.tracks.push_back(trimTrack);
	view.setModel(trim);
	view.setSnapEnabled(false);
	QApplication::processEvents();
	const QRect rect = view.clipRectForTest(0, 0);
	const QPoint edge(rect.right() - 1, rect.center().y());
	mouse(view, QEvent::MouseButtonPress, edge, Qt::LeftButton, Qt::LeftButton);
	mouse(view, QEvent::MouseMove, edge - QPoint(35, 0), Qt::NoButton, Qt::LeftButton);
	mouse(view, QEvent::MouseButtonRelease, edge - QPoint(35, 0), Qt::LeftButton, Qt::NoButton);
	check(view.model().tracks[0].clips[0].srcEndMs < 2000 && view.model().tracks[0].clips[0].speed == 1.0 &&
	      view.model().tracks[0].clips[1].outStartMs == 4000,
	      "an ordinary timeline edge drag trims footage without moving later clips");

	TimelineModel menuModel = fixture();
	menuModel.tracks[0].rippleSpeed = false;
	view.setModel(menuModel);
	QApplication::processEvents();
	committed.clear();
	const MenuResult enabled = toggleMenu(view);
	check(enabled.found && enabled.checkable && !enabled.checked && enabled.explained && !enabled.timedOut &&
	      view.model().tracks[0].rippleSpeed && !view.model().tracks[0].ripple && committed.size() == 1,
	      "the track menu exposes an explained, independently checkable speed-ripple option as one undo edit");
	const MenuResult disabled = toggleMenu(view);
	check(disabled.checked && !disabled.timedOut && !view.model().tracks[0].rippleSpeed && committed.size() == 2,
	      "the track menu reflects the saved switch and can disable it again");
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
