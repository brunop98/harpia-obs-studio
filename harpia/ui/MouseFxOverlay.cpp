#include "MouseFxOverlay.hpp"

#include <QCursor>
#include <QDateTime>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QTimer>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace harpia {

namespace {
constexpr int kRippleMs = 500; // click ripple lifetime
}

MouseFxOverlay::MouseFxOverlay(QWidget *parent) : QWidget(parent)
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
		       Qt::WindowTransparentForInput);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_TransparentForMouseEvents);

	timer_ = new QTimer(this);
	timer_->setInterval(16); // ~60 fps
	connect(timer_, &QTimer::timeout, this, &MouseFxOverlay::tick);
}

void MouseFxOverlay::setScreen(QScreen *screen)
{
	screen_ = screen;
}

void MouseFxOverlay::start()
{
	if (!screen_)
		screen_ = QGuiApplication::primaryScreen();
	if (screen_)
		setGeometry(screen_->geometry());
	ripples_.clear();
	lastLeft_ = lastRight_ = false;
	show();
	timer_->start();
}

void MouseFxOverlay::stop()
{
	timer_->stop();
	ripples_.clear();
	hide();
}

void MouseFxOverlay::tick()
{
	const QPoint prevCursor = cursorLocal_;
	cursorLocal_ = mapFromGlobal(QCursor::pos());

	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	bool ripplesChanged = false;

#if defined(_WIN32)
	if (cfg_.showClicks) {
		const bool left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
		const bool right = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
		if (left && !lastLeft_) {
			ripples_.push_back({QPointF(cursorLocal_), now, cfg_.leftColor});
			ripplesChanged = true;
		}
		if (right && !lastRight_) {
			ripples_.push_back({QPointF(cursorLocal_), now, cfg_.rightColor});
			ripplesChanged = true;
		}
		lastLeft_ = left;
		lastRight_ = right;
	}
#endif

	// Drop expired ripples, remembering their area so the last ring is erased.
	QRegion dirty;
	const int rippleR = 10 + 40 + 5; // max radius + pen width margin
	for (size_t i = 0; i < ripples_.size();) {
		if (now - ripples_[i].startMs > kRippleMs) {
			const QPoint c = ripples_[i].center.toPoint();
			dirty += QRect(c.x() - rippleR, c.y() - rippleR, 2 * rippleR, 2 * rippleR);
			ripples_.erase(ripples_.begin() + i);
			ripplesChanged = true;
		} else {
			++i;
		}
	}

	// Nothing visible changed? Skip the repaint entirely — this overlay runs at
	// 60 fps while recording, and a full-screen translucent repaint is the most
	// expensive no-op in the app.
	const bool cursorMoved = cursorLocal_ != prevCursor;
	if (!cursorMoved && ripples_.empty() && !ripplesChanged && dirty.isEmpty())
		return;

	// Repaint only what changed: old + new cursor circles and live ripple areas.
	if (cfg_.showArea) {
		const int r = cfg_.areaSize / 2 + 4;
		dirty += QRect(prevCursor.x() - r, prevCursor.y() - r, 2 * r, 2 * r);
		dirty += QRect(cursorLocal_.x() - r, cursorLocal_.y() - r, 2 * r, 2 * r);
	}
	for (const Ripple &rp : ripples_) {
		const QPoint c = rp.center.toPoint();
		dirty += QRect(c.x() - rippleR, c.y() - rippleR, 2 * rippleR, 2 * rippleR);
	}
	if (dirty.isEmpty())
		return;
	update(dirty);
}

void MouseFxOverlay::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	// Cursor area highlight.
	if (cfg_.showArea) {
		QColor fill = cfg_.areaColor;
		fill.setAlpha(90);
		const int r = cfg_.areaSize / 2;
		p.setPen(QPen(cfg_.areaColor, 2));
		p.setBrush(fill);
		p.drawEllipse(cursorLocal_, r, r);
	}

	// Click ripples: expanding, fading rings.
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	for (const Ripple &rp_ : ripples_) {
		const double t = double(now - rp_.startMs) / kRippleMs; // 0..1
		if (t < 0.0 || t > 1.0)
			continue;
		const int radius = int(10 + t * 40);
		QColor c = rp_.color;
		c.setAlpha(int(200 * (1.0 - t)));
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(c, 3));
		p.drawEllipse(rp_.center, radius, radius);
	}
}

} // namespace harpia
