// The pointer over the preview's grips says what a press will do.
//
// The arithmetic first: which of Qt's four resize cursors a grip on a turned
// box wants. Then the widget: hover each handle of a real transform box, at
// zero and at 45 degrees, and read the cursor back -- because the complaint
// was "it is hard to know if the mouse is in the right position", and the
// only proof that it is now easy is the cursor changing exactly there.
#include "editor/EditorWidgets.hpp"
#include "editor/GripCursor.hpp"

#include <QApplication>
#include <QImage>
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

static const char *shapeName(Qt::CursorShape s)
{
	switch (s) {
	case Qt::ArrowCursor: return "arrow";
	case Qt::OpenHandCursor: return "open hand";
	case Qt::ClosedHandCursor: return "closed hand";
	case Qt::SizeHorCursor: return "size <->";
	case Qt::SizeVerCursor: return "size ^v";
	case Qt::SizeFDiagCursor: return "size \\";
	case Qt::SizeBDiagCursor: return "size /";
	case Qt::BitmapCursor: return "bitmap (rotate)";
	case Qt::CrossCursor: return "cross";
	default: return "other";
	}
}

static void hover(QWidget *w, QPointF p)
{
	QMouseEvent e(QEvent::MouseMove, p, w->mapToGlobal(p.toPoint()), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}

static void expectCursor(PreviewCanvas &c, QPointF at, Qt::CursorShape want, const char *w)
{
	hover(&c, at);
	const Qt::CursorShape got = c.cursor().shape();
	std::printf("  %s %s (got %s, want %s)\n", got == want ? "PASS" : "FAIL", w, shapeName(got),
		    shapeName(want));
	if (got != want)
		++failures;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- the arithmetic --\n");
	{
		ok(resizeCursorForAxis(0) == Qt::SizeHorCursor && resizeCursorForAxis(180) == Qt::SizeHorCursor &&
			   resizeCursorForAxis(-10) == Qt::SizeHorCursor,
		   "a horizontal axis is the <-> cursor, whichever way round");
		ok(resizeCursorForAxis(90) == Qt::SizeVerCursor && resizeCursorForAxis(270) == Qt::SizeVerCursor,
		   "vertical is ^v");
		ok(resizeCursorForAxis(45) == Qt::SizeFDiagCursor && resizeCursorForAxis(225) == Qt::SizeFDiagCursor,
		   "45 degrees (y down) is the \\ diagonal");
		ok(resizeCursorForAxis(135) == Qt::SizeBDiagCursor && resizeCursorForAxis(-45) == Qt::SizeBDiagCursor,
		   "135, or -45, is the / diagonal");
		ok(resizeCursorForAxis(22) == Qt::SizeHorCursor && resizeCursorForAxis(23) == Qt::SizeFDiagCursor,
		   "the boundary between them is at 22.5");

		ok(gripAxisDeg(GripIndex::R, 0) == 0 && gripAxisDeg(GripIndex::T, 0) == 90 &&
			   gripAxisDeg(GripIndex::BR, 0) == 45 && gripAxisDeg(GripIndex::TR, 0) == -45,
		   "on an upright box the side grips are H and V and the corners are the diagonals");
		ok(resizeCursorForAxis(gripAxisDeg(GripIndex::T, 45)) == Qt::SizeBDiagCursor &&
			   resizeCursorForAxis(gripAxisDeg(GripIndex::R, 45)) == Qt::SizeFDiagCursor &&
			   resizeCursorForAxis(gripAxisDeg(GripIndex::TR, 45)) == Qt::SizeHorCursor,
		   "turn the box 45 degrees and every grip's cursor turns with it");
		ok(resizeCursorForAxis(gripAxisDeg(GripIndex::R, 90)) == Qt::SizeVerCursor,
		   "at 90 degrees the right grip is a vertical resize");
		ok(rotateCursor().shape() == Qt::BitmapCursor && !rotateCursor().pixmap().isNull(),
		   "the rotate cursor is a drawn one, since Qt has none");
	}

	std::printf("\n-- on the canvas --\n");
	{
		PreviewCanvas c;
		c.resize(800, 450);
		c.show();
		QApplication::processEvents();
		QImage frame(1920, 1080, QImage::Format_RGB32);
		frame.fill(Qt::darkGray);
		c.setVideoSize(1920, 1080); // the canvas shape; the frame alone does not set it
		c.setFrame(frame);
		c.setTransformMode(true);
		c.setTransformBox(QRectF(480, 270, 960, 540), 0.0); // the middle half of the canvas
		QApplication::processEvents();

		const QVector<QPointF> h = c.handlePointsForTest();
		ok(h.size() == 9, "nine handles: eight grips and the rotate knob");
		if (h.size() == 9) {
			expectCursor(c, h[4], Qt::SizeHorCursor, "the right grip: <->");
			expectCursor(c, h[3], Qt::SizeHorCursor, "the left grip: <->");
			expectCursor(c, h[1], Qt::SizeVerCursor, "the top grip: ^v");
			expectCursor(c, h[6], Qt::SizeVerCursor, "the bottom grip: ^v");
			expectCursor(c, h[0], Qt::SizeFDiagCursor, "top-left: \\");
			expectCursor(c, h[7], Qt::SizeFDiagCursor, "bottom-right: \\");
			expectCursor(c, h[2], Qt::SizeBDiagCursor, "top-right: /");
			expectCursor(c, h[5], Qt::SizeBDiagCursor, "bottom-left: /");
			expectCursor(c, h[8], Qt::BitmapCursor, "the rotate knob: the rotate cursor");
			const QPointF centre = (h[0] + h[7]) / 2.0;
			expectCursor(c, centre, Qt::OpenHandCursor, "the body: an open hand");
			expectCursor(c, QPointF(2, 2), Qt::ArrowCursor, "off the clip: a plain arrow, since a press there does nothing");
			// Just outside a grip's 8 px is the body or nothing, not the grip.
			expectCursor(c, h[4] + QPointF(14, 0), Qt::ArrowCursor, "14 px past the right grip is outside the clip");
		}

		// Turn it: the cursors follow the box.
		c.setTransformBox(QRectF(480, 270, 960, 540), 45.0);
		QApplication::processEvents();
		const QVector<QPointF> t = c.handlePointsForTest();
		if (t.size() == 9) {
			expectCursor(c, t[1], Qt::SizeBDiagCursor, "at 45 degrees the top grip is a / resize");
			expectCursor(c, t[4], Qt::SizeFDiagCursor, "and the right grip a \\ resize");
			expectCursor(c, t[2], Qt::SizeHorCursor, "and the top-right corner a <-> resize");
		}

		// Pressing a grip keeps the same cursor; the body closes the hand.
		c.setTransformBox(QRectF(480, 270, 960, 540), 0.0);
		QApplication::processEvents();
		const QVector<QPointF> u = c.handlePointsForTest();
		if (u.size() == 9) {
			hover(&c, u[4]);
			QMouseEvent pr(QEvent::MouseButtonPress, u[4], c.mapToGlobal(u[4].toPoint()), Qt::LeftButton,
				       Qt::LeftButton, Qt::NoModifier);
			QApplication::sendEvent(&c, &pr);
			ok(c.cursor().shape() == Qt::SizeHorCursor, "pressing the right grip keeps the <-> cursor");
			QMouseEvent rl(QEvent::MouseButtonRelease, u[4], c.mapToGlobal(u[4].toPoint()), Qt::LeftButton,
				       Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(&c, &rl);
			const QPointF centre = (u[0] + u[7]) / 2.0;
			hover(&c, centre);
			QMouseEvent pb(QEvent::MouseButtonPress, centre, c.mapToGlobal(centre.toPoint()), Qt::LeftButton,
				       Qt::LeftButton, Qt::NoModifier);
			QApplication::sendEvent(&c, &pb);
			ok(c.cursor().shape() == Qt::ClosedHandCursor, "pressing the body closes the hand");
			QMouseEvent rb(QEvent::MouseButtonRelease, centre, c.mapToGlobal(centre.toPoint()), Qt::LeftButton,
				       Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(&c, &rb);
		}

		// Crop mode has its own box, never turned.
		c.setTransformMode(false);
		c.setCropEnabled(true);
		QApplication::processEvents();
		// The 16:9 frame fills the 16:9 widget, so the widget's own middle and
		// right edge are the crop box's.
		expectCursor(c, QPointF(c.width() / 2.0, c.height() / 2.0), Qt::SizeAllCursor, "inside the crop box: move");
		expectCursor(c, QPointF(c.width() - 2, c.height() / 2.0), Qt::SizeHorCursor, "the crop's right edge: <->");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
