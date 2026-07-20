#include "EditorWidgets.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace harpia {

// ============================ PreviewCanvas ============================

namespace {
constexpr int kHandle = 8;
}

PreviewCanvas::PreviewCanvas(QWidget *parent) : QWidget(parent)
{
	setMinimumSize(480, 270);
	setMouseTracking(true);
	setStyleSheet(QStringLiteral("background:#0d0e11;"));
}

void PreviewCanvas::setVideoSize(int w, int h)
{
	vw_ = w;
	vh_ = h;
	if (cropVideo_.isNull())
		cropVideo_ = QRect(0, 0, w, h);
	update();
}

void PreviewCanvas::setFrame(const QImage &img)
{
	frame_ = img;
	update();
}

void PreviewCanvas::setCropEnabled(bool on)
{
	cropEnabled_ = on;
	if (on && (cropVideo_.isNull() || cropVideo_.width() < 2))
		cropVideo_ = QRect(0, 0, vw_, vh_);
	update();
}

void PreviewCanvas::resetCrop()
{
	cropVideo_ = QRect(0, 0, vw_, vh_);
	emit cropChanged(cropVideo_);
	update();
}

QRect PreviewCanvas::displayRect() const
{
	if (vw_ <= 0 || vh_ <= 0)
		return rect();
	const double s = std::min(double(width()) / vw_, double(height()) / vh_);
	const int dw = std::max(1, int(vw_ * s));
	const int dh = std::max(1, int(vh_ * s));
	return QRect((width() - dw) / 2, (height() - dh) / 2, dw, dh);
}

QRect PreviewCanvas::videoToWidget(const QRect &r) const
{
	const QRect d = displayRect();
	if (vw_ <= 0 || vh_ <= 0)
		return d;
	const double sx = double(d.width()) / vw_;
	const double sy = double(d.height()) / vh_;
	return QRect(d.x() + int(r.x() * sx), d.y() + int(r.y() * sy), int(r.width() * sx),
		     int(r.height() * sy));
}

QRect PreviewCanvas::widgetCropRect() const
{
	return videoToWidget(cropVideo_);
}

void PreviewCanvas::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.fillRect(rect(), QColor(0x0d, 0x0e, 0x11));
	const QRect d = displayRect();
	if (!frame_.isNull())
		p.drawImage(d, frame_);

	if (!cropEnabled_)
		return;

	const QRect c = widgetCropRect();
	// Dim everything outside the crop.
	QRegion outside(d);
	outside -= QRegion(c);
	p.setClipRegion(outside);
	p.fillRect(d, QColor(0, 0, 0, 130));
	p.setClipping(false);

	p.setPen(QPen(QColor(0x00, 0xae, 0xef), 2));
	p.setBrush(Qt::NoBrush);
	p.drawRect(c);
	p.setBrush(QColor(0x00, 0xae, 0xef));
	p.setPen(Qt::NoPen);
	const QPoint pts[8] = {c.topLeft(),
			       {c.center().x(), c.top()},
			       c.topRight(),
			       {c.left(), c.center().y()},
			       {c.right(), c.center().y()},
			       c.bottomLeft(),
			       {c.center().x(), c.bottom()},
			       c.bottomRight()};
	for (const QPoint &h : pts)
		p.drawRect(QRect(h.x() - kHandle / 2, h.y() - kHandle / 2, kHandle, kHandle));
}

PreviewCanvas::Zone PreviewCanvas::zoneAt(const QPoint &pos) const
{
	const QRect c = widgetCropRect();
	const int m = kHandle;
	const bool nl = std::abs(pos.x() - c.left()) <= m;
	const bool nr = std::abs(pos.x() - c.right()) <= m;
	const bool nt = std::abs(pos.y() - c.top()) <= m;
	const bool nb = std::abs(pos.y() - c.bottom()) <= m;
	if (nt && nl)
		return Zone::TL;
	if (nt && nr)
		return Zone::TR;
	if (nb && nl)
		return Zone::BL;
	if (nb && nr)
		return Zone::BR;
	if (nl)
		return Zone::L;
	if (nr)
		return Zone::R;
	if (nt)
		return Zone::T;
	if (nb)
		return Zone::B;
	if (c.contains(pos))
		return Zone::Move;
	return Zone::None;
}

void PreviewCanvas::mousePressEvent(QMouseEvent *e)
{
	if (!cropEnabled_ || e->button() != Qt::LeftButton)
		return;
	drag_ = zoneAt(e->pos());
	dragStart_ = e->pos();
	dragStartCrop_ = widgetCropRect();
}

void PreviewCanvas::mouseMoveEvent(QMouseEvent *e)
{
	if (drag_ == Zone::None || !(e->buttons() & Qt::LeftButton))
		return;
	const QPoint delta = e->pos() - dragStart_;
	QRect g = dragStartCrop_;
	switch (drag_) {
	case Zone::Move:
		g.translate(delta);
		break;
	case Zone::L:
		g.setLeft(g.left() + delta.x());
		break;
	case Zone::R:
		g.setRight(g.right() + delta.x());
		break;
	case Zone::T:
		g.setTop(g.top() + delta.y());
		break;
	case Zone::B:
		g.setBottom(g.bottom() + delta.y());
		break;
	case Zone::TL:
		g.setTopLeft(g.topLeft() + delta);
		break;
	case Zone::TR:
		g.setTopRight(g.topRight() + delta);
		break;
	case Zone::BL:
		g.setBottomLeft(g.bottomLeft() + delta);
		break;
	case Zone::BR:
		g.setBottomRight(g.bottomRight() + delta);
		break;
	default:
		break;
	}
	applyWidgetCrop(g.normalized());
}

void PreviewCanvas::mouseReleaseEvent(QMouseEvent *)
{
	drag_ = Zone::None;
}

void PreviewCanvas::applyWidgetCrop(const QRect &widgetRect)
{
	const QRect d = displayRect();
	if (vw_ <= 0 || vh_ <= 0 || d.width() <= 0)
		return;
	// Clamp to the displayed frame, then map widget px -> source px.
	QRect w = widgetRect.intersected(d);
	if (w.width() < 8 || w.height() < 8)
		return;
	const double sx = double(vw_) / d.width();
	const double sy = double(vh_) / d.height();
	QRect v(int((w.x() - d.x()) * sx), int((w.y() - d.y()) * sy), int(w.width() * sx),
		int(w.height() * sy));
	v = v.intersected(QRect(0, 0, vw_, vh_));
	cropVideo_ = v;
	emit cropChanged(cropVideo_);
	update();
}

// ============================== Timeline ==============================

namespace {
constexpr int kPad = 12;      // left/right margin
constexpr int kBarTop = 14;   // bar y
constexpr int kBarH = 36;     // bar height (tall enough for the filmstrip)
constexpr int kHandleW = 8;   // handle grab width
constexpr double kMaxZoom = 32.0;
} // namespace

Timeline::Timeline(QWidget *parent) : QWidget(parent)
{
	setMinimumHeight(kBarTop + kBarH + 22);
	setMouseTracking(true);
	setToolTip(QStringLiteral("Ctrl+scroll to zoom, scroll to pan"));
}

void Timeline::setDuration(qint64 ms)
{
	duration_ = std::max<qint64>(1, ms);
	start_ = 0;
	end_ = duration_;
	playhead_ = 0;
	zoom_ = 1.0;
	viewStart_ = 0;
	update();
}

void Timeline::setThumbs(const QVector<QImage> &thumbs)
{
	thumbs_ = thumbs;
	update();
}

qint64 Timeline::visibleMs() const
{
	return std::max<qint64>(1, qint64(duration_ / zoom_));
}

void Timeline::clampView()
{
	viewStart_ = std::clamp<qint64>(viewStart_, 0, duration_ - visibleMs());
}

void Timeline::wheelEvent(QWheelEvent *e)
{
	if (duration_ <= 0)
		return;
	const int delta = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
	if (delta == 0)
		return;
	const double steps = delta / 120.0;
	if (e->modifiers() & Qt::ControlModifier) {
		// Zoom around the time under the cursor.
		const int x = int(e->position().x());
		const qint64 anchor = xToMs(x);
		const double frac =
			std::clamp(double(x - kPad) / std::max(1, width() - 2 * kPad), 0.0, 1.0);
		zoom_ = std::clamp(zoom_ * std::pow(1.3, steps), 1.0, kMaxZoom);
		viewStart_ = anchor - qint64(frac * visibleMs());
	} else {
		// Plain scroll pans the zoomed view.
		viewStart_ -= qint64(steps * visibleMs() * 0.15);
	}
	clampView();
	update();
	e->accept();
}

void Timeline::setStart(qint64 ms)
{
	start_ = std::clamp<qint64>(ms, 0, end_ - 1);
	update();
}

void Timeline::setEnd(qint64 ms)
{
	end_ = std::clamp<qint64>(ms, start_ + 1, duration_);
	update();
}

void Timeline::setPlayhead(qint64 ms)
{
	playhead_ = std::clamp<qint64>(ms, 0, duration_);
	update();
}

int Timeline::msToX(qint64 ms) const
{
	const int w = width() - 2 * kPad;
	return kPad + int(double(ms - viewStart_) / visibleMs() * w);
}

qint64 Timeline::xToMs(int x) const
{
	const int w = std::max(1, width() - 2 * kPad);
	return std::clamp<qint64>(viewStart_ + qint64(double(x - kPad) / w * visibleMs()), 0,
				  duration_);
}

void Timeline::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	const QRect bar(kPad, kBarTop, width() - 2 * kPad, kBarH);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x2b, 0x2d, 0x31));
	p.drawRoundedRect(bar, 4, 4);

	// Filmstrip + overlays live inside the rounded bar.
	{
		QPainterPath clip;
		clip.addRoundedRect(bar, 4, 4);
		p.save();
		p.setClipPath(clip);

		if (!thumbs_.isEmpty()) {
			const int n = thumbs_.size();
			const double sliceMs = double(duration_) / n;
			const qint64 viewEnd = viewStart_ + visibleMs();
			int i0 = std::clamp(int(viewStart_ / sliceMs), 0, n - 1);
			int i1 = std::clamp(int(viewEnd / sliceMs) + 1, i0 + 1, n);
			for (int i = i0; i < i1; ++i) {
				if (thumbs_[i].isNull())
					continue;
				const int x1 = msToX(qint64(i * sliceMs));
				const int x2 = msToX(qint64((i + 1) * sliceMs));
				if (x2 > x1)
					p.drawImage(QRect(x1, bar.top(), x2 - x1, bar.height()),
						    thumbs_[i]);
			}
		}

		// Selected [start,end] span (translucent, over the filmstrip) and the
		// dimmed outside regions so the kept part reads instantly.
		const int xs = msToX(start_), xe = msToX(end_);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 110));
		p.drawRect(QRect(bar.left(), bar.top(), xs - bar.left(), bar.height()));
		p.drawRect(QRect(xe, bar.top(), bar.right() + 1 - xe, bar.height()));
		p.setBrush(QColor(0x00, 0xae, 0xef, 55));
		p.drawRect(QRect(xs, bar.top(), xe - xs, bar.height()));
		p.restore();
	}
	const int xs = msToX(start_), xe = msToX(end_);

	// Zoom indicator: which part of the clip is visible.
	if (zoom_ > 1.001) {
		const int y = bar.bottom() + 3;
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0x2b, 0x2d, 0x31));
		p.drawRect(QRect(bar.left(), y, bar.width(), 2));
		p.setBrush(QColor(0x9a, 0x9f, 0xa8));
		const int ix = bar.left() + int(double(viewStart_) / duration_ * bar.width());
		const int iw = std::max(8, int(double(visibleMs()) / duration_ * bar.width()));
		p.drawRect(QRect(ix, y, iw, 2));
	}

	// Handles.
	auto handle = [&](int x, const QColor &col) {
		p.setBrush(col);
		p.drawRoundedRect(QRect(x - kHandleW / 2, bar.top() - 4, kHandleW, bar.height() + 8), 2, 2);
	};
	handle(xs, QColor(0x3f, 0xb9, 0x50));
	handle(xe, QColor(0xe5, 0x48, 0x4d));

	// Playhead.
	const int xp = msToX(playhead_);
	p.setPen(QPen(QColor(0xff, 0xff, 0xff), 1));
	p.drawLine(xp, bar.top() - 6, xp, bar.bottom() + 6);

	// Times.
	p.setPen(QColor(0x9a, 0x9f, 0xa8));
	auto t = [](qint64 ms) {
		return QStringLiteral("%1:%2.%3")
			.arg(ms / 60000, 2, 10, QLatin1Char('0'))
			.arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
			.arg((ms % 1000) / 100);
	};
	p.drawText(rect().adjusted(kPad, bar.bottom() + 6, -kPad, 0), Qt::AlignLeft,
		   QStringLiteral("Start %1").arg(t(start_)));
	p.drawText(rect().adjusted(kPad, bar.bottom() + 6, -kPad, 0), Qt::AlignRight,
		   QStringLiteral("End %1").arg(t(end_)));
}

void Timeline::mousePressEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;
	const int x = e->pos().x();
	const int xs = msToX(start_), xe = msToX(end_);
	if (std::abs(x - xs) <= kHandleW)
		grab_ = Grab::Start;
	else if (std::abs(x - xe) <= kHandleW)
		grab_ = Grab::End;
	else
		grab_ = Grab::Playhead;
	mouseMoveEvent(e);
}

void Timeline::mouseMoveEvent(QMouseEvent *e)
{
	if (grab_ == Grab::None || !(e->buttons() & Qt::LeftButton))
		return;
	const qint64 ms = xToMs(e->pos().x());
	if (grab_ == Grab::Start) {
		start_ = std::clamp<qint64>(ms, 0, end_ - 1);
		playhead_ = start_;
		emit startChanged(start_);
		emit scrub(start_);
	} else if (grab_ == Grab::End) {
		end_ = std::clamp<qint64>(ms, start_ + 1, duration_);
		playhead_ = end_;
		emit endChanged(end_);
		emit scrub(end_);
	} else {
		playhead_ = ms;
		emit scrub(ms);
	}
	update();
}

void Timeline::mouseReleaseEvent(QMouseEvent *)
{
	grab_ = Grab::None;
}

} // namespace harpia
