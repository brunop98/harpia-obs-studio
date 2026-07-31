// The floating overview: where am I in a long project?
//
// Zoomed into a long timeline, nothing on screen says which part of the video
// is in front of you. The overview draws the whole project as one bar with the
// visible slice as a red rectangle. It is worth nothing unless that rectangle
// is in the RIGHT PLACE, so most of this is about the mapping:
//
//   * zoomed out, the box covers everything -- it must not vanish or shrink;
//   * zoomed in at the start / middle / end, it lands where it should, and the
//     one at the end really does touch the end;
//   * at extreme zoom on a long project the true width rounds to zero, and a
//     marker you cannot see is a bug, not a detail;
//   * a start beyond what the zoom allows is clamped rather than drawn outside.
//
// Then two things about the widget itself, since "only visible while I am
// interacting" is half the feature: it starts invisible, appears when told,
// and goes away on its own. And a render check, because a rectangle that is
// computed correctly and painted somewhere else is still wrong.
#include "editor/TimelineOverview.hpp"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
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

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	// A bar 400 px wide starting at x=10, as the widget's inset produces.
	const QRect bar(10, 4, 400, 18);
	const qint64 hour = 60 * 60 * 1000;

	std::printf("\n-- the red box lands where the view is --\n");
	{
		// Zoomed right out: the viewport IS the project.
		const QRect all = TimelineOverview::viewportRect(hour, 0, hour, bar);
		ok(all == bar, "zoomed out, the box covers the whole bar");
		ok(TimelineOverview::viewportRect(hour, 0, hour * 2, bar) == bar,
		   "and a viewport larger than the project still covers it, not more");

		// A quarter of the project, at the start, the middle and the end.
		const QRect head = TimelineOverview::viewportRect(hour, 0, hour / 4, bar);
		std::printf("     first quarter -> x=%d w=%d (bar x=%d w=%d)\n", head.x(), head.width(),
			    bar.x(), bar.width());
		ok(head.x() == bar.x(), "the first quarter starts at the left edge");
		ok(std::abs(head.width() - bar.width() / 4) <= 1, "and is a quarter wide");

		const QRect mid = TimelineOverview::viewportRect(hour, hour * 3 / 8, hour / 4, bar);
		ok(std::abs(mid.center().x() - bar.center().x()) <= 1,
		   "a viewport centred in the project draws centred in the bar");

		const QRect tail = TimelineOverview::viewportRect(hour, hour * 3 / 4, hour / 4, bar);
		std::printf("     last quarter  -> x=%d w=%d, bar right=%d\n", tail.x(), tail.width(),
			    bar.right());
		ok(tail.right() == bar.right(), "the last quarter really reaches the end");

		// Ordering across the project, which is the property that makes it
		// readable at a glance rather than any single case.
		bool monotonic = true;
		int prev = -1;
		for (int i = 0; i <= 10; ++i) {
			const qint64 st = (hour - hour / 10) * i / 10;
			const int x = TimelineOverview::viewportRect(hour, st, hour / 10, bar).x();
			if (x < prev)
				monotonic = false;
			prev = x;
		}
		ok(monotonic, "the box only moves right as the view moves later");
	}

	std::printf("\n-- deep zoom on a long project still shows something --\n");
	{
		// 400 px for an hour is 9 seconds a pixel. A two-second viewport is a
		// fifth of a pixel: rounded honestly it disappears, which is the one
		// outcome that makes the whole feature useless.
		const QRect tiny = TimelineOverview::viewportRect(hour, hour / 2, 2000, bar);
		std::printf("     2s of an hour across %d px -> w=%d\n", bar.width(), tiny.width());
		ok(tiny.width() >= 3, "a very small viewport is still drawn wide enough to see");
		ok(bar.contains(tiny), "and stays inside the bar");

		// Including at the very end, where widening it naively would push it out.
		const QRect end = TimelineOverview::viewportRect(hour, hour - 2000, 2000, bar);
		std::printf("     the same at the very end -> x=%d w=%d, bar right=%d\n", end.x(),
			    end.width(), bar.right());
		ok(bar.contains(end), "the last sliver is widened inwards, not off the end");
		ok(end.right() == bar.right(), "and still reads as being at the end");
	}

	std::printf("\n-- nonsense in, nothing out --\n");
	{
		ok(TimelineOverview::viewportRect(0, 0, 1000, bar).isEmpty(),
		   "a project with no duration has no box");
		ok(TimelineOverview::viewportRect(hour, 0, hour / 4, QRect()).isEmpty(),
		   "nor does an empty bar");
		// A start past the end: clamped, not drawn outside.
		const QRect over = TimelineOverview::viewportRect(hour, hour * 2, hour / 4, bar);
		ok(bar.contains(over), "a start beyond the end is clamped into the bar");
		ok(over.right() == bar.right(), "and pinned to the end, which is where it is");
		const QRect neg = TimelineOverview::viewportRect(hour, -hour, hour / 4, bar);
		ok(neg.x() == bar.x(), "a negative start is clamped to the beginning");
	}

	std::printf("\n-- it only shows while you are moving --\n");
	{
		TimelineOverview ov;
		ov.resize(420, TimelineOverview::kHeight);
		ok(!ov.isVisible(), "it starts hidden");
		ok(ov.opacityForTest() <= 0.001, "and fully transparent");

		ov.showFor(hour, hour / 2, hour / 4, hour / 2);
		ov.finishFadeForTest();
		ok(ov.isVisible(), "a view change shows it");
		ok(ov.opacityForTest() >= 0.99, "at full opacity");

		// The idle timer is what makes it go away on its own; rather than
		// sleeping out its 1.5 s, check the explicit dismissal, which is the
		// same path the fade ends on.
		ov.hideNow();
		ok(!ov.isVisible(), "and it can be dismissed outright");
		ok(ov.opacityForTest() <= 0.001, "back to transparent");

		// A project with no duration has nothing to say, so it must not appear.
		ov.showFor(0, 0, 0, -1);
		ov.finishFadeForTest();
		ok(!ov.isVisible(), "an empty project never shows the strip");
	}

	std::printf("\n-- dragging the box moves the view --\n");
	{
		// The arithmetic first. A grab keeps its GRIP: picking the box up near
		// its right edge and moving must slide it, not snap its centre to the
		// pointer, or every drag would jump before it moved.
		const qint64 vis = hour / 4;
		// Grabbed dead centre, pointer at the middle of the bar -> the view is
		// centred on the middle of the project.
		const qint64 mid = TimelineOverview::startForDrag(hour, vis, 0.5, bar.center().x(), bar);
		std::printf("     centre grab at the middle -> start %lld ms (want %lld)\n",
			    (long long)mid, (long long)(hour / 2 - vis / 2));
		ok(std::llabs(mid - (hour / 2 - vis / 2)) < 60000, "a centred grab centres the view");

		// Same pointer, grabbed at the box's LEFT edge: the view now starts at
		// the pointer instead of straddling it.
		const qint64 fromLeft = TimelineOverview::startForDrag(hour, vis, 0.0, bar.center().x(), bar);
		ok(std::llabs(fromLeft - hour / 2) < 60000, "grabbing the left edge puts that edge under the pointer");
		ok(fromLeft > mid, "so the two grabs are genuinely different, not the same answer twice");

		// Dragging past either end is clamped, not scrolled into nothing.
		ok(TimelineOverview::startForDrag(hour, vis, 0.5, bar.x() - 500, bar) == 0,
		   "dragging off the left end stops at the beginning");
		ok(TimelineOverview::startForDrag(hour, vis, 0.5, bar.right() + 500, bar) == hour - vis,
		   "and off the right end stops at the last full view");

		// Moving the pointer right always moves the view later. This is the
		// property that makes a drag feel attached to the hand.
		bool tracks = true;
		qint64 prev = -1;
		for (int x = bar.x(); x <= bar.right(); x += 10) {
			const qint64 st = TimelineOverview::startForDrag(hour, vis, 0.5, x, bar);
			if (st < prev)
				tracks = false;
			prev = st;
		}
		ok(tracks, "the view only moves later as the pointer moves right");
	}

	std::printf("\n-- and the real widget reports it --\n");
	{
		TimelineOverview ov;
		ov.resize(420, TimelineOverview::kHeight);
		ov.showFor(hour, 0, hour / 4, -1); // looking at the first quarter
		ov.finishFadeForTest();

		QSignalSpy spy(&ov, &TimelineOverview::viewStartRequested);
		const QRect bar2 = ov.barRect();
		const int y = bar2.center().y();

		// Click far to the right of the box: a jump, centred there.
		const QPointF at(bar2.x() + bar2.width() * 0.8, y);
		QMouseEvent press(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton,
				  Qt::NoModifier);
		QApplication::sendEvent(&ov, &press);
		ok(spy.count() == 1, "a click in the bar asks for a new view start");
		ok(ov.draggingForTest(), "and begins a drag, so the mouse can carry on");
		const qint64 jumped = spy.count() ? spy.at(0).at(0).toLongLong() : -1;
		std::printf("     clicked at 80%% of an hour -> start %lld ms\n", (long long)jumped);
		ok(jumped > hour / 2, "the view jumped towards the end, where the click was");

		// Drag left; the requested start must come back down.
		const QPointF to(bar2.x() + bar2.width() * 0.3, y);
		QMouseEvent move(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(&ov, &move);
		ok(spy.count() == 2, "moving while held asks again");
		ok(spy.at(1).at(0).toLongLong() < jumped, "and dragging left moves the view earlier");

		QMouseEvent rel(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton,
				Qt::NoModifier);
		QApplication::sendEvent(&ov, &rel);
		ok(!ov.draggingForTest(), "releasing ends the drag");

		// CONTROL: with no button held, a bare move must NOT move the view --
		// otherwise merely crossing the strip would throw the timeline about.
		const QPointF elsewhere(bar2.x() + 5.0, y);
		QMouseEvent hover(QEvent::MouseMove, elsewhere, elsewhere, Qt::NoButton, Qt::NoButton,
				  Qt::NoModifier);
		QApplication::sendEvent(&ov, &hover);
		ok(spy.count() == 2, "CONTROL: hovering across it changes nothing");
	}

	std::printf("\n-- and the box is painted where it was computed --\n");
	{
		// Computing the rectangle correctly and drawing it elsewhere is still
		// wrong, so this reads the red back off the pixels.
		TimelineOverview ov;
		ov.resize(420, TimelineOverview::kHeight);
		ov.showFor(hour, hour * 3 / 4, hour / 4, -1);
		ov.finishFadeForTest();

		QImage shot(ov.size(), QImage::Format_RGBA8888);
		shot.fill(Qt::black);
		ov.render(&shot);

		const QRect want = TimelineOverview::viewportRect(hour, hour * 3 / 4, hour / 4, ov.barRect());
		const int y = ov.barRect().center().y();
		auto reddish = [&](int x) {
			const QColor c = shot.pixelColor(x, y);
			return c.red() > c.green() + 25 && c.red() > c.blue() + 25;
		};
		// Find the red run along the middle of the bar.
		int left = -1, right = -1;
		for (int x = 0; x < shot.width(); ++x)
			if (reddish(x)) {
				if (left < 0)
					left = x;
				right = x;
			}
		std::printf("     computed x=%d..%d, painted red x=%d..%d\n", want.x(), want.right(),
			    left, right);
		ok(left >= 0, "the red box is actually painted");
		ok(std::abs(left - want.x()) <= 2 && std::abs(right - want.right()) <= 2,
		   "and it is painted where the mapping says");
		// CONTROL: the left third of the bar is NOT in the viewport here, so if
		// the check above passed by finding red everywhere, this catches it.
		ok(!reddish(ov.barRect().x() + 5),
		   "CONTROL: the part of the project you are NOT looking at is not red");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
