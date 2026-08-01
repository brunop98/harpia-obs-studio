// The region overlay stays on screen while Custom Region is the capture mode.
//
// It used to vanish the moment focus left Harpia, which defeated the one job it
// exists for: lining the frame up against the app you are about to record.
// Clicking that app to bring it forward took the overlay away with it, so the
// region could only be adjusted against an empty desktop.
//
// Keeping it up is not simply "stop calling hide()", and the ways it goes wrong
// are all worse than the problem:
//
//   - an always-on-top window left over another app with a solid input area
//     makes that app unclickable -- the interior has to stop taking the mouse
//     the moment Harpia is not in front;
//   - showing an always-on-top window normally ACTIVATES it, so it would steal
//     the foreground back from whatever the user just clicked, and then bounce
//     it away again on the next activation change;
//   - re-masking during a drag pulls the interior out from under a move that
//     started there, and activation changes fire exactly then, because clicking
//     the overlay is what deactivates the main window.
#include "ui/RegionTool.hpp"

#include <QApplication>
#include <QMouseEvent>
#include <QScreen>
#include <QSignalSpy>

#include <cstdio>
#include <utility>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static RegionTool *build()
{
	auto *t = new RegionTool;
	t->setScreen(QGuiApplication::primaryScreen());
	t->setRegionDevicePx(QRect(200, 150, 640, 400));
	t->show();
	QApplication::processEvents();
	return t;
}

// Does the interior take the mouse?
//
// Read off the mask the widget ASKED for. The offscreen platform cannot apply a
// window mask -- it says so on stderr -- so this checks intent rather than what
// the compositor did with it. Intent is the part that can regress in code.
//
// A cleared mask means the whole widget is live (so the region can be dragged
// from the middle); a frame-only mask means clicks fall through to whatever is
// underneath.
static bool interiorGrabsMouse(RegionTool *t)
{
	const QRegion m = t->mask();
	if (m.isNull() || m.isEmpty())
		return true; // no mask = the whole widget is live
	return m.contains(t->rect().center());
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- the policy: Custom Region keeps the overlay on screen --\n");
	{
		// The fix itself, stated where it can be checked without a window.
		using M = RegionTool::Mode;
		const auto st = [](bool region, bool rec, bool focus, bool editor = false) {
			return regionOverlayState(region, rec, focus, editor);
		};

		// Entire Monitor: nothing to frame, so nothing on screen.
		ok(!st(false, false, true).visible, "Entire Monitor shows no overlay");
		ok(!st(false, true, true).visible, "not even while recording");

		// Custom Region with Harpia in front: fully interactive.
		ok(st(true, false, true).visible, "Custom Region shows it");
		ok(st(true, false, true).mode == M::Editing, "interactive while Harpia is in front");

		// THE change. Clicking the app you are about to record used to take the
		// overlay away with the focus, which is the whole complaint.
		const auto unfocused = st(true, false, false);
		ok(unfocused.visible, "and it STAYS on screen when another app takes focus");
		ok(unfocused.mode == M::Watching,
		   "in Watching, so that app is still clickable through the middle");

		// Recording wins over focus either way.
		ok(st(true, true, true).mode == M::Recording, "recording shows the recording state");
		ok(st(true, true, false).mode == M::Recording, "focused or not");

		// The editor is a window you work INSIDE, usually maximised. A frame
		// floating over it marks out a piece of desktop that has nothing to do
		// with what is being edited, and it covers the thing you are looking at.
		ok(!st(true, false, true, /*editor=*/true).visible,
		   "no overlay while the editor is open");
		ok(!st(true, false, false, true).visible, "whether or not Harpia is in front");
		// ...but not at the cost of hiding a recording in progress: there the
		// frame is saying "this is what is going into the file".
		ok(st(true, true, true, true).visible, "unless a recording is running");
		ok(st(true, true, true, true).mode == M::Recording, "still in the recording state");
	}

	std::printf("\n-- Editing: the whole rectangle is grabbable --\n");
	{
		RegionTool *t = build();
		t->setMode(RegionTool::Mode::Editing);
		QApplication::processEvents();
		ok(t->mode() == RegionTool::Mode::Editing, "it is in Editing mode");
		// The only mode where dragging the middle moves the region.
		ok(interiorGrabsMouse(t), "the interior takes the mouse, so it can be dragged");
		ok(t->windowOpacity() > 0.95, "and it is at full strength");
		t->deleteLater();
	}

	std::printf("\n-- Watching: still visible, but the app underneath stays usable --\n");
	{
		RegionTool *t = build();
		t->setMode(RegionTool::Mode::Watching);
		QApplication::processEvents();
		// The point of the change. Before, this state did not exist and the
		// overlay was simply hidden.
		ok(t->isVisible(), "the overlay is still on screen");
		// And the point of it being a separate state rather than just "shown":
		// a solid input area over someone else's window makes it unclickable.
		ok(!interiorGrabsMouse(t), "but the interior no longer takes the mouse");
		std::printf("     opacity %.2f\n", t->windowOpacity());
		// Readable enough to line up against what is underneath -- that is the
		// whole job -- but visibly stepped back from Editing.
		ok(t->windowOpacity() < 0.95, "it steps back, since Harpia is not in front");
		ok(t->windowOpacity() > 0.5, "without fading so far it cannot be aimed");
		t->deleteLater();
	}

	std::printf("\n-- Recording: same input rules, dimmer --\n");
	{
		RegionTool *t = build();
		t->setMode(RegionTool::Mode::Recording);
		QApplication::processEvents();
		ok(t->isVisible(), "shown as a boundary indicator");
		ok(!interiorGrabsMouse(t), "click-through, so the recorded app stays usable");
		std::printf("     opacity %.2f\n", t->windowOpacity());
		ok(t->windowOpacity() < 0.5, "and dimmer than Watching, since it is passive now");
		t->deleteLater();
	}

	std::printf("\n-- the border and handles stay live in every mode --\n");
	{
		// Whatever else changes, the frame has to remain grabbable: resizing the
		// region while another app is in front is exactly what this is for.
		for (auto m : {RegionTool::Mode::Editing, RegionTool::Mode::Watching,
			       RegionTool::Mode::Recording}) {
			RegionTool *t = build();
			t->setMode(m);
			QApplication::processEvents();
			const QRegion mask = t->mask();
			// A point on the frame itself, a couple of pixels in from the edge.
			const QPoint onFrame(t->width() / 2, 2);
			const bool live = mask.isNull() || mask.isEmpty() || mask.contains(onFrame);
			ok(live, "the frame takes the mouse in this mode");
			t->deleteLater();
		}
	}

	std::printf("\n-- a drag is not interrupted by a mode change --\n");
	{
		// Clicking the overlay deactivates the main window, which is what
		// triggers the mode change -- so the two collide precisely when a drag
		// begins. The owner asks isInteracting() and defers; this is the flag it
		// reads, and the signal that tells it to look again.
		RegionTool *t = build();
		t->setMode(RegionTool::Mode::Editing);
		QApplication::processEvents();
		ok(!t->isInteracting(), "nothing in flight to begin with");

		QSignalSpy finished(t, &RegionTool::interactionFinished);
		const QPoint mid = t->rect().center();
		QMouseEvent press(QEvent::MouseButtonPress, QPointF(mid), t->mapToGlobal(mid),
				  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(t, &press);
		ok(t->isInteracting(), "pressing the interior starts an interaction");

		const QPoint to = mid + QPoint(40, 25);
		QMouseEvent move(QEvent::MouseMove, QPointF(to), t->mapToGlobal(to), Qt::NoButton,
				 Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(t, &move);
		ok(t->isInteracting(), "and it is still in flight while the mouse moves");

		QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(to), t->mapToGlobal(to),
				Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(t, &rel);
		QApplication::processEvents();
		ok(!t->isInteracting(), "releasing ends it");
		std::printf("     interactionFinished emitted %d time(s)\n", int(finished.count()));
		// Without this the owner never re-evaluates: it skipped the change that
		// arrived mid-drag, and nothing else would arrive.
		ok(finished.count() == 1, "and it says so exactly once, so the owner looks again");

		// A release with no drag behind it must NOT fire -- otherwise every
		// stray click re-evaluates the mode.
		QSignalSpy again(t, &RegionTool::interactionFinished);
		QMouseEvent stray(QEvent::MouseButtonRelease, QPointF(to), t->mapToGlobal(to),
				  Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(t, &stray);
		QApplication::processEvents();
		ok(again.count() == 0, "a release with no drag behind it says nothing");
		t->deleteLater();
	}

	// ------------------------------------------------------------------------
	// The move handle.
	//
	// Zone::Move is only returned for a point INSIDE the region, and Watching and
	// Recording mask the interior out so the app underneath stays clickable. So
	// the region was resizable but not MOVABLE in the two states that matter:
	// lining the frame up against the app you are about to record (which means
	// that app is in front), and while recording. The tab lives outside the
	// region so it can stay in the input mask without covering a captured pixel.
	// ------------------------------------------------------------------------

	std::printf("\n-- off unless the preset asks for it --\n");
	{
		RegionTool *t = build();
		ok(!t->moveHandleEnabled(), "no handle by default");
		ok(t->moveHandleRect().isNull(), "and nothing drawn where it would go");
		t->deleteLater();
	}

	std::printf("\n-- turning it on does not move the region --\n");
	{
		// The tab needs room above the frame, so the WIDGET grows. The region
		// must not. Get this wrong -- have innerRectLocal() and applyGeometry()
		// disagree by the tab's height -- and every toggle silently rewrites the
		// user's region through regionChanged(), 27 px at a time.
		RegionTool *t = build();
		const CaptureRegion before = t->region();
		const int hBefore = t->height();
		t->setMoveHandleEnabled(true);
		QApplication::processEvents();
		const CaptureRegion after = t->region();
		std::printf("     widget %d -> %d px tall; region %dx%d at (%d,%d) -> %dx%d at (%d,%d)\n",
			    hBefore, t->height(), before.width, before.height, before.x, before.y,
			    after.width, after.height, after.x, after.y);
		ok(t->height() > hBefore, "the widget grew, so there is somewhere to put the tab");
		ok(before.x == after.x && before.y == after.y && before.width == after.width &&
			   before.height == after.height,
		   "and the region is exactly where it was");
		ok(!t->moveHandleRect().isNull(), "the tab has a place now");
		// Above the frame, not over it: covering captured pixels is the thing the
		// tab exists to avoid, which is why it went outside rather than inside.
		const int tabBottomGlobal = t->mapToGlobal(t->moveHandleRect().bottomLeft()).y();
		const int regionTopGlobal = QGuiApplication::primaryScreen()->geometry().top() + after.y;
		std::printf("     tab bottom at y=%d, region starts at y=%d\n", tabBottomGlobal,
			    regionTopGlobal);
		ok(tabBottomGlobal < regionTopGlobal, "and it sits clear above the captured area");
		ok(t->rect().contains(t->moveHandleRect()), "while staying inside the widget");
		t->deleteLater();
	}

	// A region with room to move on every side. build()'s is wide enough that
	// applyGeometry() clamps it against the right edge of the test screen, so a
	// rightward drag would be swallowed by the clamp and prove nothing.
	const auto buildSmall = []() {
		auto *t = new RegionTool;
		t->setScreen(QGuiApplication::primaryScreen());
		t->setRegionDevicePx(QRect(200, 200, 300, 200));
		t->show();
		QApplication::processEvents();
		return t;
	};

	// Drag the tab from `from` and report how far the region moved.
	const auto dragBy = [](RegionTool *t, const QPoint &from, const QPoint &delta) {
		const CaptureRegion before = t->region();
		QMouseEvent press(QEvent::MouseButtonPress, QPointF(from), t->mapToGlobal(from),
				  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(t, &press);
		const QPoint to = from + delta;
		QMouseEvent move(QEvent::MouseMove, QPointF(to), t->mapToGlobal(to), Qt::NoButton,
				 Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(t, &move);
		QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(to), t->mapToGlobal(to), Qt::LeftButton,
				Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(t, &rel);
		QApplication::processEvents();
		const CaptureRegion after = t->region();
		return std::make_pair(after, before);
	};

	std::printf("\n-- the tab moves the region while another app is in front --\n");
	{
		RegionTool *t = buildSmall();
		t->setMoveHandleEnabled(true);
		t->setMode(RegionTool::Mode::Watching);
		QApplication::processEvents();
		const QRect tab = t->moveHandleRect();
		// Where the tab is on screen BEFORE the drag -- the widget is about to
		// move, and the control below compares against this spot.
		const QPoint tabGlobal = t->mapToGlobal(tab.center());
		const auto [after, before] = dragBy(t, tab.center(), QPoint(40, 25));
		std::printf("     (%d,%d) -> (%d,%d), size %dx%d -> %dx%d\n", before.x, before.y, after.x,
			    after.y, before.width, before.height, after.width, after.height);
		ok(after.x == before.x + 40 && after.y == before.y + 25,
		   "dragging the tab moves the region by exactly the drag");
		ok(after.width == before.width && after.height == before.height, "and does not resize it");

		// CONTROL. Not "the same drag with the handle off moves nothing" -- that
		// cannot be tested this way and would be a lie if written. What stops the
		// interior taking the mouse in Watching is the WINDOW MASK, and a
		// synthetic event sent straight to the widget never consults it; the
		// offscreen platform cannot apply one either. So the mask is checked as a
		// mask, two sections down.
		//
		// What IS true here: with the handle off the widget does not extend far
		// enough above the frame for that point to exist at all, so a real
		// windowing system delivers nothing there.
		RegionTool *off = buildSmall();
		off->setMode(RegionTool::Mode::Watching);
		QApplication::processEvents();
		const QPoint inOff = off->mapFromGlobal(tabGlobal);
		std::printf("     control: the tab's spot lands at y=%d in a handle-off widget "
			    "(0..%d is inside it)\n",
			    inOff.y(), off->height() - 1);
		ok(!off->rect().contains(inOff),
		   "CONTROL: with the handle off there is no widget where the tab would be");
		t->deleteLater();
		off->deleteLater();
	}

	std::printf("\n-- and while recording, without changing the size --\n");
	{
		// Moving mid-recording is the safe half of live editing: the output
		// canvas is fixed when recording starts, so a move just slides the crop
		// and still fills the frame. A RESIZE would not, which is why the size
		// being untouched is asserted rather than assumed.
		RegionTool *t = buildSmall();
		t->setMoveHandleEnabled(true);
		t->setMode(RegionTool::Mode::Recording);
		QApplication::processEvents();
		const auto [after, before] = dragBy(t, t->moveHandleRect().center(), QPoint(-30, 20));
		std::printf("     (%d,%d) -> (%d,%d), size %dx%d -> %dx%d\n", before.x, before.y, after.x,
			    after.y, before.width, before.height, after.width, after.height);
		ok(after.x == before.x - 30 && after.y == before.y + 20, "the region moved while recording");
		ok(after.width == before.width && after.height == before.height,
		   "at exactly the same size, so the encode canvas still matches");
		t->deleteLater();
	}

	std::printf("\n-- the tab is grabbable, the empty space around it is not --\n");
	{
		// The widget now extends well above the frame to make room for the tab.
		// If that whole band took the mouse it would block clicks on whatever
		// sits above the region -- the exact failure the interior mask exists to
		// prevent, moved to a new place.
		RegionTool *t = build();
		t->setMoveHandleEnabled(true);
		t->setMode(RegionTool::Mode::Watching);
		QApplication::processEvents();
		const QRegion mask = t->mask();
		const QRect tab = t->moveHandleRect();
		const bool tabLive = mask.isNull() || mask.isEmpty() || mask.contains(tab.center());
		// Same height as the tab, but off to the side of it.
		const QPoint beside(6, tab.center().y());
		const bool besideLive = mask.isNull() || mask.isEmpty() || mask.contains(beside);
		std::printf("     tab live: %s   band beside it live: %s\n", tabLive ? "yes" : "no",
			    besideLive ? "yes" : "no");
		ok(tabLive, "the tab takes the mouse");
		ok(!besideLive, "the empty band beside it does not, so clicks above the region get through");
		t->deleteLater();
	}

	std::printf("\n-- a region at the top of the screen still gets a reachable tab --\n");
	{
		// "Above the frame" runs out of screen for a region snapped to the top,
		// and a tab hanging off the edge of the display cannot be grabbed at all
		// -- which would take the feature away in exactly the case someone
		// recording a full-width window at the top of their screen hits first.
		RegionTool *t = new RegionTool;
		t->setScreen(QGuiApplication::primaryScreen());
		t->setRegionDevicePx(QRect(200, 0, 300, 200));
		t->show();
		t->setMoveHandleEnabled(true);
		QApplication::processEvents();
		const QRect tab = t->moveHandleRect();
		const int tabTopGlobal = t->mapToGlobal(tab.topLeft()).y();
		const int screenTop = QGuiApplication::primaryScreen()->geometry().top();
		std::printf("     region at the very top; tab top at y=%d, screen starts at y=%d\n",
			    tabTopGlobal, screenTop);
		ok(tabTopGlobal >= screenTop, "the tab stayed on screen");
		ok(t->rect().contains(tab), "and inside the widget, so it is in the mask");

		// And it still moves the region -- flipping it inside must not cost it
		// its job, and it now overlaps the Top resize zone, which it has to win.
		t->setMode(RegionTool::Mode::Watching);
		QApplication::processEvents();
		const auto [after, before] = dragBy(t, tab.center(), QPoint(25, 30));
		std::printf("     (%d,%d) -> (%d,%d), size %dx%d -> %dx%d\n", before.x, before.y, after.x,
			    after.y, before.width, before.height, after.width, after.height);
		ok(after.x == before.x + 25 && after.y == before.y + 30, "and it still moves the region");
		ok(after.width == before.width && after.height == before.height,
		   "rather than resizing from the top edge it now sits on");
		t->deleteLater();
	}

	// ------------------------------------------------------------------------
	// The Record button below the frame.
	//
	// Starting a recording meant going back to the main window -- the one step of
	// a region recording that pulled you away from the thing you were framing.
	// Like the move tab, it lives outside the region so it covers no captured
	// pixel and stays in the input mask while another app is in front.
	// ------------------------------------------------------------------------

	std::printf("\n-- it is below the frame, and gone while recording --\n");
	{
		RegionTool *t = buildSmall();
		QApplication::processEvents();
		ok(t->startButtonVisible(), "there is a Record button when there is no recording");
		const QRect btn = t->startButtonRect();
		ok(!btn.isNull(), "and it has somewhere to be");
		ok(t->rect().contains(btn), "inside the widget, so it can be in the mask");

		const CaptureRegion r = t->region();
		const int regionBottomGlobal =
			QGuiApplication::primaryScreen()->geometry().top() + r.y + r.height;
		const int btnTopGlobal = t->mapToGlobal(btn.topLeft()).y();
		std::printf("     region ends at y=%d, button starts at y=%d\n", regionBottomGlobal,
			    btnTopGlobal);
		ok(btnTopGlobal >= regionBottomGlobal, "clear below the captured area, not over it");

		// A green "start" beside a running recording would say the opposite of
		// the truth, and Stop is not here -- it is on the floating controls.
		t->setMode(RegionTool::Mode::Recording);
		QApplication::processEvents();
		ok(!t->startButtonVisible(), "and it is gone once recording");
		ok(t->startButtonRect().isNull(), "with nothing left where it was");
		t->deleteLater();
	}

	std::printf("\n-- pressing it asks to record, and moves nothing --\n");
	{
		RegionTool *t = buildSmall();
		t->setMoveHandleEnabled(true);
		t->setMode(RegionTool::Mode::Watching);
		QApplication::processEvents();
		QSignalSpy started(t, &RegionTool::startRecordingRequested);
		QSignalSpy finished(t, &RegionTool::interactionFinished);

		const QRect btn = t->startButtonRect();
		const CaptureRegion before = t->region();
		const QPoint c = btn.center();
		QMouseEvent press(QEvent::MouseButtonPress, QPointF(c), t->mapToGlobal(c), Qt::LeftButton,
				  Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(t, &press);
		QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(c), t->mapToGlobal(c), Qt::LeftButton,
				Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(t, &rel);
		QApplication::processEvents();
		const CaptureRegion after = t->region();

		std::printf("     startRecordingRequested x%d; region (%d,%d) -> (%d,%d)\n",
			    int(started.count()), before.x, before.y, after.x, after.y);
		ok(started.count() == 1, "one press, one request to start");
		ok(after.x == before.x && after.y == before.y && after.width == before.width &&
			   after.height == before.height,
		   "and the region did not budge -- a button is not a drag handle");
		// The owner re-evaluates the overlay's mode on interactionFinished. A
		// button press changed no geometry, so making it fire would be work for
		// nothing, four times over on a double click.
		ok(finished.count() == 0, "pressing a button is not an interaction to finish");

		// CONTROL: the same gesture on the MOVE TAB does move the region. Without
		// it, "the region did not budge" would also pass on a build where nothing
		// responds to the mouse at all.
		const QPoint tabC = t->moveHandleRect().center();
		const auto [afterTab, beforeTab] = dragBy(t, tabC, QPoint(20, 15));
		std::printf("     control: the move tab, same gesture: (%d,%d) -> (%d,%d)\n", beforeTab.x,
			    beforeTab.y, afterTab.x, afterTab.y);
		ok(afterTab.x == beforeTab.x + 20 && afterTab.y == beforeTab.y + 15,
		   "CONTROL: the move tab still moves it, so the mouse IS being handled");
		ok(started.count() == 1, "and dragging the tab did not ask to record");
		t->deleteLater();
	}

	std::printf("\n-- a press you slide off is cancelled --\n");
	{
		// The ordinary button contract. Starting a recording by accident is not a
		// thing to shrug at -- it is a file, a countdown and a running encoder.
		RegionTool *t = buildSmall();
		t->setMode(RegionTool::Mode::Watching);
		QApplication::processEvents();
		QSignalSpy started(t, &RegionTool::startRecordingRequested);

		const QRect btn = t->startButtonRect();
		const QPoint c = btn.center();
		const QPoint away = c - QPoint(0, btn.height() * 3); // up into the region
		QMouseEvent press(QEvent::MouseButtonPress, QPointF(c), t->mapToGlobal(c), Qt::LeftButton,
				  Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(t, &press);
		QMouseEvent move(QEvent::MouseMove, QPointF(away), t->mapToGlobal(away), Qt::NoButton,
				 Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(t, &move);
		QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(away), t->mapToGlobal(away),
				Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(t, &rel);
		QApplication::processEvents();
		std::printf("     pressed the button, released elsewhere: %d request(s)\n",
			    int(started.count()));
		ok(started.count() == 0, "sliding off before letting go starts nothing");

		// And the slide itself must not have dragged the region along with it.
		const CaptureRegion r = t->region();
		ok(r.x == 200 && r.y == 200, "nor did the slide move the region");
		t->deleteLater();
	}

	std::printf("\n-- the button is grabbable, the band beside it is not --\n");
	{
		RegionTool *t = buildSmall();
		t->setMode(RegionTool::Mode::Watching);
		QApplication::processEvents();
		const QRegion mask = t->mask();
		const QRect btn = t->startButtonRect();
		const auto live = [&](const QPoint &p) {
			return mask.isNull() || mask.isEmpty() || mask.contains(p);
		};
		std::printf("     button live: %s   band beside it live: %s\n",
			    live(btn.center()) ? "yes" : "no",
			    live(QPoint(6, btn.center().y())) ? "yes" : "no");
		ok(live(btn.center()), "the button takes the mouse with another app in front");
		ok(!live(QPoint(6, btn.center().y())),
		   "and the reserved band beside it does not block clicks below the region");

		// Once recording, the button is gone and its space must stop grabbing.
		t->setMode(RegionTool::Mode::Recording);
		QApplication::processEvents();
		const QRegion recMask = t->mask();
		const bool stillLive = recMask.isNull() || recMask.isEmpty() || recMask.contains(btn.center());
		std::printf("     while recording, that same spot live: %s\n", stillLive ? "yes" : "no");
		ok(!stillLive, "with no button there, the space stops taking the mouse");
		t->deleteLater();
	}

	// ------------------------------------------------------------------------
	// While recording, the frame moves but does not resize.
	//
	// The output canvas is fixed when recording starts, so the encoder is
	// committed to one frame size for the whole file. Moving keeps that size and
	// slides the crop. Resizing does not -- the new rectangle is scaled into the
	// original canvas, so the picture silently stretches part-way through, and
	// nothing can undo that afterwards.
	// ------------------------------------------------------------------------

	std::printf("\n-- recording: grabbing a handle moves, it does not resize --\n");
	{
		using Z = RegionTool::Zone;
		RegionTool *t = buildSmall();
		t->setMode(RegionTool::Mode::Recording);
		QApplication::processEvents();

		// Every handle now reports Move. Stated per-corner because "the corner
		// resizes" is exactly the case that survives a half-done fix.
		const QRect inner = t->rect().adjusted(10, 37, -10, -37);
		struct Spot {
			const char *name;
			QPoint p;
		};
		const Spot spots[] = {
			{"top-left", inner.topLeft()},        {"top-right", inner.topRight()},
			{"bottom-left", inner.bottomLeft()},  {"bottom-right", inner.bottomRight()},
			{"left edge", QPoint(inner.left(), inner.center().y())},
			{"bottom edge", QPoint(inner.center().x(), inner.bottom())},
		};
		bool allMove = true;
		for (const Spot &s : spots)
			if (t->zoneAtForTest(s.p) != Z::Move) {
				std::printf("     %s still resizes\n", s.name);
				allMove = false;
			}
		ok(allMove, "all eight handles report Move while recording");

		// And it really is a move: the size comes out identical.
		const auto [after, before] = dragBy(t, inner.bottomRight(), QPoint(35, 25));
		std::printf("     dragged the bottom-right corner: %dx%d at (%d,%d) -> %dx%d at (%d,%d)\n",
			    before.width, before.height, before.x, before.y, after.width, after.height,
			    after.x, after.y);
		ok(after.width == before.width && after.height == before.height,
		   "dragging a corner leaves the size alone, so the encode canvas still fits");
		ok(after.x == before.x + 35 && after.y == before.y + 25, "it moved instead");
		t->deleteLater();
	}

	std::printf("\n-- CONTROL: the same corner still resizes when NOT recording --\n");
	{
		// Without this the section above would pass on a build where the corners
		// stopped doing anything at all, which would be a different bug.
		RegionTool *t = buildSmall();
		t->setMode(RegionTool::Mode::Watching);
		QApplication::processEvents();
		const QRect inner = t->rect().adjusted(10, 37, -10, -37);
		ok(t->zoneAtForTest(inner.bottomRight()) == RegionTool::Zone::BottomRight,
		   "idle, the bottom-right corner is a resize corner");
		const auto [after, before] = dragBy(t, inner.bottomRight(), QPoint(35, 25));
		std::printf("     %dx%d -> %dx%d\n", before.width, before.height, after.width, after.height);
		ok(after.width != before.width || after.height != before.height,
		   "CONTROL: and dragging it really does resize");
		t->deleteLater();
	}

	std::printf("\n-- the pointer never promises what the press will not do --\n");
	{
		using Z = RegionTool::Zone;
		using RT = RegionTool;
		// The mapping, stated once. Derived from the zone rather than worked out
		// separately, so it cannot drift out of step with the hit test.
		ok(RT::cursorForZone(Z::Move) == Qt::SizeAllCursor, "Move shows the move arrows");
		ok(RT::cursorForZone(Z::StartButton) == Qt::PointingHandCursor, "the button, a hand");
		ok(RT::cursorForZone(Z::Left) == Qt::SizeHorCursor, "a side edge, a horizontal arrow");
		ok(RT::cursorForZone(Z::Bottom) == Qt::SizeVerCursor, "a top/bottom edge, a vertical one");
		ok(RT::cursorForZone(Z::TopLeft) == Qt::SizeFDiagCursor, "and the corners, diagonals");
		ok(RT::cursorForZone(Z::TopRight) == Qt::SizeBDiagCursor, "the other way for the other pair");
		ok(RT::cursorForZone(Z::None) == Qt::ArrowCursor, "and nothing in particular, an arrow");

		// The point of the change: over a corner, the pointer says "resize" when
		// idle and "move" while recording, because that is what a press does.
		RegionTool *t = buildSmall();
		const QRect inner = t->rect().adjusted(10, 37, -10, -37);
		const QPoint corner = inner.bottomRight();
		const auto hoverShape = [&](RegionTool::Mode m) {
			t->setMode(m);
			QApplication::processEvents();
			QMouseEvent mv(QEvent::MouseMove, QPointF(corner), t->mapToGlobal(corner),
				       Qt::NoButton, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(t, &mv);
			QApplication::processEvents();
			return t->cursor().shape();
		};
		const Qt::CursorShape idle = hoverShape(RegionTool::Mode::Watching);
		const Qt::CursorShape rec = hoverShape(RegionTool::Mode::Recording);
		std::printf("     over the same corner: idle=%d recording=%d (SizeFDiag=%d SizeAll=%d)\n",
			    int(idle), int(rec), int(Qt::SizeFDiagCursor), int(Qt::SizeAllCursor));
		ok(idle == Qt::SizeFDiagCursor, "hovering a corner while idle offers a resize");
		ok(rec == Qt::SizeAllCursor, "and while recording it offers the move arrows instead");
		t->deleteLater();
	}

	std::printf("\n-- with the handle on, the frame still behaves --\n");
	{
		// The mask was rewritten to build up from the frame band instead of
		// subtracting from the whole widget, so the two properties the earlier
		// sections pin are worth re-checking in the new configuration.
		for (auto m : {RegionTool::Mode::Editing, RegionTool::Mode::Watching,
			       RegionTool::Mode::Recording}) {
			RegionTool *t = build();
			t->setMoveHandleEnabled(true);
			t->setMode(m);
			QApplication::processEvents();
			const QRegion mask = t->mask();
			// A point on the frame's top edge, below the tab band.
			const QPoint onFrame(t->width() / 2, t->moveHandleRect().bottom() + 12);
			ok(mask.isNull() || mask.isEmpty() || mask.contains(onFrame),
			   "the frame takes the mouse in this mode");
			t->deleteLater();
		}
		RegionTool *t = build();
		t->setMoveHandleEnabled(true);
		QApplication::processEvents();
		const CaptureRegion before = t->region();
		for (auto m : {RegionTool::Mode::Watching, RegionTool::Mode::Recording,
			       RegionTool::Mode::Editing}) {
			t->setMode(m);
			QApplication::processEvents();
		}
		const CaptureRegion after = t->region();
		ok(before.x == after.x && before.y == after.y && before.width == after.width &&
			   before.height == after.height,
		   "and mode changes still leave the region alone");
		t->deleteLater();
	}

	std::printf("\n-- switching modes does not move the region --\n");
	{
		// The frame is the user's setting. Changing how it takes the mouse must
		// not nudge where it is.
		RegionTool *t = build();
		t->setMode(RegionTool::Mode::Editing);
		QApplication::processEvents();
		const CaptureRegion before = t->region();
		for (auto m : {RegionTool::Mode::Watching, RegionTool::Mode::Recording,
			       RegionTool::Mode::Editing}) {
			t->setMode(m);
			QApplication::processEvents();
		}
		const CaptureRegion after = t->region();
		std::printf("     %dx%d at (%d,%d) -> %dx%d at (%d,%d)\n", before.width, before.height,
			    before.x, before.y, after.width, after.height, after.x, after.y);
		ok(before.x == after.x && before.y == after.y && before.width == after.width &&
			   before.height == after.height,
		   "the region is exactly where it was");
		t->deleteLater();
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
