// The track list: four things that were quietly wrong about it.
//
//   - a name you typed did not last. Renumbering ran after every add or remove
//     and rewrote EVERY name, so "Narration" was "A2" again the moment any
//     other lane changed.
//   - an effect lane could not be hidden, although the compositor honours it:
//     the H switch and the menu item were gated on Video. And an effect lane
//     offered Mute, which does nothing there.
//   - dragging the last clip off a lane deleted the lane -- including one you
//     had just made and named as somewhere to park things.
//   - a locked lane still took a clip from a menu action; only a drop checked.
//
// Each is pinned with its control: the thing that SHOULD still happen next to
// the thing that should not.
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
static void eqs(const QString &got, const char *want, const char *w)
{
	const bool good = got == QString::fromLatin1(want);
	std::printf("  %s %s (got \"%s\")\n", good ? "PASS" : "FAIL", w, qPrintable(got));
	if (!good)
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
		      Qt::ShiftModifier); // no magnet, so the clip lands where it is put
	QApplication::sendEvent(w, &e);
}
static void release(QWidget *w, QPoint p)
{
	QMouseEvent e(QEvent::MouseButtonRelease, QPointF(p), w->mapToGlobal(p), Qt::LeftButton,
		      Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}
static void click(QWidget *w, QPoint p)
{
	press(w, p);
	release(w, p);
}

static TlClip clipAt(qint64 outMs, int source = 7)
{
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = source;
	c.srcStartMs = 0;
	c.srcEndMs = 4000;
	c.outStartMs = outMs;
	return c;
}
static TlTrack track(TlTrack::Kind k, const char *name)
{
	TlTrack t;
	t.kind = k;
	t.name = QString::fromLatin1(name);
	return t;
}
// A clip on each lane. Nothing that is being dragged should ever land on an
// empty lane by accident, and the count of lanes is what most checks read.
static TimelineModel threeLanes()
{
	TimelineModel m;
	TlTrack v2 = track(TlTrack::Kind::Video, "V2");
	v2.clips.append(clipAt(0));
	TlTrack v1 = track(TlTrack::Kind::Video, "V1");
	v1.clips.append(clipAt(10000));
	TlTrack a1 = track(TlTrack::Kind::Audio, "A1");
	a1.clips.append(clipAt(0));
	m.tracks = {v2, v1, a1};
	return m;
}

// Drag the only clip on `from` onto the middle of lane `to`.
static void dragClipToLane(TimelineView &v, int from, int to)
{
	const QRect r = v.clipRectForTest(from, 0);
	const QPoint start(r.center());
	const QPoint end(r.center().x() + 30, v.clipRectForTest(to, 0).center().y());
	press(&v, start);
	moveTo(&v, start + QPoint(8, 0));
	moveTo(&v, end);
	release(&v, end);
	QApplication::processEvents();
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	TimelineView v;
	v.resize(900, 400);
	v.show();
	QApplication::processEvents();

	std::printf("\n-- a name you typed stays typed --\n");
	{
		v.setModel(threeLanes());
		v.renameTrack(2, QStringLiteral("Narration"));
		eqs(v.model().tracks[2].name, "Narration", "the rename lands");

		// The bug: any change to the list used to renumber it away.
		v.addTrack(TlTrack::Kind::Audio); // a new A lane above it
		eqs(v.model().tracks[3].name, "Narration", "and survives another lane being added");
		eqs(v.model().tracks[2].name, "A1", "while the new lane takes the automatic name");
		v.addTrack(TlTrack::Kind::Video);
		v.deleteTrack(0);
		eqs(v.model().tracks[3].name, "Narration", "and lanes coming and going elsewhere");
		// The control: the automatic names DO still renumber, which is what
		// keeps V1 at the bottom after a video lane is removed.
		eqs(v.model().tracks[0].name, "V2", "automatic names are still kept in order");
		eqs(v.model().tracks[1].name, "V1", "with V1 at the bottom");

		// Handing it back. Empty means "number it for me", and so does typing an
		// automatic-looking name yourself: "A7" on the second audio lane would
		// otherwise be a lie the next renumber could not correct.
		v.renameTrack(3, QString());
		eqs(v.model().tracks[3].name, "A2", "an empty name goes back to the numbering");
		v.renameTrack(3, QStringLiteral("A7"));
		eqs(v.model().tracks[3].name, "A2", "and so does a name that only looks automatic");

		ok(TlTrack::isAutoName(QStringLiteral("FX12")) && TlTrack::isAutoName(QStringLiteral("V1")),
		   "FX12 and V1 are automatic names");
		ok(!TlTrack::isAutoName(QStringLiteral("Voice")) && !TlTrack::isAutoName(QStringLiteral("V")) &&
			   !TlTrack::isAutoName(QStringLiteral("A1b")),
		   "Voice, a bare V and A1b are not");
	}

	std::printf("\n-- an effect lane can be hidden, and cannot be muted --\n");
	{
		TimelineModel m = threeLanes();
		TlTrack fx = track(TlTrack::Kind::Effect, "FX1");
		m.tracks.insert(0, fx);
		v.setModel(m);
		QApplication::processEvents();

		// Slot 1 is the H switch on any picture lane.
		const QRect h = v.headerToggleRectForTest(0, 1);
		ok(!h.isEmpty() && h.right() < v.contentRectForTest().x(), "the H switch sits in the gutter");
		click(&v, h.center());
		ok(v.model().tracks[0].hidden, "clicking it hides the effect lane");
		ok(!v.model().tracks[0].muted, "and nothing else flipped");
		click(&v, h.center());
		ok(!v.model().tracks[0].hidden, "and again shows it");

		// Where M would be on a video lane there is nothing on an effect lane.
		const QRect mWould = v.headerToggleRectForTest(0, 2);
		click(&v, mWould.center());
		ok(!v.model().tracks[0].muted && !v.model().tracks[0].hidden,
		   "there is no Mute switch to hit on an effect lane");

		// The controls: video has all three, audio has Lock and Mute with Mute
		// moved up into Hide's slot -- so the audio lane's second switch is M.
		const QRect vh = v.headerToggleRectForTest(1, 1);
		click(&v, vh.center());
		ok(v.model().tracks[1].hidden, "a video lane's second switch is still Hide");
		const QRect am = v.headerToggleRectForTest(3, 2);
		click(&v, am.center());
		ok(v.model().tracks[3].muted && !v.model().tracks[3].hidden,
		   "an audio lane's second switch is Mute, and does not hide it");
	}

	std::printf("\n-- your lanes stay; only lanes that made themselves go --\n");
	{
		v.setModel(threeLanes());
		QApplication::processEvents();
		ok(v.model().tracks.size() == 3, "three lanes to start");

		// A lane from the model is the user's. Emptying it must not remove it.
		dragClipToLane(v, 0, 1);
		ok(v.model().tracks.size() == 3, "moving the only clip off a lane keeps the lane");
		ok(v.model().tracks[0].clips.isEmpty() && v.model().tracks[1].clips.size() == 2,
		   "and the clip really did move");

		// A lane the drop created, on the other hand, is the timeline's own
		// tidy-up: it appears for the drop and goes when the clip leaves.
		v.addClipAt(TlTrack::Kind::Video, clipAt(20000, 9), -1, /*newTrackAt=*/0);
		ok(v.model().tracks.size() == 4, "a drop between lanes makes a lane");
		dragClipToLane(v, 0, 2);
		ok(v.model().tracks.size() == 3, "and moving the clip off it takes it away again");

		// Touching such a lane claims it. Renaming is the clearest case.
		v.addClipAt(TlTrack::Kind::Video, clipAt(20000, 9), -1, 0);
		v.renameTrack(0, QStringLiteral("Overlays"));
		dragClipToLane(v, 0, 2);
		ok(v.model().tracks.size() == 4 && v.model().tracks[0].name == QStringLiteral("Overlays"),
		   "a lane you named is yours, and stays when emptied");
	}

	std::printf("\n-- a locked lane takes no clip, from anywhere --\n");
	{
		v.setModel(threeLanes());
		TimelineModel m = v.model();
		m.tracks[1].locked = true; // V1, the bottom video lane addClip prefers
		v.setModel(m);

		v.addClip(TlTrack::Kind::Video, clipAt(30000, 11));
		ok(v.model().tracks[1].clips.size() == 1, "a menu-added clip skips the locked lane");
		ok(v.model().tracks[0].clips.size() == 2, "and lands on the unlocked one above it");

		// Every video lane locked: rather than forcing it onto one, a fresh
		// lane is made -- and it is an automatic lane, like any other the
		// timeline made for itself.
		m = v.model();
		m.tracks[0].locked = true;
		v.setModel(m);
		v.addClip(TlTrack::Kind::Video, clipAt(40000, 12));
		ok(v.model().tracks.size() == 4, "with every lane locked, a new lane is made instead");
		ok(v.model().tracks[2].kind == TlTrack::Kind::Video && v.model().tracks[2].clips.size() == 1 &&
			   !v.model().tracks[2].locked,
		   "unlocked, below the locked ones, with the clip on it");
		ok(v.model().tracks[0].clips.size() == 2 && v.model().tracks[1].clips.size() == 1,
		   "and the locked lanes are untouched");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
