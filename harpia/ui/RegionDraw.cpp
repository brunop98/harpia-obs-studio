#include "RegionDraw.hpp"

#include "NativeWindowOps.hpp"

#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace harpia {

namespace {
constexpr int kSnap = 12; // logical px, as RegionTool's handles snap
// Recording sizes worth landing on exactly (device px), each side on its own:
// the landscape ones the frame snaps to, and 720 / 1080 / 1920 for vertical
// video (1080x1920 is a width of 1080 and a height of 1920).
constexpr int kSnapWidths[] = {720, 1080, 1280, 1920, 2560, 3840};
constexpr int kSnapHeights[] = {720, 1080, 1280, 1440, 1920, 2160};
const QColor kGreen(0x3d, 0xd6, 0x8c); // the region frame's green

// Snap one side's length to the nearest listed size within reach, moving only
// the end that is being dragged.
void snapLength(int &lo, int &hi, bool dragHi, const int *sizes, int n, qreal dpr, int limit)
{
	const int len = hi - lo;
	for (int i = 0; i < n; ++i) {
		const int want = int(std::lround(sizes[i] / dpr));
		if (std::abs(len - want) > kSnap || want > limit)
			continue;
		if (dragHi && lo + want <= limit)
			hi = lo + want;
		else if (!dragHi && hi - want >= 0)
			lo = hi - want;
		return;
	}
}
} // namespace

RegionDraw::RegionDraw(QWidget *parent)
	: QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
{
	setAttribute(Qt::WA_TranslucentBackground);
	setMouseTracking(true);
	setCursor(Qt::CrossCursor);
	setFocusPolicy(Qt::StrongFocus);
}

void RegionDraw::start(QScreen *screen)
{
	if (screen) {
		setScreen(screen);
		setGeometry(screen->geometry());
		dpr_ = screen->devicePixelRatio();
		origin_ = screen->geometry().topLeft();
	}
	dragging_ = false;
	pointer_ = mapFromGlobal(QCursor::pos());
	show();
	raise();
	update();
}

void RegionDraw::cancel()
{
	if (!isVisible())
		return;
	dragging_ = false;
	hide();
	emit cancelled();
}

QRect RegionDraw::dragRect(const QPoint &a, const QPoint &b, const QSize &screen, qreal dpr)
{
	const int w = screen.width(), h = screen.height();
	int x0 = std::clamp(std::min(a.x(), b.x()), 0, w), x1 = std::clamp(std::max(a.x(), b.x()), 0, w);
	int y0 = std::clamp(std::min(a.y(), b.y()), 0, h), y1 = std::clamp(std::max(a.y(), b.y()), 0, h);
	// Edges near the screen's edges go all the way: the last few pixels of a
	// taskbar or a window border are never what was meant.
	if (x0 <= kSnap)
		x0 = 0;
	if (y0 <= kSnap)
		y0 = 0;
	if (w - x1 <= kSnap)
		x1 = w;
	if (h - y1 <= kSnap)
		y1 = h;
	// Then the size, from the side being dragged (the side away from where
	// the drag started), so the corner you began at stays put.
	snapLength(x0, x1, b.x() >= a.x(), kSnapWidths, int(std::size(kSnapWidths)), dpr, w);
	snapLength(y0, y1, b.y() >= a.y(), kSnapHeights, int(std::size(kSnapHeights)), dpr, h);
	return QRect(x0, y0, x1 - x0, y1 - y0);
}

QRect RegionDraw::toDevicePx(const QRect &r, qreal dpr)
{
	return QRect(int(std::lround(r.x() * dpr)), int(std::lround(r.y() * dpr)), int(std::lround(r.width() * dpr)),
		     int(std::lround(r.height() * dpr)));
}

QRect RegionDraw::devicePxOnScreen(const QRect &sel) const
{
	// The layer is meant to cover the display exactly, and then this is plain
	// toDevicePx. Windows can put it elsewhere on a display at another scale
	// (see NativeWindowOps.hpp); what was dragged over is then where the layer
	// really is plus the drag, not where Qt's arithmetic puts the layer.
	const std::optional<QRect> win = native_window::rect(this);
	if (!win)
		return toDevicePx(sel, dpr_);
	const qreal k = devicePixelRatioF();
	return QRect(win->x() + int(std::lround(sel.x() * k)) - origin_.x(),
		     win->y() + int(std::lround(sel.y() * k)) - origin_.y(), int(std::lround(sel.width() * k)),
		     int(std::lround(sel.height() * k)));
}

QRect RegionDraw::current() const
{
	return dragging_ ? dragRect(anchor_, pointer_, size(), dpr_) : QRect();
}

void RegionDraw::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QRect sel = current();

	// The screen dimmed, the area being drawn left bright. Not fully clear:
	// a fully transparent pixel of a layered window lets clicks through on
	// Windows, and the drag must keep the mouse.
	QPainterPath veil;
	veil.addRect(QRectF(rect()));
	if (!sel.isEmpty()) {
		QPainterPath hole;
		hole.addRect(QRectF(sel));
		veil = veil.subtracted(hole);
		p.fillRect(sel, QColor(0, 0, 0, 1));
	}
	p.fillPath(veil, QColor(0, 0, 0, 115));

	if (sel.isEmpty() && rect().contains(pointer_)) {
		// Before the press: two faint lines through the pointer, to line the
		// first corner up against what is on screen.
		p.setPen(QPen(QColor(255, 255, 255, 90), 1));
		p.drawLine(QPointF(0, pointer_.y() + 0.5), QPointF(width(), pointer_.y() + 0.5));
		p.drawLine(QPointF(pointer_.x() + 0.5, 0), QPointF(pointer_.x() + 0.5, height()));
	}
	if (!sel.isEmpty()) {
		p.setPen(QPen(kGreen, 2));
		p.setBrush(Qt::NoBrush);
		p.drawRect(QRectF(sel).adjusted(1, 1, -1, -1));
		// The size it will record at, beside the pointer, kept on screen.
		const QRect dev = toDevicePx(sel, dpr_);
		const QString label = QStringLiteral("%1 × %2").arg(dev.width()).arg(dev.height());
		QFont f = font();
		f.setBold(true);
		p.setFont(f);
		const QRect box = QFontMetrics(f).boundingRect(label).adjusted(-8, -4, 8, 4);
		QPoint at = pointer_ + QPoint(16, 16);
		at.setX(std::min(at.x(), width() - box.width() - 4));
		at.setY(std::min(at.y(), height() - box.height() - 4));
		const QRectF tag(at, box.size());
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(20, 22, 26, 225));
		p.drawRoundedRect(tag, 4, 4);
		p.setPen(QColor(0xe8, 0xea, 0xed));
		p.drawText(tag, Qt::AlignCenter, label);
	}

	// What to do, at the top -- out of the way once a drag has begun.
	if (!dragging_) {
		const QString help = QStringLiteral("Drag over the area to record   ·   Esc to cancel");
		QFont f = font();
		f.setPointSizeF(f.pointSizeF() * 1.15);
		f.setBold(true);
		p.setFont(f);
		const QRect box = QFontMetrics(f).boundingRect(help).adjusted(-14, -7, 14, 7);
		const QRectF hb(QPointF((width() - box.width()) / 2.0, 24), QSizeF(box.size()));
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(20, 22, 26, 220));
		p.drawRoundedRect(hb, 6, 6);
		p.setPen(QColor(0xe8, 0xea, 0xed));
		p.drawText(hb, Qt::AlignCenter, help);
	}
}

void RegionDraw::mousePressEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton) {
		cancel();
		return;
	}
	anchor_ = pointer_ = e->position().toPoint();
	dragging_ = true;
	update();
}

void RegionDraw::mouseMoveEvent(QMouseEvent *e)
{
	pointer_ = e->position().toPoint();
	update();
}

void RegionDraw::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton || !dragging_)
		return;
	pointer_ = e->position().toPoint();
	const QRect sel = current();
	dragging_ = false;
	if (sel.width() < kMinDrag || sel.height() < kMinDrag) {
		update(); // a click: keep waiting for a real drag
		return;
	}
	const QRect dev = devicePxOnScreen(sel); // before hide(): asked while the window is where it was dragged on
	hide();
	emit drawn(dev);
}

void RegionDraw::keyPressEvent(QKeyEvent *e)
{
	if (e->key() == Qt::Key_Escape)
		cancel();
	else
		QWidget::keyPressEvent(e);
}

void RegionDraw::showEvent(QShowEvent *e)
{
	QWidget::showEvent(e);
	activateWindow();
	setFocus();
}

} // namespace harpia
