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
// And the two sound controls a lane was missing: Solo (while any lane is
// soloed, only soloed lanes play; mute still wins) and a per-lane gain that
// multiplies every clip's volume, set from the menu or Ctrl+wheel over the
// header. TimelineModel::trackAudible is the one place mute and solo combine,
// and it is what buildTakes and the mix cache key both read.
//
// Each is pinned with its control: the thing that SHOULD still happen next to
// the thing that should not.
#include "editor/timeline/TimelineView.hpp"
#include "editor/TimelineAudio.hpp" // takeVolume: header-only, so no decoder needed

#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>

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

	// One counter for the whole run, connected once. A block-local counter
	// captured by reference outlives its block through the connection, and
	// every later clipsChanged then writes through a dangling reference into
	// whatever the stack slot holds next -- which at -O1 was a loop index.
	int changes = 0;
	QObject::connect(&v, &TimelineView::clipsChanged, [&changes]() { ++changes; });

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

	std::printf("\n-- solo: one lane alone, the rest go quiet --\n");
	{
		TimelineModel m = threeLanes();
		TlTrack a2 = track(TlTrack::Kind::Audio, "A2");
		a2.clips.append(clipAt(0, 8));
		m.tracks.insert(2, a2); // V2, V1, A2, A1
		v.setModel(m);
		QApplication::processEvents();
		const TimelineModel &mm = v.model();
		ok(!mm.anySolo() && mm.trackAudible(mm.tracks[0]) && mm.trackAudible(mm.tracks[3]),
		   "with nothing soloed every sound lane is heard");

		changes = 0;

		// Slot 3 on a video lane is S, after L H M; slot 3 on an audio lane too,
		// since the logical slots do not move even when the physical ones do.
		const QRect s = v.headerToggleRectForTest(3, 3);
		ok(!s.isEmpty() && s.right() < v.contentRectForTest().x(), "the S switch sits in the gutter");
		click(&v, s.center());
		ok(mm.tracks[3].solo, "clicking it soloes the lane");
		ok(mm.trackAudible(mm.tracks[3]), "which is still heard");
		ok(!mm.trackAudible(mm.tracks[2]) && !mm.trackAudible(mm.tracks[0]) &&
			   !mm.trackAudible(mm.tracks[1]),
		   "and every other sound lane -- video lanes included -- is not");
		ok(!mm.tracks[2].muted, "without anything being MARKED muted");
		ok(changes >= 1, "and the mix is told, so the preview follows");

		// Two soloed lanes are both heard: solo is a set, not a radio button.
		click(&v, v.headerToggleRectForTest(2, 3).center());
		ok(mm.trackAudible(mm.tracks[2]) && mm.trackAudible(mm.tracks[3]) &&
			   !mm.trackAudible(mm.tracks[0]),
		   "a second solo joins the first rather than replacing it");

		// Mute still wins on a soloed lane, as on a desk.
		click(&v, v.headerToggleRectForTest(3, 2).center());
		ok(mm.tracks[3].solo && mm.tracks[3].muted && !mm.trackAudible(mm.tracks[3]),
		   "a lane both soloed and muted is silent");

		// Unsolo everything and the rest comes back.
		click(&v, v.headerToggleRectForTest(2, 3).center());
		click(&v, v.headerToggleRectForTest(3, 3).center());
		ok(!mm.anySolo() && mm.trackAudible(mm.tracks[0]) && mm.trackAudible(mm.tracks[2]),
		   "with the last solo off, the others are heard again");

		// An effect lane has no S, and a stale solo flag on it counts for nothing.
		TimelineModel fxm = threeLanes();
		TlTrack fx = track(TlTrack::Kind::Effect, "FX1");
		fx.solo = true; // as a hand-edited project could say
		fxm.tracks.insert(0, fx);
		v.setModel(fxm);
		QApplication::processEvents();
		ok(!v.model().anySolo() && v.model().trackAudible(v.model().tracks[3]),
		   "a soloed effect lane soloes nothing: it has no sound to solo");
		v.setTrackSolo(0, true);
		click(&v, v.headerToggleRectForTest(0, 3).center());
		ok(!v.model().anySolo(), "and neither the setter nor a click can solo it");
		ok(v.headerToggleRectForTest(1, 3).right() < v.contentRectForTest().x(),
		   "the fourth chip on a video lane still fits inside the gutter");
	}

	std::printf("\n-- gain: one level for the whole lane --\n");
	{
		v.setModel(threeLanes());
		QApplication::processEvents();
		changes = 0;

		v.setTrackGain(2, 0.6);
		ok(qAbs(v.model().tracks[2].gain - 0.6) < 1e-9, "the setter sets it");
		ok(changes >= 1, "and the mix is told");
		v.setTrackGain(2, 7.0);
		ok(qAbs(v.model().tracks[2].gain - 2.0) < 1e-9, "clamped to 2.0 at the top");
		v.setTrackGain(2, -1.0);
		ok(qAbs(v.model().tracks[2].gain) < 1e-9, "and to silence at the bottom");
		const QRect gr = v.headerGainRectForTest(2);
		ok(!gr.isEmpty() && gr.right() < v.contentRectForTest().x(), "the readout is inside the gutter");
		ok(v.headerGainRectForTest(0).isEmpty() == false, "a video lane has one too (it carries sound)");

		// Ctrl+wheel over the header: 5% a notch, 1% with Shift, nothing without Ctrl.
		v.setTrackGain(2, 1.0);
		const QPoint hp(gr.center().x(), v.headerToggleRectForTest(2, 0).center().y());
		auto wheel = [&](int dy, Qt::KeyboardModifiers mods) {
			QWheelEvent e(QPointF(hp), QPointF(v.mapToGlobal(hp)), QPoint(), QPoint(0, dy),
				      Qt::NoButton, mods, Qt::NoScrollPhase, false);
			QApplication::sendEvent(&v, &e);
		};
		wheel(120, Qt::ControlModifier);
		ok(TimelineView::gainPercent(v.model().tracks[2].gain) == 105, "Ctrl+wheel up is +5%");
		wheel(-120, Qt::ControlModifier | Qt::ShiftModifier);
		ok(TimelineView::gainPercent(v.model().tracks[2].gain) == 104, "with Shift, 1% a notch");
		wheel(120, Qt::NoModifier);
		ok(TimelineView::gainPercent(v.model().tracks[2].gain) == 104,
		   "a plain wheel over the gutter leaves the gain alone (it scrolls)");

		TimelineModel fxm = threeLanes();
		fxm.tracks.insert(0, track(TlTrack::Kind::Effect, "FX1"));
		v.setModel(fxm);
		v.setTrackGain(0, 0.5);
		ok(qAbs(v.model().tracks[0].gain - 1.0) < 1e-9 && v.headerGainRectForTest(0).isEmpty(),
		   "an effect lane has no gain and no readout");
	}

	std::printf("\n-- a duplicated lane keeps its name, numbered --\n");
	{
		v.setModel(threeLanes());
		v.renameTrack(2, QStringLiteral("Narration"));
		TimelineModel m = v.model();
		// The copy goes ABOVE the original (a smaller index), so the original
		// moves down one.
		v.pasteTrack(m.tracks[2], 2);
		eqs(v.model().tracks[2].name, "Narration 2", "the copy of Narration is Narration 2");
		eqs(v.model().tracks[3].name, "Narration", "and the original keeps its name");
		v.pasteTrack(v.model().tracks[2], 2);
		eqs(v.model().tracks[2].name, "Narration 3",
		    "a copy of Narration 2 is Narration 3, not Narration 2 2");
		ok(v.model().tracks[2].clips.size() == 1, "with the clips along");

		// An automatic name still renumbers: two lanes both called V1 is the
		// thing this must never produce.
		v.pasteTrack(v.model().tracks[1], 1); // V1
		eqs(v.model().tracks[1].name, "V2", "a copy of V1 takes the next automatic name");
		eqs(v.model().tracks[0].name, "V3", "and the lane above it renumbers to match");
	}

	std::printf("\n-- a clip's volume counts on a video lane too --\n");
	{
		TlTrack vt = track(TlTrack::Kind::Video, "V1");
		TlTrack at = track(TlTrack::Kind::Audio, "A1");
		TlClip c = clipAt(0);
		c.volume = 0.5;
		ok(qAbs(TimelineAudio::takeVolume(at, c) - 0.5) < 1e-9, "on an audio lane, as before");
		ok(qAbs(TimelineAudio::takeVolume(vt, c) - 0.5) < 1e-9,
		   "and on a video lane, where it used to be pinned to unity");
		vt.gain = 0.5;
		ok(qAbs(TimelineAudio::takeVolume(vt, c) - 0.25) < 1e-9, "times the lane's gain");
		c.volume = 9.0;
		ok(qAbs(TimelineAudio::takeVolume(at, c) - 2.0) < 1e-9,
		   "a volume from a hand-edited project is clamped to 2.0 first");
	}

	std::printf("\n-- reordering lanes --\n");
	{
		// V3(src 1), V2(src 2), V1(src 3), A1(src 4), A2(src 5)
		TimelineModel m;
		const char *vn[] = {"V3", "V2", "V1"};
		for (int i = 0; i < 3; ++i) {
			TlTrack t = track(TlTrack::Kind::Video, vn[i]);
			t.clips.append(clipAt(0, i + 1));
			m.tracks.append(t);
		}
		const char *an[] = {"A1", "A2"};
		for (int i = 0; i < 2; ++i) {
			TlTrack t = track(TlTrack::Kind::Audio, an[i]);
			t.clips.append(clipAt(0, i + 4));
			m.tracks.append(t);
		}
		v.setModel(m);
		QApplication::processEvents();
		const auto src = [&v](int i) { return v.model().tracks[i].clips[0].sourceId; };

		changes = 0;

		// moveTrack(from, to): `to` is "before lane `to`" in the current list.
		ok(v.moveTrack(0, 3) == 2, "moving the top video lane below the others lands at index 2");
		ok(src(0) == 2 && src(1) == 3 && src(2) == 1, "and the order is what was asked for");
		eqs(v.model().tracks[2].name, "V1", "the lane that is now at the bottom is V1");
		eqs(v.model().tracks[0].name, "V3", "and the one now on top is V3: names follow slots");
		ok(changes >= 1, "the composite is told (the stacking order changed)");

		// No-ops: before itself and after itself.
		ok(v.moveTrack(1, 1) == -1 && v.moveTrack(1, 2) == -1, "dropping a lane where it is does nothing");
		ok(src(0) == 2 && src(1) == 3 && src(2) == 1, "and leaves the order alone");

		// Clamped into the kind's group: a video lane cannot go among the audio.
		ok(v.moveTrack(0, 5) == 2, "a video lane pushed past the audio stops at the bottom of the picture group");
		ok(v.model().tracks[3].kind == TlTrack::Kind::Audio && src(3) == 4,
		   "with the audio lanes untouched");
		ok(v.moveTrack(4, 0) == 3, "and an audio lane pulled up stops at the top of the audio group");
		ok(src(3) == 5 && src(4) == 4, "having swapped the two audio lanes");
		eqs(v.model().tracks[3].name, "A1", "and A1 is whichever is now on top");
	}

	std::printf("\n-- dragging a header reorders --\n");
	{
		TimelineModel m;
		const char *vn[] = {"V3", "V2", "V1"};
		for (int i = 0; i < 3; ++i) {
			TlTrack t = track(TlTrack::Kind::Video, vn[i]);
			t.clips.append(clipAt(0, i + 1));
			m.tracks.append(t);
		}
		TlTrack a = track(TlTrack::Kind::Audio, "A1");
		a.clips.append(clipAt(0, 9));
		m.tracks.append(a);
		v.setModel(m);
		QApplication::processEvents();
		const auto src = [&v](int i) { return v.model().tracks[i].clips[0].sourceId; };
		// A point on a header away from its toggles: the top-right corner area.
		const auto headerGrab = [&v](int track) {
			const QRect l = v.headerToggleRectForTest(track, 0);
			return QPoint(v.contentRectForTest().x() - 6, l.top() - 12);
		};

		// Select the middle lane's clip first: the selection must follow the LANE.
		const QRect cr = v.clipRectForTest(1, 0);
		click(&v, QPoint(cr.center()));
		ok(v.selectedTrack() == 1, "the clip on lane 1 is selected");

		// Drag the top header down to the bottom of the video group.
		const QPoint from = headerGrab(0);
		const QPoint to(from.x(), v.clipRectForTest(2, 0).bottom() + 2);
		press(&v, from);
		ok(v.selectedHeaderTrack() == 0, "pressing a header selects the track");
		ok(v.selectedTrack() == -1, "and lets go of the clip selection");
		moveTo(&v, from + QPoint(0, 2));
		release(&v, from + QPoint(0, 2));
		ok(src(0) == 1 && src(1) == 2 && src(2) == 3, "a two-pixel wobble is a click, not a move");

		press(&v, from);
		moveTo(&v, from + QPoint(0, 10));
		moveTo(&v, to);
		release(&v, to);
		QApplication::processEvents();
		ok(src(0) == 2 && src(1) == 3 && src(2) == 1, "dragging the top header past the others moves the lane to the bottom");
		ok(v.selectedHeaderTrack() == 2, "and the header selection followed it");
		ok(src(3) == 9, "the audio lane did not move");

		// Past the audio lane: still clamped to the picture group.
		const QPoint far(from.x(), v.clipRectForTest(3, 0).bottom() + 2);
		press(&v, headerGrab(0));
		moveTo(&v, headerGrab(0) + QPoint(0, 10));
		moveTo(&v, far);
		release(&v, far);
		ok(v.model().tracks[3].kind == TlTrack::Kind::Audio && src(2) == 2,
		   "a header dragged below the audio stops at the bottom of the picture group");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
