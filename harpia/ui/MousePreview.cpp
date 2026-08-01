#include "MousePreview.hpp"

#include <QPainter>
#include <QPolygon>
#include <QTimer>

#include <algorithm>

namespace harpia {

namespace {
constexpr int kLoop = 60; // frames per ripple loop
}

MousePreview::MousePreview(QWidget *parent) : QWidget(parent)
{
	setMinimumSize(240, 120);
	setStyleSheet(QStringLiteral("background:#15161a; border:1px solid #303338; border-radius:6px;"));
	timer_ = new QTimer(this);
	timer_->setInterval(33);
	connect(timer_, &QTimer::timeout, this, [this]() {
		phase_ = (phase_ + 1) % kLoop;
		if (phase_ == 0)
			rippleIndex_ = (rippleIndex_ + 1) % 2; // next loop: other button's color
		update();
	});
	// Not started here: syncTimer() runs the animation only while the widget
	// is actually on screen with an effect to preview. It used to animate at
	// 30 Hz for the whole life of the dialog -- other tabs, effects off,
	// dialog covered -- which is a lot of repaints of a picture nobody sees.
}

void MousePreview::syncTimer()
{
	const bool want = isVisible() && (showArea_ || showClicks_);
	if (want == timer_->isActive())
		return;
	if (want)
		timer_->start();
	else
		timer_->stop();
}

void MousePreview::showEvent(QShowEvent *e)
{
	QWidget::showEvent(e);
	syncTimer();
}

void MousePreview::hideEvent(QHideEvent *e)
{
	QWidget::hideEvent(e);
	syncTimer();
}

void MousePreview::configure(bool showArea, const QColor &areaColor, int areaSize, bool showClicks,
			     const QColor &leftClickColor, const QColor &rightClickColor)
{
	showArea_ = showArea;
	areaColor_ = areaColor;
	areaSize_ = areaSize;
	showClicks_ = showClicks;
	leftClickColor_ = leftClickColor;
	rightClickColor_ = rightClickColor;
	syncTimer();
	update();
}

void MousePreview::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	const QPoint c = rect().center();

	// Click ripple (looping, alternating left/right click colors).
	if (showClicks_) {
		const double t = double(phase_) / kLoop;
		const int radius = int(8 + t * 34);
		QColor col = (rippleIndex_ % 2 == 0) ? leftClickColor_ : rightClickColor_;
		col.setAlpha(int(200 * (1.0 - t)));
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(col, 3));
		p.drawEllipse(c, radius, radius);
	}

	// Cursor highlight.
	if (showArea_) {
		QColor fill = areaColor_;
		fill.setAlpha(90);
		const int r = std::min(areaSize_ / 2, 46);
		p.setPen(QPen(areaColor_, 2));
		p.setBrush(fill);
		p.drawEllipse(c, r, r);
	}

	// A simple cursor arrow at the center.
	QPolygon cursor;
	cursor << c << (c + QPoint(0, 18)) << (c + QPoint(5, 13)) << (c + QPoint(9, 20))
	       << (c + QPoint(12, 18)) << (c + QPoint(8, 11)) << (c + QPoint(14, 11));
	p.setPen(QPen(Qt::black, 1));
	p.setBrush(Qt::white);
	p.drawPolygon(cursor);
}

} // namespace harpia
