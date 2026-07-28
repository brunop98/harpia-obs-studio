#include "UiIcons.hpp"

#include <QApplication>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPolygonF>

namespace harpia {

namespace {

// Every shape is drawn inside a 0..1 box and scaled to fit, so one description
// serves every size. `u` maps a unit coordinate onto the target rectangle.
struct Unit {
	QRectF box;
	QPointF operator()(double x, double y) const
	{
		return QPointF(box.x() + x * box.width(), box.y() + y * box.height());
	}
	double w(double f) const { return f * box.width(); }
	double h(double f) const { return f * box.height(); }
};

void strokeArrow(QPainter &p, const Unit &u, bool up)
{
	// A shaft with a chevron head — reads better at 14 px than a filled
	// triangle, which loses its point.
	const double tipY = up ? 0.18 : 0.82, tailY = up ? 0.86 : 0.14;
	const double barbY = up ? 0.45 : 0.55;
	p.drawLine(u(0.5, tailY), u(0.5, tipY));
	p.drawPolyline(QPolygonF{u(0.22, barbY), u(0.5, tipY), u(0.78, barbY)});
}

void strokeChevron(QPainter &p, const Unit &u, bool down)
{
	if (down)
		p.drawPolyline(QPolygonF{u(0.22, 0.38), u(0.5, 0.66), u(0.78, 0.38)});
	else
		p.drawPolyline(QPolygonF{u(0.38, 0.22), u(0.66, 0.5), u(0.38, 0.78)});
}

void drawShape(QPainter &p, Glyph g, const QRectF &box, const QColor &c)
{
	const Unit u{box};
	// The stroke scales with the icon so a 32 px icon is not a hairline.
	const double sw = std::max(1.2, box.width() * 0.11);
	QPen pen(c, sw, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.setRenderHint(QPainter::Antialiasing, true);

	switch (g) {
	case Glyph::ArrowUp: strokeArrow(p, u, true); break;
	case Glyph::ArrowDown: strokeArrow(p, u, false); break;

	case Glyph::Cross:
		p.drawLine(u(0.24, 0.24), u(0.76, 0.76));
		p.drawLine(u(0.76, 0.24), u(0.24, 0.76));
		break;

	case Glyph::Play:
		p.setPen(Qt::NoPen);
		p.setBrush(c);
		p.drawPolygon(QPolygonF{u(0.28, 0.18), u(0.82, 0.5), u(0.28, 0.82)});
		break;

	case Glyph::Pause:
		p.setPen(Qt::NoPen);
		p.setBrush(c);
		p.drawRoundedRect(QRectF(u(0.28, 0.2), QSizeF(u.w(0.14), u.h(0.6))), u.w(0.04),
				  u.w(0.04));
		p.drawRoundedRect(QRectF(u(0.58, 0.2), QSizeF(u.w(0.14), u.h(0.6))), u.w(0.04),
				  u.w(0.04));
		break;

	case Glyph::Stop:
		p.setPen(Qt::NoPen);
		p.setBrush(c);
		p.drawRoundedRect(QRectF(u(0.24, 0.24), QSizeF(u.w(0.52), u.h(0.52))), u.w(0.08),
				  u.w(0.08));
		break;

	case Glyph::Record:
		p.setPen(Qt::NoPen);
		p.setBrush(c);
		p.drawEllipse(QRectF(u(0.22, 0.22), QSizeF(u.w(0.56), u.h(0.56))));
		break;

	case Glyph::SkipStart:
		p.drawLine(u(0.26, 0.2), u(0.26, 0.8));
		p.setPen(Qt::NoPen);
		p.setBrush(c);
		p.drawPolygon(QPolygonF{u(0.8, 0.2), u(0.8, 0.8), u(0.38, 0.5)});
		break;

	case Glyph::Undo:
	case Glyph::Redo: {
		// A curved shaft with a FILLED head. Two earlier attempts used a
		// stroked chevron for the head and a wide arc for the shaft; at 16 px
		// both came out as a bare bump with no sense of direction. A solid
		// triangle is the part that survives being small.
		const bool back = (g == Glyph::Undo);
		// Mirror horizontally for redo, so one description serves both.
		const auto m = [&](double x, double y) { return u(back ? x : 1.0 - x, y); };
		QPainterPath shaft;
		shaft.moveTo(m(0.80, 0.80));
		shaft.cubicTo(m(0.84, 0.44), m(0.66, 0.30), m(0.40, 0.30));
		p.setBrush(Qt::NoBrush);
		p.drawPath(shaft);
		p.setPen(Qt::NoPen);
		p.setBrush(c);
		p.drawPolygon(QPolygonF{m(0.14, 0.30), m(0.44, 0.13), m(0.44, 0.47)});
		break;
	}

	case Glyph::ChevronDown: strokeChevron(p, u, true); break;
	case Glyph::ChevronRight: strokeChevron(p, u, false); break;

	case Glyph::StepBack:
		p.drawPolyline(QPolygonF{u(0.62, 0.22), u(0.34, 0.5), u(0.62, 0.78)});
		break;
	case Glyph::StepForward:
		p.drawPolyline(QPolygonF{u(0.38, 0.22), u(0.66, 0.5), u(0.38, 0.78)});
		break;

	case Glyph::Diamond:
		p.setPen(Qt::NoPen);
		p.setBrush(c);
		p.drawPolygon(QPolygonF{u(0.5, 0.16), u(0.84, 0.5), u(0.5, 0.84), u(0.16, 0.5)});
		break;

	case Glyph::Magnet: {
		// A horseshoe: an open arc with two legs. Unmistakable at this size
		// even without the usual red/grey banding.
		QRectF arc(u(0.18, 0.16), QSizeF(u.w(0.64), u.h(0.64)));
		p.drawArc(arc, 0, 180 * 16);
		p.drawLine(u(0.18, 0.48), u(0.18, 0.8));
		p.drawLine(u(0.82, 0.48), u(0.82, 0.8));
		break;
	}

	case Glyph::Speaker:
	case Glyph::SpeakerMuted: {
		p.setPen(Qt::NoPen);
		p.setBrush(c);
		p.drawPolygon(QPolygonF{u(0.14, 0.36), u(0.3, 0.36), u(0.5, 0.18), u(0.5, 0.82),
					u(0.3, 0.64), u(0.14, 0.64)});
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		if (g == Glyph::Speaker) {
			p.drawArc(QRectF(u(0.44, 0.28), QSizeF(u.w(0.3), u.h(0.44))), -70 * 16,
				  140 * 16);
			p.drawArc(QRectF(u(0.44, 0.16), QSizeF(u.w(0.5), u.h(0.68))), -70 * 16,
				  140 * 16);
		} else {
			p.drawLine(u(0.62, 0.36), u(0.9, 0.64));
			p.drawLine(u(0.9, 0.36), u(0.62, 0.64));
		}
		break;
	}

	case Glyph::Keyboard: {
		QPen thin(c, std::max(1.0, sw * 0.75), Qt::SolidLine, Qt::RoundCap);
		p.setPen(thin);
		p.drawRoundedRect(QRectF(u(0.1, 0.28), QSizeF(u.w(0.8), u.h(0.44))), u.w(0.08),
				  u.w(0.08));
		for (double x : {0.26, 0.42, 0.58, 0.74})
			p.drawPoint(u(x, 0.42));
		p.drawLine(u(0.34, 0.58), u(0.66, 0.58)); // the space bar
		break;
	}

	case Glyph::Warning: {
		QPainterPath tri;
		tri.moveTo(u(0.5, 0.14));
		tri.lineTo(u(0.92, 0.84));
		tri.lineTo(u(0.08, 0.84));
		tri.closeSubpath();
		p.drawPath(tri);
		p.drawLine(u(0.5, 0.42), u(0.5, 0.62));
		p.setPen(QPen(c, sw, Qt::SolidLine, Qt::RoundCap));
		p.drawPoint(u(0.5, 0.73));
		break;
	}

	case Glyph::Sparkle: {
		// A four-pointed star with concave sides.
		QPainterPath s;
		s.moveTo(u(0.5, 0.1));
		s.quadTo(u(0.56, 0.44), u(0.9, 0.5));
		s.quadTo(u(0.56, 0.56), u(0.5, 0.9));
		s.quadTo(u(0.44, 0.56), u(0.1, 0.5));
		s.quadTo(u(0.44, 0.44), u(0.5, 0.1));
		p.setPen(Qt::NoPen);
		p.setBrush(c);
		p.drawPath(s);
		break;
	}
	}
}

struct Key {
	int glyph, px;
	QRgb rgb;
	bool operator==(const Key &o) const
	{
		return glyph == o.glyph && px == o.px && rgb == o.rgb;
	}
};
size_t qHash(const Key &k, size_t seed = 0)
{
	return qHashMulti(seed, k.glyph, k.px, k.rgb);
}

} // namespace

void paintGlyph(QPainter &p, Glyph g, const QRectF &box, const QColor &colour)
{
	p.save();
	drawShape(p, g, box, colour);
	p.restore();
}

QIcon uiIcon(Glyph g, int px, const QColor &colour)
{
	// Default to the palette's text colour so an icon suits whatever theme is
	// in force, the way a text button's glyph used to.
	const QColor c = colour.isValid()
				 ? colour
				 : (qApp ? qApp->palette().color(QPalette::ButtonText)
					 : QColor(0xe8, 0xea, 0xed));
	px = std::max(8, px);

	static QHash<Key, QIcon> cache;
	const Key key{int(g), px, c.rgba()};
	if (const auto it = cache.constFind(key); it != cache.constEnd())
		return it.value();

	// Rendered at the device pixel ratio, so it is crisp on a HiDPI screen
	// rather than a scaled-up 16 px bitmap.
	const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
	QPixmap pm(QSize(px, px) * dpr);
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	{
		QPainter p(&pm);
		drawShape(p, g, QRectF(0, 0, px, px), c);
	}
	const QIcon icon(pm);
	cache.insert(key, icon);
	return icon;
}

} // namespace harpia
