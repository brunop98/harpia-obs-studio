// The timeline must not rescale itself while you are dragging a clip.
//
// Every pixel position on this widget comes from the project's TOTAL span --
// its duration plus a tail -- and zoom 1 means "the whole span across the
// view". So dragging a clip past the current end grows the span, and the axis
// re-derives: the ruler stretches, every other clip shrinks, and the clip in
// your hand stops keeping up with the pointer, because the millisecond under
// the cursor moved while you were holding it. It reads as the timeline zooming
// itself while you work.
//
// What is pinned here is that the mapping is FROZEN for the length of such a
// drag, and -- the control, without which this proves nothing -- that it is
// released afterwards, because the project really is longer than it was.
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QMouseEvent>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static void press(QWidget *w, QPoint p)
{
	QMouseEvent e(QEvent::MouseButtonPress, QPointF(p), w->mapToGlobal(p), Qt::LeftButton,
		      Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}
static void moveTo(QWidget *w, QPoint p)
{
	QMouseEvent e(QEvent::MouseMove, QPointF(p), w->mapToGlobal(p), Qt::NoButton, Qt::LeftButton,
		      Qt::ShiftModifier); // Shift: no magnet, so the maths is the maths
	QApplication::sendEvent(w, &e);
}
static void release(QWidget *w, QPoint p)
{
	QMouseEvent e(QEvent::MouseButtonRelease, QPointF(p), w->mapToGlobal(p), Qt::LeftButton,
		      Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}

// One 10 s clip at the origin, so dragging it right makes the project longer
// and there is nothing else to keep the span up.
static TimelineModel oneClip()
{
	TimelineModel m;
	TlTrack t;
	t.kind = TlTrack::Kind::Video;
	t.name = QStringLiteral("V1");
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = 7;
	c.srcStartMs = 0;
	c.srcEndMs = 10000;
	c.outStartMs = 0;
	t.clips.append(c);
	m.tracks.append(t);
	return m;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	TimelineView v;
	v.resize(800, 220);
	v.show();
	QApplication::processEvents();
	v.setModel(oneClip());
	QApplication::processEvents();

	// The axis, sampled as "where does 5 s sit on screen".
	const auto xOf5s = [&v]() { return v.xForMs(5000); };

	std::printf("\n-- the axis holds still for the whole drag --\n");
	{
		const int before = xOf5s();
		const QRect r = v.clipRectForTest(0, 0);
		const QPoint from(r.center().x(), r.center().y());

		press(&v, from);
		// Out past the end of the project, in steps, checking as we go: this is
		// exactly the gesture that used to shrink everything under the cursor.
		bool held = true;
		int lastX = before;
		for (int dx = 40; dx <= 240; dx += 40) {
			moveTo(&v, from + QPoint(dx, 0));
			lastX = xOf5s();
			if (lastX != before)
				held = false;
		}
		std::printf("     5 s sat at x=%d, and at x=%d %s\n", before, lastX,
			    held ? "throughout" : "by the end of the drag");
		ok(held, "5 s stays on the same pixel while the clip is dragged past the end");
		// And the drag really did extend the project -- otherwise the check
		// above passes for the boring reason that nothing happened.
		ok(v.model().tracks[0].clips[0].outStartMs > 0, "the clip really moved");
		ok(v.durationMs() > 10000, "and the project really got longer");

		release(&v, from + QPoint(240, 0));
		QApplication::processEvents();

		// THE CONTROL. The axis is not frozen forever: the project is longer
		// now, and at zoom 1 the whole of it is meant to fit across the view.
		std::printf("     after the drop, 5 s sits at x=%d\n", xOf5s());
		ok(xOf5s() != before, "and the one rescale happens on the drop");
	}

	std::printf("\n-- a trim holds it too --\n");
	{
		v.setModel(oneClip());
		QApplication::processEvents();
		const int before = xOf5s();
		const QRect r = v.clipRectForTest(0, 0);
		// The right edge: dragging it outwards lengthens the clip, which is the
		// other way to grow the project mid-gesture.
		const QPoint edge(r.right() - 2, r.center().y());
		press(&v, edge);
		moveTo(&v, edge + QPoint(120, 0));
		const int during = xOf5s();
		release(&v, edge + QPoint(120, 0));
		QApplication::processEvents();
		ok(during == before, "the axis is held while an edge is dragged");
	}

	std::printf("\n-- and a release the widget did not expect leaves it free --\n");
	{
		// A press that starts a drag, then a release of a DIFFERENT button. The
		// early return there used to skip past everything; a frozen axis that
		// is never released is a timeline that stops tracking its own contents
		// for the rest of the session.
		v.setModel(oneClip());
		QApplication::processEvents();
		const QRect r = v.clipRectForTest(0, 0);
		const QPoint from(r.center().x(), r.center().y());
		press(&v, from);
		moveTo(&v, from + QPoint(200, 0));
		QMouseEvent mid(QEvent::MouseButtonRelease, QPointF(from), v.mapToGlobal(from),
				Qt::RightButton, Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(&v, &mid);
		release(&v, from + QPoint(200, 0));
		QApplication::processEvents();

		// Whatever happened to the clip, the axis must now describe the model.
		const qint64 span = v.durationMs();
		v.setModel(oneClip()); // back to a 10 s project
		QApplication::processEvents();
		ok(v.xForMs(5000) != 0 && span >= 10000,
		   "the axis follows the model again once the gesture is over");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
