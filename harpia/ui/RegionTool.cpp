#include "RegionTool.hpp"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>

#include <algorithm>

namespace harpia {

namespace {
constexpr int kMargin = 10;   // frame margin around the region (for handles)
constexpr int kHandle = 8;    // handle square size
constexpr int kSnap = 12;     // snap threshold (logical px)
constexpr int kMinSize = 32;  // minimum region size (logical px)
const QColor kAccent(0, 174, 239);

// Common capture resolutions to snap to (device pixels).
const QSize kSnapSizes[] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
} // namespace

RegionTool::RegionTool(QWidget *parent) : QWidget(parent)
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
	setAttribute(Qt::WA_TranslucentBackground);
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
}

void RegionTool::setScreen(QScreen *screen)
{
	screen_ = screen;
	dpr_ = screen ? screen->devicePixelRatio() : 1.0;
}

QRect RegionTool::innerRectLocal() const
{
	return rect().adjusted(kMargin, kMargin, -kMargin, -kMargin);
}

void RegionTool::setRegionDevicePx(const QRect &deviceRect)
{
	if (!screen_)
		screen_ = QGuiApplication::primaryScreen();
	if (!screen_)
		return;
	const QPoint origin = screen_->geometry().topLeft();
	const QRect regionGlobal(origin.x() + int(deviceRect.x() / dpr_), origin.y() + int(deviceRect.y() / dpr_),
				 std::max(kMinSize, int(deviceRect.width() / dpr_)),
				 std::max(kMinSize, int(deviceRect.height() / dpr_)));
	applyGeometry(regionGlobal);
}

CaptureRegion RegionTool::region() const
{
	CaptureRegion r;
	if (!screen_)
		return r;
	// Inner region in global logical coords.
	const QRect innerGlobal(mapToGlobal(innerRectLocal().topLeft()), innerRectLocal().size());
	const QPoint origin = screen_->geometry().topLeft();
	r.enabled = true;
	r.x = int((innerGlobal.x() - origin.x()) * dpr_);
	r.y = int((innerGlobal.y() - origin.y()) * dpr_);
	r.width = int(innerGlobal.width() * dpr_);
	r.height = int(innerGlobal.height() * dpr_);
	return r;
}

void RegionTool::emitRegion()
{
	emit regionChanged(region());
}

void RegionTool::rebuildMask()
{
	if (!recording_) {
		clearMask(); // whole widget grabs input (drag interior to move)
		return;
	}
	// Recording: only the border frame + handles are interactive; the interior
	// is click-through so the app being recorded stays usable.
	QRegion mask(rect());
	mask -= QRegion(innerRectLocal().adjusted(2, 2, -2, -2));
	// Re-add handle squares (they sit on the border, already included, but keep
	// explicit for clarity).
	setMask(mask);
}

void RegionTool::applyGeometry(const QRect &globalRect)
{
	QRect g = globalRect;
	// Clamp the region within the screen.
	if (screen_) {
		const QRect s = screen_->geometry();
		if (g.width() > s.width())
			g.setWidth(s.width());
		if (g.height() > s.height())
			g.setHeight(s.height());
		if (g.left() < s.left())
			g.moveLeft(s.left());
		if (g.top() < s.top())
			g.moveTop(s.top());
		if (g.right() > s.right())
			g.moveRight(s.right());
		if (g.bottom() > s.bottom())
			g.moveBottom(s.bottom());
	}
	// The widget is the region expanded by the handle margin.
	setGeometry(g.adjusted(-kMargin, -kMargin, kMargin, kMargin));
	rebuildMask();
	emitRegion();
	update();
}

RegionTool::Zone RegionTool::zoneAt(const QPoint &p) const
{
	const QRect inner = innerRectLocal();
	const int m = kMargin + 2;
	const bool nearL = std::abs(p.x() - inner.left()) <= m;
	const bool nearR = std::abs(p.x() - inner.right()) <= m;
	const bool nearT = std::abs(p.y() - inner.top()) <= m;
	const bool nearB = std::abs(p.y() - inner.bottom()) <= m;

	if (nearT && nearL)
		return Zone::TopLeft;
	if (nearT && nearR)
		return Zone::TopRight;
	if (nearB && nearL)
		return Zone::BottomLeft;
	if (nearB && nearR)
		return Zone::BottomRight;
	if (nearL)
		return Zone::Left;
	if (nearR)
		return Zone::Right;
	if (nearT)
		return Zone::Top;
	if (nearB)
		return Zone::Bottom;
	if (inner.contains(p))
		return Zone::Move;
	return Zone::None;
}

void RegionTool::snap(QRect &g) const
{
	if (!screen_)
		return;
	const QRect s = screen_->geometry();
	// Snap edges to the screen bounds.
	if (std::abs(g.left() - s.left()) <= kSnap)
		g.moveLeft(s.left());
	if (std::abs(g.top() - s.top()) <= kSnap)
		g.moveTop(s.top());
	if (std::abs(g.right() - s.right()) <= kSnap)
		g.moveRight(s.right());
	if (std::abs(g.bottom() - s.bottom()) <= kSnap)
		g.moveBottom(s.bottom());
	// Snap the size to common resolutions (converted to logical px).
	for (const QSize &sz : kSnapSizes) {
		const int lw = int(sz.width() / dpr_);
		const int lh = int(sz.height() / dpr_);
		if (std::abs(g.width() - lw) <= kSnap)
			g.setWidth(lw);
		if (std::abs(g.height() - lh) <= kSnap)
			g.setHeight(lh);
	}
}

void RegionTool::mousePressEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;
	dragZone_ = zoneAt(e->pos());
	dragStartGlobal_ = e->globalPosition().toPoint();
	// Store the current region geometry (global) as the drag anchor.
	dragStartGeom_ = QRect(mapToGlobal(innerRectLocal().topLeft()), innerRectLocal().size());
	showDims_ = (dragZone_ != Zone::Move && dragZone_ != Zone::None);
	update();
}

void RegionTool::mouseMoveEvent(QMouseEvent *e)
{
	if (dragZone_ == Zone::None || !(e->buttons() & Qt::LeftButton))
		return;

	const QPoint delta = e->globalPosition().toPoint() - dragStartGlobal_;
	QRect g = dragStartGeom_;

	switch (dragZone_) {
	case Zone::Move:
		g.translate(delta);
		break;
	case Zone::Left:
		g.setLeft(g.left() + delta.x());
		break;
	case Zone::Right:
		g.setRight(g.right() + delta.x());
		break;
	case Zone::Top:
		g.setTop(g.top() + delta.y());
		break;
	case Zone::Bottom:
		g.setBottom(g.bottom() + delta.y());
		break;
	case Zone::TopLeft:
		g.setTopLeft(g.topLeft() + delta);
		break;
	case Zone::TopRight:
		g.setTopRight(g.topRight() + delta);
		break;
	case Zone::BottomLeft:
		g.setBottomLeft(g.bottomLeft() + delta);
		break;
	case Zone::BottomRight:
		g.setBottomRight(g.bottomRight() + delta);
		break;
	default:
		break;
	}

	if (g.width() < kMinSize)
		g.setWidth(kMinSize);
	if (g.height() < kMinSize)
		g.setHeight(kMinSize);

	snap(g);
	applyGeometry(g);
}

void RegionTool::mouseReleaseEvent(QMouseEvent *)
{
	dragZone_ = Zone::None;
	showDims_ = false;
	update();
}

void RegionTool::mouseDoubleClickEvent(QMouseEvent *)
{
	if (!screen_)
		return;
	const QRect s = screen_->geometry();
	QRect inner(mapToGlobal(innerRectLocal().topLeft()), innerRectLocal().size());
	inner.moveCenter(s.center());
	applyGeometry(inner);
}

void RegionTool::keyPressEvent(QKeyEvent *e)
{
	if (e->key() == Qt::Key_Escape && !recording_)
		emit cancelled();
	else
		QWidget::keyPressEvent(e);
}

void RegionTool::setRecordingMode(bool recording)
{
	recording_ = recording;
	setWindowOpacity(recording ? 0.28 : 1.0);
	rebuildMask();
	update();
}

void RegionTool::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, false);

	const QRect inner = innerRectLocal();

	// Green while idle (ready), red while recording — instant state feedback.
	QColor border = recording_ ? QColor(0xe5, 0x48, 0x4d) : QColor(0x3f, 0xb9, 0x50);
	p.setPen(QPen(border, 2));
	p.setBrush(Qt::NoBrush);
	p.drawRect(inner);

	// Eight resize handles.
	p.setBrush(border);
	p.setPen(Qt::NoPen);
	const int h = kHandle;
	const QPoint pts[8] = {
		inner.topLeft(),
		{inner.center().x(), inner.top()},
		inner.topRight(),
		{inner.left(), inner.center().y()},
		{inner.right(), inner.center().y()},
		inner.bottomLeft(),
		{inner.center().x(), inner.bottom()},
		inner.bottomRight(),
	};
	for (const QPoint &c : pts)
		p.drawRect(QRect(c.x() - h / 2, c.y() - h / 2, h, h));

	// Live dimensions while resizing.
	if (showDims_) {
		const CaptureRegion r = region();
		const QString label = QStringLiteral("%1 × %2").arg(r.width).arg(r.height);
		p.setPen(Qt::white);
		p.drawText(inner.adjusted(6, 6, -6, -6), Qt::AlignTop | Qt::AlignLeft, label);
	}
}

} // namespace harpia
