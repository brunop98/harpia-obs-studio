#include "LevelMeter.hpp"

#include <QLinearGradient>
#include <QPainter>

#include <algorithm>

namespace harpia {

LevelMeter::LevelMeter(QWidget *parent) : QWidget(parent)
{
	setToolTip(QStringLiteral("Microphone input level"));
}

QSize LevelMeter::sizeHint() const
{
	return QSize(160, 14);
}

QSize LevelMeter::minimumSizeHint() const
{
	return QSize(60, 10);
}

void LevelMeter::setLevel(qreal rms, qreal peak)
{
	rms_ = std::clamp<qreal>(rms, 0.0, 1.0);
	// Peak-hold: jump up instantly, decay slowly so brief peaks stay visible.
	const qreal p = std::clamp<qreal>(peak, 0.0, 1.0);
	peakHold_ = std::max(p, peakHold_ * 0.92);
	update();
}

void LevelMeter::reset()
{
	rms_ = 0.0;
	peakHold_ = 0.0;
	update();
}

void LevelMeter::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	const QRectF r = rect().adjusted(0.5, 0.5, -0.5, -0.5);
	const qreal radius = std::min<qreal>(4.0, r.height() / 2.0);

	// Track.
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x25, 0x28, 0x2d));
	p.drawRoundedRect(r, radius, radius);

	// Filled portion, green→amber→red across the full width so louder = hotter.
	if (rms_ > 0.0) {
		QRectF fill = r;
		fill.setWidth(r.width() * rms_);
		QLinearGradient g(r.left(), 0, r.right(), 0);
		g.setColorAt(0.0, QColor(0x3f, 0xb9, 0x50));  // green
		g.setColorAt(0.7, QColor(0xd2, 0x99, 0x22));  // amber
		g.setColorAt(0.9, QColor(0xe5, 0x48, 0x4d));  // red
		p.setBrush(g);
		p.drawRoundedRect(fill, radius, radius);
	}

	// Peak-hold marker.
	if (peakHold_ > 0.01) {
		const qreal x = r.left() + r.width() * peakHold_;
		p.setPen(QPen(QColor(0xf2, 0xf3, 0xf5), 1.5));
		p.drawLine(QPointF(x, r.top() + 1), QPointF(x, r.bottom() - 1));
	}
}

} // namespace harpia
