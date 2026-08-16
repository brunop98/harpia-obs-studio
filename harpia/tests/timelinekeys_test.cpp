// Keyframes on the timeline itself, and a magnet that knows about them.
//
// Two changes are pinned here, and they are the same underlying claim: the
// moments a project is BUILT around -- the markers you dropped and the keys you
// set -- have to be reachable from the timeline, not just visible on it.
//
//   - The magnet. A marker used to be decoration: you could see the flag and
//     drag a clip straight through it, because snap() only ever considered 0,
//     the playhead and clip edges. Same for every keyframe.
//   - The pips. The diamonds along a clip's top edge were paint-only, and they
//     were drawn for the clip's POSE keys alone -- so a clip animated through
//     an effect or a component read as not animated at all. Now every kind is
//     drawn, and a pip can be clicked, dragged and deleted.
//
// The controls matter as much as the checks. "The clip did not move" is worth
// nothing unless the identical gesture in the clip's body DOES move it, and
// "the key snapped to the marker" is worth nothing unless it lands somewhere
// else with the magnet off.
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
static void eq(qint64 got, qint64 want, const char *w)
{
	const bool good = got == want;
	std::printf("  %s %s (got %lld, want %lld)\n", good ? "PASS" : "FAIL", w,
		    (long long)got, (long long)want);
	if (!good)
		++failures;
}

static void press(QWidget *w, QPoint p, Qt::MouseButton b,
		  Qt::KeyboardModifiers m = Qt::NoModifier)
{
	QMouseEvent e(QEvent::MouseButtonPress, QPointF(p), w->mapToGlobal(p), b, b, m);
	QApplication::sendEvent(w, &e);
}
static void moveTo(QWidget *w, QPoint p, Qt::MouseButtons held,
		   Qt::KeyboardModifiers m = Qt::NoModifier)
{
	QMouseEvent e(QEvent::MouseMove, QPointF(p), w->mapToGlobal(p), Qt::NoButton, held, m);
	QApplication::sendEvent(w, &e);
}
static void release(QWidget *w, QPoint p, Qt::MouseButton b)
{
	QMouseEvent e(QEvent::MouseButtonRelease, QPointF(p), w->mapToGlobal(p), b, Qt::NoButton,
		      Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}

// One video track, one 20 s clip at the origin, with a pose key at 4 s.
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
	c.srcEndMs = 20000;
	c.outStartMs = 0;
	TlKeyframe k;
	k.tMs = 4000;
	c.keys.append(k);
	t.clips.append(c);
	m.tracks.append(t);
	return m;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- a clip's keys, whatever kind they are --\n");
	{
		TlClip c;
		c.outStartMs = 5000;
		c.srcStartMs = 0;
		c.srcEndMs = 10000;

		ok(!clipHasKeys(c), "a clip with nothing keyed says so");

		TlKeyframe pose;
		pose.tMs = 1000;
		c.keys.append(pose);

		FxKey fk;
		fk.tMs = 2000;
		c.fx.keys.append(fk);

		SpotMask mask;
		SpotKey sk;
		sk.tMs = c.outStartMs + 3000; // spotlight keys are stored in OUTPUT time
		mask.keys.append(sk);
		c.fx.spot.masks.append(mask);

		ComponentInstance ci;
		ci.typeId = QStringLiteral("harpia.blur");
		ci.keys.insert(QStringLiteral("radius"), {PropKey{1000, 0.5, TlEase::Linear, 0, 0}});
		c.components.append(ci);

		// A key past the end of the clip: trimming a clip shorter leaves keys
		// beyond it in the data, and a pip drawn off the end of the clip is a
		// pip you cannot reach.
		FxKey stray;
		stray.tMs = 99000;
		c.fx.keys.append(stray);

		const QVector<qint64> times = clipKeyTimes(c);
		ok(clipHasKeys(c), "and one with any kind of key says that too");
		eq(times.size(), 3, "four stores, one moment shared, one out of range");
		ok(times == QVector<qint64>({1000, 2000, 3000}),
		   "sorted, de-duplicated, and clip-relative — the spotlight key converted");

		std::printf("\n-- moving a whole column of keys --\n");
		ok(retimeClipKeys(c, 1000, 1500), "a column moves");
		ok(c.keys[0].tMs == 1500 &&
			   c.components[0].keys[QStringLiteral("radius")][0].tMs == 1500,
		   "and takes EVERY channel keyed at that moment with it");
		ok(!retimeClipKeys(c, 1500, 2000),
		   "onto an occupied moment it refuses, rather than silently merging two "
		   "columns into one");
		ok(c.keys[0].tMs == 1500, "so nothing moved");
		ok(retimeClipKeys(c, 3000, 99999) && c.fx.spot.masks[0].keys[0].tMs ==
							    c.outStartMs + c.outDurationMs(),
		   "past the end it clamps to the clip, staying reachable");

		std::printf("\n-- and deleting one --\n");
		ok(removeClipKeysAt(c, 1500), "the column goes");
		ok(c.keys.isEmpty() && !c.components[0].keys.contains(QStringLiteral("radius")),
		   "every channel with it, and an emptied property is un-keyed rather than "
		   "left as an empty list that still reads as animated");
		ok(!removeClipKeysAt(c, 1500), "and deleting nothing reports nothing");
	}

	TimelineView v;
	v.resize(700, 220);
	v.show();
	QApplication::processEvents();
	v.setModel(oneClip());
	v.setSnapEnabled(true);
	QApplication::processEvents();

	std::printf("\n-- the magnet knows about markers --\n");
	{
		TimelineModel m = v.model();
		m.markers.append(9000);
		v.setModel(m);
		v.clearPlayhead();

		// Just inside the tolerance, in ms, at this zoom.
		const qint64 tolMs = v.msForX(v.contentRectForTest().x() + 8) -
				     v.msForX(v.contentRectForTest().x());
		eq(v.snapForTest(9000 + tolMs / 2), 9000, "a drag near a marker lands on it");
		ok(v.snapForTest(9000 + tolMs * 4) != 9000, "and one nowhere near it does not");

		v.setSnapEnabled(false);
		ok(v.snapForTest(9000 + tolMs / 2) != 9000, "with the magnet off, nothing pulls");
		v.setSnapEnabled(true);
	}

	std::printf("\n-- and about keyframes --\n");
	{
		const qint64 tolMs = v.msForX(v.contentRectForTest().x() + 8) -
				     v.msForX(v.contentRectForTest().x());
		// The clip starts at 0, so its key at 4 s is at output 4 s.
		eq(v.snapForTest(4000 + tolMs / 2), 4000,
		   "a cut can be lined up with the moment a move lands");
	}

	std::printf("\n-- a pip is a thing you can hit --\n");
	{
		const QPoint pip = v.keyPipCenterForTest(0, 0, 4000);
		int ht = -1, hc = -1;
		qint64 hms = -1;
		ok(v.keyPipHitForTest(pip, &ht, &hc, &hms), "the pip is under its own centre");
		ok(ht == 0 && hc == 0, "on the right clip");
		eq(hms, 4000, "and it is the key we drew");

		ok(!v.keyPipHitForTest(pip + QPoint(40, 0), nullptr, nullptr, nullptr),
		   "a point well away from it is not");
		// Deeper into the clip is the CLIP, not the pip: everything below the
		// top band still moves and trims exactly as it did.
		ok(!v.keyPipHitForTest(pip + QPoint(0, 26), nullptr, nullptr, nullptr),
		   "and neither is the clip's body");
	}

	std::printf("\n-- dragging a pip retimes the key, and moves nothing else --\n");
	{
		v.setModel(oneClip());
		QApplication::processEvents();
		const qint64 startWas = v.model().tracks[0].clips[0].outStartMs;
		const QPoint from = v.keyPipCenterForTest(0, 0, 4000);
		const QPoint to = from + QPoint(60, 0);

		press(&v, from, Qt::LeftButton);
		moveTo(&v, to, Qt::LeftButton, Qt::ShiftModifier); // Shift = no magnet
		release(&v, to, Qt::LeftButton);

		const TlClip &c = v.model().tracks[0].clips[0];
		const qint64 moved = c.keys[0].tMs;
		std::printf("     key 4000 -> %lld\n", (long long)moved);
		ok(moved > 4000, "the key moved with the pointer");
		eq(c.outStartMs, startWas, "and the CLIP stayed exactly where it was");

		// The control. Without it, "the clip did not move" is also what a
		// timeline that ignores every drag would report.
		const QRect r = v.clipRectForTest(0, 0);
		const QPoint body(r.center().x(), r.center().y());
		press(&v, body, Qt::LeftButton);
		moveTo(&v, body + QPoint(60, 0), Qt::LeftButton, Qt::ShiftModifier);
		release(&v, body + QPoint(60, 0), Qt::LeftButton);
		ok(v.model().tracks[0].clips[0].outStartMs != startWas,
		   "while the same gesture in the clip's body DOES move the clip");
	}

	std::printf("\n-- a dragged key snaps to a marker --\n");
	{
		TimelineModel m = oneClip();
		// A marker a little way past the key, well inside the pull.
		m.markers.append(4000);
		m.tracks[0].clips[0].keys[0].tMs = 2000;
		v.setModel(m);
		QApplication::processEvents();

		const QPoint from = v.keyPipCenterForTest(0, 0, 2000);
		const QPoint mark(v.xForMs(4000) - 3, from.y());
		press(&v, from, Qt::LeftButton);
		moveTo(&v, mark, Qt::LeftButton);
		release(&v, mark, Qt::LeftButton);
		eq(v.model().tracks[0].clips[0].keys[0].tMs, 4000,
		   "it lands ON the marker, not three pixels short of it");
	}

	std::printf("\n-- and a click that does not drag goes there instead --\n");
	{
		v.setModel(oneClip());
		v.clearPlayhead();
		QApplication::processEvents();
		const QPoint pip = v.keyPipCenterForTest(0, 0, 4000);
		press(&v, pip, Qt::LeftButton);
		release(&v, pip, Qt::LeftButton);
		eq(v.playhead(), 4000, "the playhead is on the key");
		eq(v.model().tracks[0].clips[0].keys[0].tMs, 4000, "and the key did not move");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
