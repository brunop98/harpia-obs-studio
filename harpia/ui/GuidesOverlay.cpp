#include "GuidesOverlay.hpp"

#include <QImage>
#include <QLinearGradient>
#include <QOperatingSystemVersion>
#include <QPainter>

#include <cmath>

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

void paintGuides(QPainter &p, const QRectF &frame, const GuideStyle &style, double pxPerUnit)
{
	const double unit = pxPerUnit > 0.01 ? pxPerUnit : 1.0;
	// Thickness is in screen pixels at 100%; on a scaled display it grows with
	// everything else, rounded to whole device pixels so it stays sharp.
	const int devW = std::max(1, int(std::lround(std::clamp(style.thickness, 1, 5) * unit)));
	// Straight lines centred on a pixel for an odd width, on a pixel edge for
	// an even one -- in device pixels, which is what the snapping is about.
	const double crisp = (devW % 2) ? 0.5 : 0.0;
	const QRectF devFrame(frame.topLeft() * unit, frame.size() * unit);
	const GuidePaths dev = guidePaths(style.kinds, devFrame, crisp);
	if (dev.solid.isEmpty() && dev.dashed.isEmpty())
		return;

	const double alpha = std::clamp(style.opacityPct, 10, 100) / 100.0;
	QColor line = style.color.isValid() ? style.color : QColor(255, 255, 255);
	line.setAlphaF(float(alpha));
	QColor edge = line.lightnessF() > 0.5 ? QColor(0, 0, 0) : QColor(255, 255, 255);
	edge.setAlphaF(float(alpha * 0.35));

	p.save();
	p.scale(1.0 / unit, 1.0 / unit);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setBrush(Qt::NoBrush);
	// The edge first, a pixel wider each side; then the line itself replaces
	// whatever is under it, so the edge never darkens the colour you picked.
	QPen pen(edge, devW + 2, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
	p.setCompositionMode(QPainter::CompositionMode_SourceOver);
	p.strokePath(dev.solid, pen);
	pen.setStyle(Qt::DashLine);
	p.strokePath(dev.dashed, pen);
	p.setCompositionMode(QPainter::CompositionMode_Source);
	pen = QPen(line, devW, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
	p.strokePath(dev.solid, pen);
	pen.setStyle(Qt::DashLine);
	p.strokePath(dev.dashed, pen);
	p.restore();
}

GuidesOverlay::GuidesOverlay(QWidget *parent) : QWidget(parent)
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
		       Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setAttribute(Qt::WA_ShowWithoutActivating);
	setAttribute(Qt::WA_NoSystemBackground);
	hidden_ = systemCanHideFromCapture();
}

bool GuidesOverlay::systemCanHideFromCapture()
{
#if defined(_WIN32)
	// Before version 2004 (build 19041) the same flag means WDA_MONITOR, which
	// puts the window in the capture as a solid BLACK rectangle -- over the
	// whole picture, for a window this size. So the version decides, not the
	// call's return value.
	return QOperatingSystemVersion::current() >=
	       QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 19041);
#else
	return false;
#endif
}

void GuidesOverlay::showGuides(const QRect &rectLogical, const GuideStyle &style)
{
	if (rectLogical.width() < 8 || rectLogical.height() < 8) {
		hideGuides();
		return;
	}
	if (isVisible() && rectLogical == rect_ && style == style_)
		return;
	style_ = style;
	if (rectLogical != rect_ || !isVisible()) {
		rect_ = rectLogical;
		setGeometry(rectLogical);
	}
	if (!isVisible()) {
		// Kept out of the capture BEFORE it is first on screen: shown during a
		// recording (the shortcut, mid-take), even one frame of lines would
		// otherwise make it into the file. winId() creates the window without
		// showing it. Asked again once shown, which is the answer that counts.
		(void)winId();
		excludeFromCapture();
		show();
		raise();
		excludeFromCapture();
	}
	update();
}

void GuidesOverlay::hideGuides()
{
	if (isVisible())
		hide();
}

void GuidesOverlay::excludeFromCapture()
{
#if defined(_WIN32)
	if (!systemCanHideFromCapture()) {
		hidden_ = false;
		return;
	}
	hidden_ = SetWindowDisplayAffinity((HWND)winId(), WDA_EXCLUDEFROMCAPTURE) != FALSE;
#endif
}

void GuidesOverlay::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setCompositionMode(QPainter::CompositionMode_Source);
	p.fillRect(rect(), Qt::transparent);
	paintGuides(p, QRectF(rect()), style_, devicePixelRatioF());
}

GuidesPreview::GuidesPreview(QWidget *parent) : QWidget(parent)
{
	setFixedSize(sizeHint());
}

void GuidesPreview::setStyle(const GuideStyle &style)
{
	if (style == style_)
		return;
	style_ = style;
	update();
}

void GuidesPreview::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);
	const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
	// A stand-in for a game: dusk sky over dark ground, so both a light and a
	// dark line colour have something to show against.
	QLinearGradient sky(r.topLeft(), r.bottomLeft());
	sky.setColorAt(0.0, QColor(0x3b, 0x4a, 0x6b));
	sky.setColorAt(0.62, QColor(0x9a, 0x7a, 0x6a));
	sky.setColorAt(0.63, QColor(0x2a, 0x30, 0x2a));
	sky.setColorAt(1.0, QColor(0x1a, 0x1d, 0x1a));
	p.setPen(QPen(QColor(0x3a, 0x3f, 0x47), 1));
	p.setBrush(sky);
	p.drawRoundedRect(r, 4, 4);

	// The guides go on a layer of their own: paintGuides writes its pixels
	// outright, which would punch holes in the picture painted above.
	const qreal dpr = devicePixelRatioF();
	QImage layer(size() * dpr, QImage::Format_ARGB32_Premultiplied);
	layer.setDevicePixelRatio(dpr);
	layer.fill(Qt::transparent);
	{
		QPainter lp(&layer);
		paintGuides(lp, QRectF(rect()), style_, dpr);
	}
	p.drawImage(QPointF(0, 0), layer);
}

} // namespace harpia
