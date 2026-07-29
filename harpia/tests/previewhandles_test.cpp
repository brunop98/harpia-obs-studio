// Resize and rotate the selected clip by its grips in the preview.
//
// The preview used to offer exactly two gestures: drag anywhere to move, wheel
// to zoom. Rotation could only be typed into a spin box, scale could only be
// scrolled, and a drag started from ANYWHERE on the canvas moved the selected
// clip — so it was easy to nudge something you were not looking at.
//
// The failure modes are all invisible from a screenshot of the final pose:
//
//   - the grips drawn but not hit-testable, so the drag falls through to a move
//     and the clip slides instead of scaling;
//   - the scale applied to the LIVE pose each mouse-move instead of the pose the
//     gesture started from, which compounds and throws the clip off screen;
//   - a corner drag on a TURNED clip resizing along the screen's axes rather
//     than the clip's, so the clip walks sideways as it grows;
//   - a whole drag landing as thirty undo entries rather than one.
#include "editor/EditorWidgets.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QPushButton>
#include <QThread>

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
static void settle(int ms)
{
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < ms) {
		QApplication::processEvents();
		QThread::msleep(5);
	}
}
static QPushButton *button(QWidget *w, const QString &text)
{
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (b->text() == text)
			return b;
	return nullptr;
}

// A real press-move-release, the way the window system delivers one.
static void dragMouse(QWidget *w, QPoint from, QPoint to, int steps = 6)
{
	QMouseEvent press(QEvent::MouseButtonPress, QPointF(from), w->mapToGlobal(from),
			  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(w, &press);
	for (int i = 1; i <= steps; ++i) {
		const QPoint p(from.x() + (to.x() - from.x()) * i / steps,
			       from.y() + (to.y() - from.y()) * i / steps);
		QMouseEvent move(QEvent::MouseMove, QPointF(p), w->mapToGlobal(p), Qt::NoButton,
				 Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(w, &move);
		QApplication::processEvents();
	}
	QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(to), w->mapToGlobal(to), Qt::LeftButton,
			Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(w, &rel);
	QApplication::processEvents();
}

static void buildOneClip(TimelineView *tv)
{
	TimelineModel m;
	TlTrack t;
	t.kind = TlTrack::Kind::Video;
	TlClip c;
	c.type = TlClip::Type::Text;
	c.srcStartMs = 0;
	c.srcEndMs = 4000;
	c.outStartMs = 0;
	c.text.text = QStringLiteral("Handle me");
	t.clips.append(c);
	m.tracks.append(t);
	tv->setModel(m);
	emit tv->editCommitted(); // a baseline for undo, which setModel does not record
	QApplication::processEvents();
}
static const TlClip &clip(TimelineView *tv)
{
	return tv->model().tracks[0].clips[0];
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	VideoEditorWindow w(QStringLiteral(SRC_MEDIA));
	w.resize(1300, 900);
	w.show();
	QApplication::processEvents();
	if (QPushButton *full = button(&w, QStringLiteral("Full Editing"))) {
		full->click();
		settle(400);
	}
	TimelineView *tv = w.findChild<TimelineView *>();
	PreviewCanvas *pc = w.findChild<PreviewCanvas *>();
	ok(tv && pc, "the editor came up with a preview canvas");
	if (!tv || !pc)
		return 1;

	buildOneClip(tv);
	tv->selectClip(0, 0);
	settle(700);
	ok(pc->transformMode(), "selecting a clip arms the preview's transform tools");

	// Where the grips are: the canvas computes them, and the test has to press
	// exactly on one, so it asks rather than guessing at the geometry.
	const QVector<QPointF> h = pc->handlePointsForTest();
	std::printf("     grips reported: %d\n", int(h.size()));
	ok(h.size() == 9, "eight resize grips and a rotate knob");
	if (h.size() != 9)
		return 1;
	const QPoint tl = h[0].toPoint();       // top-left
	const QPoint rot = h[8].toPoint();      // the rotate knob
	const QPointF centre = (h[0] + h[7]) / 2.0; // TL and BR average to the centre

	std::printf("\n-- dragging a corner grip scales, it does not slide --\n");
	{
		const double s0 = clip(tv).scale;
		const double x0 = clip(tv).posX, y0 = clip(tv).posY;
		// Outward along the diagonal, away from the centre: bigger.
		const QPoint out(tl.x() - 60, tl.y() - 34);
		dragMouse(pc, tl, out);
		settle(600);
		std::printf("     scale %.3f -> %.3f, pos (%.3f,%.3f) -> (%.3f,%.3f)\n", s0,
			    clip(tv).scale, x0, y0, clip(tv).posX, clip(tv).posY);
		ok(clip(tv).scale > s0 * 1.05, "dragging the corner out made it bigger");
		// If the grip fell through to the body-move path this is what moves.
		ok(std::abs(clip(tv).posX - x0) < 1e-6 && std::abs(clip(tv).posY - y0) < 1e-6,
		   "and did not move it, so the grip was hit rather than the body");
	}

	std::printf("\n-- the scale is measured from the press, not compounded --\n");
	{
		// The answer is not a matter of taste: a corner grip dragged from radius
		// r0 to r1 about the centre scales by exactly r1/r0. Applied against the
		// LIVE pose on each of six mouse-moves instead of the pose at the press,
		// the per-move factors multiply together and the result overshoots --
		// which a loose "it did not explode" bound does NOT catch. Measured
		// here: 1.096 correct vs 1.376 compounded, so the bound has to be the
		// geometry, not a ceiling.
		buildOneClip(tv);
		tv->selectClip(0, 0);
		settle(700);
		const QVector<QPointF> g = pc->handlePointsForTest();
		if (g.size() == 9) {
			const QPointF c = (g[0] + g[7]) / 2.0;
			const QPoint corner = g[0].toPoint();
			const QPoint to(corner.x() - 40, corner.y() - 22);
			const double r0 = std::hypot(corner.x() - c.x(), corner.y() - c.y());
			const double r1 = std::hypot(to.x() - c.x(), to.y() - c.y());
			const double want = r1 / r0; // the clip starts at scale 1
			dragMouse(pc, corner, to, 6);
			settle(600);
			const double got = clip(tv).scale;
			std::printf("     after a 6-step drag: %.3f (the geometry says %.3f)\n", got,
				    want);
			ok(std::abs(got - want) < 0.02,
			   "the scale is exactly the grip's distance ratio");
		}
	}

	std::printf("\n-- the rotate knob rotates --\n");
	{
		buildOneClip(tv);
		tv->selectClip(0, 0);
		settle(700);
		const QVector<QPointF> g = pc->handlePointsForTest();
		if (g.size() == 9) {
			const QPointF c = (g[0] + g[7]) / 2.0;
			const QPoint knob = g[8].toPoint();
			// Swing the knob a quarter turn clockwise: from above the centre
			// to the right of it, at the same radius.
			const double r = std::hypot(knob.x() - c.x(), knob.y() - c.y());
			const QPoint side(int(c.x() + r), int(c.y()));
			const double before = clip(tv).rotation;
			dragMouse(pc, knob, side);
			settle(600);
			std::printf("     rotation %.1f -> %.1f degrees\n", before,
				    clip(tv).rotation);
			ok(std::abs(clip(tv).rotation - before) > 45.0, "it turned a long way");
			ok(std::abs(clip(tv).rotation) < 180.1, "and stayed in a sane range");
		}
	}

	std::printf("\n-- a whole drag is one undo, not one per mouse-move --\n");
	{
		buildOneClip(tv);
		tv->selectClip(0, 0);
		settle(700);
		const double s0 = clip(tv).scale;
		const QVector<QPointF> g = pc->handlePointsForTest();
		if (g.size() == 9) {
			const QPoint corner = g[0].toPoint();
			dragMouse(pc, corner, QPoint(corner.x() - 50, corner.y() - 28), 8);
			settle(700);
			const double s1 = clip(tv).scale;
			ok(std::abs(s1 - s0) > 1e-6, "the drag changed the scale");
			QMetaObject::invokeMethod(&w, "undo");
			settle(600);
			std::printf("     scale %.3f -> %.3f -> %.3f after one undo\n", s0, s1,
				    clip(tv).scale);
			ok(std::abs(clip(tv).scale - s0) < 1e-6,
			   "and one undo put it all the way back");
		}
	}

	std::printf("\n-- dragging empty canvas no longer moves the clip --\n");
	{
		buildOneClip(tv);
		tv->selectClip(0, 0);
		settle(700);
		const double x0 = clip(tv).posX, y0 = clip(tv).posY;
		// The very corner of the widget: outside any clip's box.
		dragMouse(pc, QPoint(4, 4), QPoint(80, 60));
		settle(500);
		std::printf("     pos (%.3f,%.3f) -> (%.3f,%.3f)\n", x0, y0, clip(tv).posX,
			    clip(tv).posY);
		ok(std::abs(clip(tv).posX - x0) < 1e-6 && std::abs(clip(tv).posY - y0) < 1e-6,
		   "the clip stayed put — a drag has to start ON it");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
