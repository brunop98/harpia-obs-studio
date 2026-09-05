// The volume rubber-band: a line across an audio clip whose HEIGHT is its level.
//
// Three things have to hold, and the third is the one that makes it useful
// rather than decorative:
//
//   - the mapping. Bottom is silence, top is 2.00x, unity across the middle --
//     the same numbers as the Inspector's slider, because two controls for one
//     value that disagree about its scale are two controls you have to convert
//     between in your head.
//   - the grab is the LINE. Everywhere else on the clip still moves it, which
//     is checked against the control of doing the identical gesture on the
//     clip's body.
//   - dragging it emits clipsChanged, every time. That signal is what rebuilds
//     the audio mix, so it is the whole of "and it updates the preview" as far
//     as this widget is concerned. A drag that only repainted would look
//     perfect and be silent.
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QMouseEvent>

#include <cmath>
#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static void near(double got, double want, const char *w, double tol = 0.02)
{
	const bool good = std::abs(got - want) <= tol;
	std::printf("  %s %s (got %.3f, want %.3f)\n", good ? "PASS" : "FAIL", w, got, want);
	if (!good)
		++failures;
}

static void press(QWidget *w, QPoint p, Qt::KeyboardModifiers m = Qt::NoModifier)
{
	QMouseEvent e(QEvent::MouseButtonPress, QPointF(p), w->mapToGlobal(p), Qt::LeftButton,
		      Qt::LeftButton, m);
	QApplication::sendEvent(w, &e);
}
static void moveTo(QWidget *w, QPoint p, Qt::KeyboardModifiers m = Qt::NoModifier)
{
	QMouseEvent e(QEvent::MouseMove, QPointF(p), w->mapToGlobal(p), Qt::NoButton, Qt::LeftButton, m);
	QApplication::sendEvent(w, &e);
}
static void release(QWidget *w, QPoint p)
{
	QMouseEvent e(QEvent::MouseButtonRelease, QPointF(p), w->mapToGlobal(p), Qt::LeftButton,
		      Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}

// One audio track with a 10 s clip, and one video track with another: the line
// belongs to the first and not the second.
static TimelineModel twoTracks()
{
	TimelineModel m;
	TlClip c;
	c.type = TlClip::Type::Video; // a media clip; the TRACK decides it is audio
	c.sourceId = 7;
	c.srcStartMs = 0;
	c.srcEndMs = 10000;
	c.outStartMs = 0;
	c.volume = 1.0;

	TlTrack v;
	v.kind = TlTrack::Kind::Video;
	v.name = QStringLiteral("V1");
	v.clips.append(c);
	TlTrack a;
	a.kind = TlTrack::Kind::Audio;
	a.name = QStringLiteral("A1");
	a.clips.append(c);
	m.tracks.append(v);
	m.tracks.append(a);
	return m;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	TimelineView v;
	v.resize(900, 320);
	v.show();
	QApplication::processEvents();
	v.setModel(twoTracks());
	QApplication::processEvents();

	const int kAudio = 1; // the audio track's index in the model above

	std::printf("\n-- the line's height is the level --\n");
	{
		const QRect r = v.clipRectForTest(kAudio, 0);
		const int atUnity = v.volumeLineYForTest(kAudio, 0);
		std::printf("     clip y %d..%d, unity line at %d\n", r.top(), r.bottom(), atUnity);
		ok(atUnity > r.top() && atUnity < r.bottom(), "unity sits inside the clip");
		ok(std::abs(atUnity - r.center().y()) <= 3, "and across the middle of it");

		TimelineModel m = v.model();
		m.tracks[kAudio].clips[0].volume = 0.0;
		v.setModel(m);
		const int atZero = v.volumeLineYForTest(kAudio, 0);
		m.tracks[kAudio].clips[0].volume = 2.0;
		v.setModel(m);
		const int atMax = v.volumeLineYForTest(kAudio, 0);
		std::printf("     silence at %d, maximum at %d\n", atZero, atMax);
		ok(atZero > atUnity && atMax < atUnity, "silence is below unity and boost above it");
		// Both extremes stay ON the clip: a line drawn on the very edge is one
		// you can neither see nor grab.
		ok(atZero <= r.bottom() && atZero > r.bottom() - 12, "silence is inset from the bottom");
		ok(atMax >= r.top() && atMax < r.top() + 12, "and the maximum from the top");

		m.tracks[kAudio].clips[0].volume = 1.0;
		v.setModel(m);
		QApplication::processEvents();
	}

	std::printf("\n-- only on an audio track --\n");
	{
		ok(v.volumeLineYForTest(0, 0) < 0, "a clip on a VIDEO track has no line");
		const QRect vr = v.clipRectForTest(0, 0);
		ok(!v.volumeHitForTest(QPoint(vr.center().x(), vr.center().y()), nullptr, nullptr),
		   "and nothing to grab there");
	}

	std::printf("\n-- dragging it sets the level, and says so --\n");
	{
		int changes = 0;
		QObject::connect(&v, &TimelineView::clipsChanged, [&changes]() { ++changes; });

		const QRect r = v.clipRectForTest(kAudio, 0);
		const int y = v.volumeLineYForTest(kAudio, 0);
		const QPoint on(r.center().x(), y);

		int ht = -1, hc = -1;
		ok(v.volumeHitForTest(on, &ht, &hc) && ht == kAudio && hc == 0,
		   "the line is grabbable at its own height");
		ok(!v.volumeHitForTest(QPoint(on.x(), y + 30), nullptr, nullptr),
		   "and not from well below it");

		const qint64 startedAt = v.model().tracks[kAudio].clips[0].outStartMs;
		press(&v, on);
		// Upwards is louder. Shift throughout, so the unity snap does not pull
		// the value back and hide a broken mapping.
		for (int dy = 4; dy <= 16; dy += 4)
			moveTo(&v, QPoint(on.x(), y - dy), Qt::ShiftModifier);
		const double mid = v.model().tracks[kAudio].clips[0].volume;
		release(&v, QPoint(on.x(), y - 16));
		QApplication::processEvents();

		const TlClip &c = v.model().tracks[kAudio].clips[0];
		std::printf("     volume 1.00 -> %.3f after dragging up 16px\n", c.volume);
		ok(c.volume > 1.0, "dragging up made it louder");
		ok(mid > 1.0, "and it was already louder DURING the drag, not only on release");
		ok(c.outStartMs == startedAt, "the clip did not move");
		ok(changes >= 3, "clipsChanged fired on every step, which is what re-mixes the audio");

		// THE CONTROL. Without it, "the clip did not move" is also what a
		// timeline that ignores every drag would report.
		//
		// Below the line's grab range, but ABOVE the lane's bottom edge: the
		// last 7 px of a lane are the drop band that means "make a new lane
		// here", and a press in it would move the clip onto a fresh lane rather
		// than along this one. (It did, once: the emptied lane used to be
		// tidied away, so the indices happened to line up. Now a lane you were
		// given stays when emptied, and the control has to mean what it says.)
		const QPoint body(r.center().x(), r.bottom() - 9);
		press(&v, body);
		moveTo(&v, body + QPoint(80, 0), Qt::ShiftModifier);
		release(&v, body + QPoint(80, 0));
		ok(v.model().tracks.size() == 2 && v.model().tracks[kAudio].clips.size() == 1,
		   "the clip stayed on its own lane");
		ok(v.model().tracks[kAudio].clips[0].outStartMs != startedAt,
		   "while the same gesture lower down DOES move the clip");
	}

	std::printf("\n-- and it stops on unity unless you say otherwise --\n");
	{
		TimelineModel m = twoTracks();
		m.tracks[kAudio].clips[0].volume = 1.4;
		v.setModel(m);
		QApplication::processEvents();

		const QRect r = v.clipRectForTest(kAudio, 0);
		const int from = v.volumeLineYForTest(kAudio, 0);
		// Aim two pixels shy of where unity is: close enough to be meant.
		m.tracks[kAudio].clips[0].volume = 1.0;
		v.setModel(m);
		const int unityY = v.volumeLineYForTest(kAudio, 0);
		m.tracks[kAudio].clips[0].volume = 1.4;
		v.setModel(m);
		QApplication::processEvents();

		press(&v, QPoint(r.center().x(), from));
		moveTo(&v, QPoint(r.center().x(), unityY - 2));
		const double snapped = v.model().tracks[kAudio].clips[0].volume;
		release(&v, QPoint(r.center().x(), unityY - 2));
		near(snapped, 1.0, "a level dropped near unity lands exactly on it", 1e-9);

		// Shift is the exact modifier, and it does both halves of exact: no
		// snap, AND a quarter-speed drag. An audio lane is about thirty pixels
		// tall -- the whole 0..2 range in thirty steps -- so without the second
		// half there is no gesture that produces 1.05 at all.
		TimelineModel m2 = v.model();
		m2.tracks[kAudio].clips[0].volume = 1.4;
		v.setModel(m2);
		QApplication::processEvents();
		const int fineFrom = v.volumeLineYForTest(kAudio, 0);
		const int dy = fineFrom - (unityY - 2);
		press(&v, QPoint(r.center().x(), fineFrom));
		moveTo(&v, QPoint(r.center().x(), unityY - 2), Qt::ShiftModifier);
		const double fine = v.model().tracks[kAudio].clips[0].volume;
		release(&v, QPoint(r.center().x(), unityY - 2));
		std::printf("     %d px down from 1.40: normal would be ~%.2f, fine gave %.3f\n", dy,
			    1.4 - dy * (2.0 / 30.0), fine);
		ok(std::abs(fine - 1.0) > 1e-9, "Shift does not snap it to unity");
		ok(fine < 1.4 && fine > 1.0,
		   "and it moved a QUARTER as far, which is what makes a value between the "
		   "pixels reachable at all");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
