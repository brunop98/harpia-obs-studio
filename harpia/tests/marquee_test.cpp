// Shift+drag on the timeline draws a box that selects every clip it touches.
//
// Through the real TimelineView with real mouse events: the box selects what
// it crosses on unlocked tracks and nothing else, it replaces the selection
// (Ctrl+Shift adds), a Shift+press on a clip draws a box instead of moving
// the clip, and a Shift+click on empty space clears the selection.
#include "editor/timeline/TimelineModel.hpp"
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

static void mouse(QWidget *w, QEvent::Type t, QPoint p, Qt::KeyboardModifiers mods = Qt::NoModifier,
		  Qt::MouseButton b = Qt::LeftButton)
{
	const Qt::MouseButtons held = t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(b);
	QMouseEvent e(t, QPointF(p), w->mapToGlobal(QPointF(p)), t == QEvent::MouseMove ? Qt::NoButton : b, held,
		      mods);
	QApplication::sendEvent(w, &e);
}

static void drag(QWidget *w, QPoint from, QPoint to, Qt::KeyboardModifiers mods)
{
	mouse(w, QEvent::MouseButtonPress, from, mods);
	mouse(w, QEvent::MouseMove, (from + to) / 2, mods);
	mouse(w, QEvent::MouseMove, to, mods);
	mouse(w, QEvent::MouseButtonRelease, to, mods);
}

static TlClip clip(int id, qint64 outStart, qint64 len)
{
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = id;
	c.srcStartMs = 0;
	c.srcEndMs = len;
	c.outStartMs = outStart;
	return c;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	// Three tracks: two with three clips each, the third locked.
	TimelineModel m;
	for (int t = 0; t < 3; ++t) {
		TlTrack tr;
		tr.kind = TlTrack::Kind::Video;
		tr.clips = {clip(1, 0, 2000), clip(1, 2500, 2000), clip(1, 5000, 2000)};
		tr.locked = t == 2;
		m.tracks.append(tr);
	}
	TimelineView v;
	v.resize(900, 400);
	v.show();
	v.setModel(m);

	const QRect a0 = v.clipRectForTest(0, 0), a1 = v.clipRectForTest(0, 1), a2 = v.clipRectForTest(0, 2);
	const QRect b0 = v.clipRectForTest(1, 0), b1 = v.clipRectForTest(1, 1);
	const QRect c0 = v.clipRectForTest(2, 0);
	std::printf("     clip (0,0) %d,%d %dx%d   (1,1) %d,%d %dx%d   locked (2,0) y %d\n", a0.x(), a0.y(),
		    a0.width(), a0.height(), b1.x(), b1.y(), b1.width(), b1.height(), c0.y());
	ok(!a0.isEmpty() && !b1.isEmpty(), "the clips have rectangles");

	std::printf("\n-- a box selects what it touches --\n");
	{
		// From above-left of clip (0,0) to inside clip (1,1): touches
		// (0,0), (0,1), (1,0), (1,1) and not (0,2).
		const QPoint from(a0.left() + 4, a0.top() + 2), to(b1.center());
		mouse(&v, QEvent::MouseButtonPress, from, Qt::ShiftModifier);
		ok(v.marqueeActiveForTest(), "Shift+press starts a box");
		mouse(&v, QEvent::MouseMove, to, Qt::ShiftModifier);
		ok(v.marqueeRectForTest() == QRect(from, to).normalized(), "the box follows the pointer");
		ok(v.isSelected(0, 0) && v.isSelected(0, 1) && v.isSelected(1, 0) && v.isSelected(1, 1),
		   "everything the box crosses is selected, live");
		ok(!v.isSelected(0, 2), "and nothing outside it");
		ok(v.selectedTrack() == 0 && v.selectedClip() == 0, "the first touched clip is the primary");
		mouse(&v, QEvent::MouseButtonRelease, to, Qt::ShiftModifier);
		ok(!v.marqueeActiveForTest() && v.marqueeRectForTest().isNull(), "release ends the box");
		ok(v.selectedCountForTest() == 4, "and keeps the four");
	}

	std::printf("\n-- a thin box across a lane takes everything it crosses --\n");
	{
		drag(&v, QPoint(a0.left() + 2, a0.center().y()), QPoint(a2.right() - 2, a0.center().y() + 1),
		     Qt::ShiftModifier);
		ok(v.isSelected(0, 0) && v.isSelected(0, 1) && v.isSelected(0, 2), "all three on the track");
		ok(!v.isSelected(1, 0) && v.selectedCountForTest() == 3, "only that track");
	}

	std::printf("\n-- it replaces; Ctrl+Shift adds --\n");
	{
		drag(&v, QPoint(b0.left() + 2, b0.top() + 2), b0.center(), Qt::ShiftModifier);
		ok(v.selectedCountForTest() == 1 && v.isSelected(1, 0), "a new box replaces the selection");
		drag(&v, QPoint(a2.left() + 2, a2.top() + 2), a2.center(), Qt::ShiftModifier | Qt::ControlModifier);
		ok(v.isSelected(1, 0) && v.isSelected(0, 2) && v.selectedCountForTest() == 2,
		   "Ctrl+Shift keeps what was selected and adds");
		ok(v.selectedTrack() == 0 && v.selectedClip() == 2, "the newly boxed clip becomes the primary");
	}

	std::printf("\n-- locked tracks are skipped --\n");
	{
		drag(&v, QPoint(c0.left() + 2, c0.top() + 2), c0.center(), Qt::ShiftModifier);
		ok(!v.isSelected(2, 0) && v.selectedCountForTest() == 0, "a box over a locked track selects nothing");
		const QPoint from(a0.left() + 2, a0.top() + 2), to(c0.center());
		drag(&v, from, to, Qt::ShiftModifier);
		ok(v.isSelected(0, 0) && v.isSelected(1, 0) && !v.isSelected(2, 0), "one spanning all three skips the locked one");
	}

	std::printf("\n-- Shift+press on a clip draws a box, it does not move the clip --\n");
	{
		const qint64 before = v.model().tracks[0].clips[1].outStartMs;
		drag(&v, a1.center(), a1.center() + QPoint(120, 0), Qt::ShiftModifier);
		ok(v.model().tracks[0].clips[1].outStartMs == before, "the clip stayed put");
		ok(v.isSelected(0, 1), "and is selected");
	}

	std::printf("\n-- dragging any member moves the whole group --\n");
	{
		// Box the three clips of track 0 (and none of track 1), then drag by
		// the SECOND one, which is not the primary.
		drag(&v, QPoint(a0.left() + 2, a0.top() + 2), QPoint(a2.right() - 2, a0.top() + 4), Qt::ShiftModifier);
		ok(v.selectedCountForTest() == 3 && v.selectedTrack() == 0 && v.selectedClip() == 0,
		   "three selected, the first is the primary");
		const qint64 s0 = v.model().tracks[0].clips[0].outStartMs, s1 = v.model().tracks[0].clips[1].outStartMs,
			     s2 = v.model().tracks[0].clips[2].outStartMs, o0 = v.model().tracks[1].clips[0].outStartMs;
		const QPoint grab = v.clipRectForTest(0, 1).center();
		drag(&v, grab, grab + QPoint(60, 0), Qt::NoModifier);
		const qint64 d1 = v.model().tracks[0].clips[1].outStartMs - s1;
		std::printf("     moved by %lld ms\n", (long long)d1);
		ok(d1 > 0, "the grabbed clip moved");
		ok(v.model().tracks[0].clips[0].outStartMs - s0 == d1, "the old primary moved by the same amount");
		ok(v.model().tracks[0].clips[2].outStartMs - s2 == d1, "and so did the third");
		ok(v.model().tracks[1].clips[0].outStartMs == o0, "an unselected clip stayed");
		ok(v.selectedCountForTest() == 3, "the group is still selected afterwards");
	}

	std::printf("\n-- Shift+click on empty space clears --\n");
	{
		const QPoint gap((a0.right() + a1.left()) / 2, a0.center().y());
		mouse(&v, QEvent::MouseButtonPress, gap, Qt::ShiftModifier);
		mouse(&v, QEvent::MouseButtonRelease, gap, Qt::ShiftModifier);
		ok(v.selectedCountForTest() == 0, "nothing selected");
	}

	std::printf("\n-- force ripple: closing a track's gaps --\n");
	{
		TlTrack g;
		g.kind = TlTrack::Kind::Video;
		// In the vector out of screen order, with a gap before the first clip,
		// two gaps, and one clip overlapping another (a transition).
		g.clips = {clip(1, 9000, 1000), clip(1, 1000, 2000), clip(1, 4000, 1000), clip(1, 4800, 1000)};
		const int moved = closeTrackGaps(g);
		ok(moved == 3, "three clips moved (the first stays)");
		ok(g.clips[1].outStartMs == 1000, "the earliest clip keeps its place (the gap before it is not between clips)");
		ok(g.clips[2].outStartMs == 3000, "the next one butts up against it");
		ok(g.clips[3].outStartMs == 3800, "an overlap with the one before is kept (200 ms)");
		ok(g.clips[0].outStartMs == 4800, "the last one on screen, first in the list, closes up too");
		ok(g.clips[0].outDurationMs() == 1000 && g.clips[2].srcStartMs == 0, "lengths and sources untouched");
		ok(closeTrackGaps(g) == 0, "a second pass has nothing to do");
		TlTrack one;
		one.clips = {clip(1, 5000, 1000)};
		ok(closeTrackGaps(one) == 0 && one.clips[0].outStartMs == 5000, "a lone clip is left where it is");

		TimelineModel gm;
		TlTrack locked = g;
		locked.clips = {clip(1, 0, 1000), clip(1, 3000, 1000)};
		locked.locked = true;
		gm.tracks = {locked};
		TimelineView gv;
		gv.resize(900, 300);
		gv.setModel(gm);
		ok(gv.forceRipple(0) == 0 && gv.model().tracks[0].clips[1].outStartMs == 3000,
		   "a locked track is not rippled");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
