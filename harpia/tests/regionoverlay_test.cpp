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
