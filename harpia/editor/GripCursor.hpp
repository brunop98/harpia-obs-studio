#pragma once

// Which cursor a grip on the preview wants, so the pointer says what a press
// will do before it is pressed. The grips are on a box that may be turned, so
// "the right edge" is not necessarily a horizontal resize: the cursor follows
// the box's own axes, snapped to the four shapes Qt has.
//
// Pure functions over angles and grip indexes; the canvas maps its zones onto
// them and a test checks the arithmetic directly.

#include <QCursor>
#include <QPainter>
#include <QPixmap>

#include <cmath>

namespace harpia {

// Grips in the order the canvas keeps them: TL, T, TR, L, R, BL, B, BR.
enum class GripIndex { TL = 0, T, TR, L, R, BL, B, BR };

// The screen-space axis (degrees, 0 = horizontal, 90 = vertical, y down) a
// grip resizes along, on a box turned by `rotationDeg`. A side grip's axis is
// the box's own; a corner's is the diagonal through it.
inline double gripAxisDeg(GripIndex g, double rotationDeg)
{
	switch (g) {
	case GripIndex::L:
	case GripIndex::R: return rotationDeg;
	case GripIndex::T:
	case GripIndex::B: return rotationDeg + 90.0;
	case GripIndex::TL:
	case GripIndex::BR: return rotationDeg + 45.0; // the "\" diagonal, y down
	case GripIndex::TR:
	case GripIndex::BL: return rotationDeg - 45.0; // the "/" diagonal
	}
	return rotationDeg;
}

// The nearest of Qt's four resize cursors to an axis. Horizontal within
// 22.5 degrees either way, then the diagonals, then vertical.
inline Qt::CursorShape resizeCursorForAxis(double axisDeg)
{
	double a = std::fmod(axisDeg, 180.0);
	if (a < 0)
		a += 180.0;
	if (a < 22.5 || a >= 157.5)
		return Qt::SizeHorCursor;
	if (a < 67.5)
		return Qt::SizeFDiagCursor; // "\"
	if (a < 112.5)
		return Qt::SizeVerCursor;
	return Qt::SizeBDiagCursor; // "/"
}

// Qt has no rotate cursor. A circular arrow drawn once: white on a dark
// outline so it reads on any picture, with the hotspot in its middle.
inline QCursor rotateCursor()
{
	static const QCursor cur = [] {
		const int s = 24;
		QPixmap pm(s, s);
		pm.fill(Qt::transparent);
		QPainter p(&pm);
		p.setRenderHint(QPainter::Antialiasing);
		const QRectF arc(4.5, 4.5, 15, 15);
		// The arc, three quarters of a turn, and an arrowhead at its end.
		auto draw = [&](const QPen &pen) {
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			p.drawArc(arc, 30 * 16, 270 * 16);
			// The arc ends at 300 degrees (Qt angles go anticlockwise from
			// 3 o'clock); the head points along the arc's direction there.
			const double end = 300.0 * M_PI / 180.0;
			const QPointF tip(arc.center().x() + 7.5 * std::cos(end),
					  arc.center().y() - 7.5 * std::sin(end));
			p.setBrush(pen.color());
			QPolygonF head;
			head << tip + QPointF(-4.5, -1.5) << tip + QPointF(1.5, -4.5) << tip + QPointF(2.5, 2.0);
			p.drawPolygon(head);
		};
		draw(QPen(QColor(0, 0, 0, 200), 4.0, Qt::SolidLine, Qt::RoundCap));
		draw(QPen(QColor(255, 255, 255), 2.0, Qt::SolidLine, Qt::RoundCap));
		p.end();
		return QCursor(pm, s / 2, s / 2);
	}();
	return cur;
}

} // namespace harpia
