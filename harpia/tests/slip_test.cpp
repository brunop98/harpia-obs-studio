// Slip: a clip keeps its place and its length, and dragging it moves the
// footage inside -- a different moment of the same video.
//
//   * Only the source window moves: the clip's position, its length and every
//     other clip stay exactly where they were.
//   * The footage follows the pointer (drag left: later footage), and stops at
//     either end of the source rather than running past it.
//   * Its detached sound (linked, same file) slips with it; a linked caption
//     does not.
//   * The preview follows the drag; one drag is one undo step.
//   * Slip is offered where it means something: a media clip with footage to
//     spare, on an unlocked lane -- and says why where it does not.
//   * Esc, Enter or a click elsewhere ends it; so does a replaced model.
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QKeyEvent>
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
	QMouseEvent e(QEvent::MouseButtonPress, QPointF(p), w->mapToGlobal(p), Qt::LeftButton, Qt::LeftButton,
		      Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}
static void moveTo(QWidget *w, QPoint p)
{
	QMouseEvent e(QEvent::MouseMove, QPointF(p), w->mapToGlobal(p), Qt::NoButton, Qt::LeftButton,
		      Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}
static void release(QWidget *w, QPoint p)
{
	QMouseEvent e(QEvent::MouseButtonRelease, QPointF(p), w->mapToGlobal(p), Qt::LeftButton, Qt::NoButton,
		      Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}
static void drag(QWidget *w, QPoint from, int dx)
{
	press(w, from);
	moveTo(w, from + QPoint(dx / 2, 0));
	moveTo(w, from + QPoint(dx, 0));
	release(w, from + QPoint(dx, 0));
}
static void key(QWidget *w, int k)
{
	QKeyEvent e(QEvent::KeyPress, k, Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}

static TlClip media(int source, qint64 out, qint64 from, qint64 to, int link = 0)
{
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = source;
	c.srcStartMs = from;
	c.srcEndMs = to;
	c.outStartMs = out;
	c.link = link;
	return c;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- how far a slip can go --\n");
	{
		// A 30 s source shown 5-10 s, as in the request's example.
		ok(tlSlipFit(5000, 10000, 30000, 3000) == 3000, "inside the footage: all of it");
		ok(tlSlipFit(5000, 10000, 30000, -8000) == -5000, "back to the first frame, no further");
		ok(tlSlipFit(5000, 10000, 30000, 25000) == 20000, "on to the last frame, no further");
		ok(tlSlipFit(5000, 10000, 0, 4000) == 0 && tlSlipFit(5000, 10000, 0, -4000) == -4000,
		   "unknown length: only the footage before the window is known to exist");
	}

	// V1: the clip to slip (source 7, 5-10 s of 30 s) and a neighbour.
	// A1: its detached sound, linked. T: a caption, linked too.
	TimelineModel m;
	TlTrack v1;
	v1.kind = TlTrack::Kind::Video;
	v1.name = QStringLiteral("V1");
	v1.clips = {media(7, 2000, 5000, 10000, 5), media(8, 7000, 0, 3000)};
	TlTrack a1;
	a1.kind = TlTrack::Kind::Audio;
	a1.name = QStringLiteral("A1");
	a1.clips = {media(7, 2000, 5000, 10000, 5)};
	TlTrack tx;
	tx.kind = TlTrack::Kind::Video;
	tx.name = QStringLiteral("T");
	TlClip cap;
	cap.type = TlClip::Type::Text;
	cap.text.text = QStringLiteral("hello");
	cap.srcEndMs = 5000;
	cap.outStartMs = 2000;
	cap.link = 5;
	tx.clips = {cap};
	m.tracks = {tx, v1, a1};
	const int T = 0, V = 1, A = 2;

	TimelineView v;
	v.resize(1100, 320);
	v.setModel(m);
	v.setSourceThumbs(7, {}, 30000);
	v.setSourceThumbs(8, {}, 3000);
	v.show();
	QApplication::processEvents();
	int commits = 0;
	QObject::connect(&v, &TimelineView::editCommitted, [&commits]() { ++commits; });
	int scrubs = 0;
	QObject::connect(&v, &TimelineView::scrub, [&scrubs](qint64) { ++scrubs; });
	const auto clip = [&v](int t, int c) { return v.model().tracks[t].clips[c]; };

	std::printf("\n-- where it is offered --\n");
	{
		QString why;
		ok(v.canSlip(V, 0, &why), "a video clip with footage either side");
		ok(!v.canSlip(V, 1, &why) && why.contains(QStringLiteral("already shows all")),
		   "a clip showing its whole source: nothing to slip to, and it says so");
		ok(!v.canSlip(T, 0, &why) && why.contains(QStringLiteral("Only video and audio")),
		   "a caption has no footage");
		TimelineModel n = m;
		n.tracks[V].locked = true;
		n.tracks[A].clips[0].tts = QStringLiteral("{}");
		TimelineView w;
		w.setModel(n);
		w.setSourceThumbs(7, {}, 30000);
		ok(!w.canSlip(V, 0, &why) && why.contains(QStringLiteral("locked")), "nor on a locked lane");
		ok(!w.canSlip(A, 0, &why) && why.contains(QStringLiteral("narration")),
		   "nor a narration, whose captions are timed to its words");
	}

	std::printf("\n-- slipping --\n");
	{
		v.beginSlip(V, 0);
		ok(v.slipping() && v.selectedTrack() == V, "Slip is on, with the clip selected");
		const QRect r = v.clipRectForTest(V, 0);
		const double msPerPx = 5000.0 / r.width();
		scrubs = 0;
		drag(&v, r.center(), -100);
		const TlClip c = clip(V, 0);
		std::printf("     source now %lld-%lld ms (%.1f ms per px)\n", (long long)c.srcStartMs,
			    (long long)c.srcEndMs, msPerPx);
		ok(c.srcStartMs > 5000 && std::abs(double(c.srcStartMs - 5000) - 100 * msPerPx) <= msPerPx + 1,
		   "dragged left: later footage, as far as the pointer went");
		ok(c.srcLenMs() == 5000 && c.outStartMs == 2000, "the clip's place and length are unchanged");
		const TlClip n = clip(V, 1);
		ok(n.outStartMs == 7000 && n.srcStartMs == 0 && n.srcEndMs == 3000, "the neighbour is untouched");
		ok(clip(A, 0).srcStartMs == c.srcStartMs && clip(A, 0).srcEndMs == c.srcEndMs &&
			   clip(A, 0).outStartMs == 2000,
		   "its sound slipped with it: still in sync");
		ok(clip(T, 0).outStartMs == 2000 && clip(T, 0).text.text == QStringLiteral("hello"),
		   "the linked caption stayed as it was");
		ok(commits == 1, "one drag, one undo step");
		ok(scrubs > 0, "the preview followed the drag");
		ok(v.slipping(), "and Slip stays on for the next drag");

		drag(&v, v.clipRectForTest(V, 0).center(), 900);
		ok(clip(V, 0).srcStartMs == 0 && clip(V, 0).srcLenMs() == 5000,
		   "dragged far right: stops at the first frame");
		drag(&v, v.clipRectForTest(V, 0).center(), -1000);
		drag(&v, v.clipRectForTest(V, 0).center(), -1000);
		drag(&v, v.clipRectForTest(V, 0).center(), -1000);
		ok(clip(V, 0).srcEndMs == 30000 && clip(V, 0).srcLenMs() == 5000,
		   "dragged far left: stops at the last frame");
		ok(clip(V, 0).outStartMs == 2000 && clip(V, 1).outStartMs == 7000, "and nothing moved along the timeline");
	}

	std::printf("\n-- ending it --\n");
	{
		key(&v, Qt::Key_Escape);
		ok(!v.slipping(), "Esc ends Slip");
		const qint64 before = clip(V, 0).srcStartMs;
		const QRect r = v.clipRectForTest(V, 0);
		drag(&v, r.center(), -60);
		ok(clip(V, 0).srcStartMs == before, "after which a drag moves the clip again, not its footage");

		v.setModel(m);
		v.setSourceThumbs(7, {}, 30000);
		v.beginSlip(V, 0);
		key(&v, Qt::Key_Return);
		ok(!v.slipping(), "Enter ends it too");
		v.beginSlip(V, 0);
		const QRect other = v.clipRectForTest(V, 1);
		press(&v, other.center());
		release(&v, other.center());
		ok(!v.slipping() && clip(V, 0).srcStartMs == 5000, "a click elsewhere ends it, footage untouched");
		v.beginSlip(V, 0);
		v.setModel(m);
		ok(!v.slipping(), "a replaced model (undo, load) ends it");
	}

	std::printf("\n-- a Random clip on the timeline --\n");
	{
		// A 4 s slot of source 7 (30 s, from 5 s); its other clip is source 9
		// (8 s long), slipped to 2 s.
		TimelineModel rm;
		TlTrack rv;
		rv.kind = TlTrack::Kind::Video;
		rv.name = QStringLiteral("V1");
		TlClip slotClip = media(7, 2000, 5000, 9000);
		slotClip.clipOptions = {{7, 5000}, {9, 2000}};
		rv.clips = {slotClip};
		rm.tracks = {rv};
		TimelineView w;
		w.resize(1100, 320);
		w.setModel(rm);
		w.setSourceThumbs(7, {}, 30000);
		w.setSourceThumbs(9, {}, 8000);
		w.show();
		QApplication::processEvents();
		const auto rc = [&w]() { return w.model().tracks[0].clips[0]; };

		// Grow its end as far as it goes: clip 2 has 8 - 2 - 4 = 2 s to spare.
		QRect r = w.clipRectForTest(0, 0);
		drag(&w, QPoint(r.right() - 2, r.center().y()), 600);
		ok(rc().srcEndMs == 11000 && rc().srcLenMs() == 6000,
		   "its end grows only as far as its shortest clip allows (2 s more)");
		// Its head: clip 2 starts 2 s in, so 2 s back at most.
		r = w.clipRectForTest(0, 0);
		drag(&w, QPoint(r.left() + 2, r.center().y()), -800);
		ok(rc().srcStartMs == 3000 && rc().clipOptions[1].srcStartMs == 0,
		   "its head grows back 2 s, and clip 2's head with it");
		r = w.clipRectForTest(0, 0);
		drag(&w, QPoint(r.left() + 2, r.center().y()), 60);
		const qint64 cut = rc().srcStartMs - 3000;
		ok(cut > 0 && rc().clipOptions[1].srcStartMs == cut, "trimming the head trims clip 2 the same way");

		// Split in the middle: the second half of every clip.
		TimelineModel sm = rm;
		w.setModel(sm);
		w.setSourceThumbs(7, {}, 30000);
		w.setSourceThumbs(9, {}, 8000);
		w.setPlayhead(4000);
		w.splitAtPlayhead();
		const TlTrack &st = w.model().tracks[0];
		ok(st.clips.size() == 2, "split in two");
		if (st.clips.size() == 2) {
			const TlClip &b = st.clips[0].outStartMs > st.clips[1].outStartMs ? st.clips[0] : st.clips[1];
			ok(b.srcStartMs == 7000 && b.clipOptions[1].srcStartMs == 4000,
			   "the second half starts 2 s into each clip, its own and clip 2");
		}
	}

	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
