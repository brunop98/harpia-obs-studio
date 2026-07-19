#include "ScreenBorderOverlay.hpp"

#include <QGuiApplication>
#include <QPainter>
#include <QRegion>
#include <QScreen>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011 // Windows 10 2004+
#endif
#endif

namespace harpia {

ScreenBorderOverlay::ScreenBorderOverlay(QWidget *parent) : QWidget(parent)
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
		       Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setAttribute(Qt::WA_ShowWithoutActivating);
}

void ScreenBorderOverlay::showBorder(QScreen *screen, const QColor &color, int thickness)
{
	color_ = color;
	thickness_ = qBound(1, thickness, 10);
	applyGeometryAndMask(screen ? screen : QGuiApplication::primaryScreen());
	show();
	raise();
	excludeFromCapture();
	update();
}

void ScreenBorderOverlay::setColor(const QColor &color)
{
	color_ = color;
	update();
}

void ScreenBorderOverlay::hideBorder()
{
	hide();
}

void ScreenBorderOverlay::applyGeometryAndMask(QScreen *screen)
{
	if (!screen)
		return;
	setGeometry(screen->geometry());

	// Mask to just the border ring so the interior is fully click-through and
	// nothing is drawn over the desktop.
	const QRect r(QPoint(0, 0), screen->geometry().size());
	QRegion ring(r);
	ring -= r.adjusted(thickness_, thickness_, -thickness_, -thickness_);
	setMask(ring);
}

void ScreenBorderOverlay::excludeFromCapture()
{
#if defined(_WIN32)
	// Visible on screen, but omitted from screen capture (so it never shows up
	// in the recording). Requires Windows 10 version 2004 or newer.
	SetWindowDisplayAffinity((HWND)winId(), WDA_EXCLUDEFROMCAPTURE);
#endif
}

void ScreenBorderOverlay::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setPen(Qt::NoPen);
	p.setBrush(color_);

	const QRect r = rect();
	const int t = thickness_;
	p.drawRect(0, 0, r.width(), t);                         // top
	p.drawRect(0, r.height() - t, r.width(), t);            // bottom
	p.drawRect(0, 0, t, r.height());                        // left
	p.drawRect(r.width() - t, 0, t, r.height());            // right
}

} // namespace harpia
