#include "ZoomPreview.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

#include <cmath>

namespace harpia {

namespace {
// Zoomed in for this long, out for this long, repeating. Long enough that the
// chase settles and the dead zone is visible, short enough that the push-in --
// the thing the animation slider controls -- comes round again quickly.
constexpr qint64 kHoldInMs = 2600;
constexpr qint64 kHoldOutMs = 1400;
constexpr int kFrameMs = 16; // ~60 Hz, the rate the recorder ticks the zoom at
} // namespace

ZoomPreview::ZoomPreview(QWidget *parent) : QWidget(parent)
{
	setMinimumHeight(150);
	setMouseTracking(true);
	setCursor(Qt::CrossCursor);
	clock_.start();
	timer_ = new QTimer(this);
	timer_->setInterval(kFrameMs);
	connect(timer_, &QTimer::timeout, this, &ZoomPreview::tick);
}

void ZoomPreview::configure(const ZoomParams &params)
{
	params_ = params;
	update();
}

void ZoomPreview::setRunning(bool on)
{
	if (on && !timer_->isActive())
		timer_->start();
	else if (!on && timer_->isActive())
		timer_->stop();
}

void ZoomPreview::showEvent(QShowEvent *)
{
	setRunning(true);
}

void ZoomPreview::hideEvent(QHideEvent *)
{
	setRunning(false);
}

void ZoomPreview::resizeEvent(QResizeEvent *)
{
	rebuildMock();
	// Canvas and source are the same here: the preview is a whole "screen".
	zoom_.setCanvas(size(), size(), QPoint(width() / 2, height() / 2));
}

void ZoomPreview::mouseMoveEvent(QMouseEvent *e)
{
	hovering_ = true;
	cursor_ = e->pos();
}

void ZoomPreview::leaveEvent(QEvent *)
{
	hovering_ = false;
}

// A slow figure-eight, so the chase and the dead zone both show themselves
// without anyone having to move the mouse.
QPoint ZoomPreview::autoCursor(qint64 ms) const
{
	const double t = ms / 1000.0;
	const double x = 0.5 + 0.34 * std::sin(t * 0.9);
	const double y = 0.5 + 0.22 * std::sin(t * 1.8);
	return QPoint(int(x * width()), int(y * height()));
}

void ZoomPreview::tick()
{
	const qint64 now = clock_.elapsed();
	if (now >= nextToggleMs_) {
		const bool nowOn = zoom_.toggle();
		nextToggleMs_ = now + (nowOn ? kHoldInMs : kHoldOutMs);
	}
	const QPoint c = hovering_ ? cursor_ : autoCursor(now);
	zoom_.tick(c, params_, now);
	update();
}

// A plausible desktop: a window with a title bar and some lines of "text", so
// the magnification has something with real detail to enlarge. A flat colour
// would zoom into nothing and the preview would look broken.
void ZoomPreview::rebuildMock()
{
	if (width() <= 0 || height() <= 0)
		return;
	mock_ = QImage(size(), QImage::Format_RGB32);
	mock_.fill(QColor(0x1b, 0x1e, 0x24));

	QPainter p(&mock_);
	p.setRenderHint(QPainter::Antialiasing, true);

	const QRect win(width() / 8, height() / 6, width() * 3 / 4, height() * 2 / 3);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0x24, 0x28, 0x30));
	p.drawRoundedRect(win, 6, 6);
	p.setBrush(QColor(0x2f, 0x34, 0x3e));
	p.drawRoundedRect(QRect(win.x(), win.y(), win.width(), 16), 6, 6);
	p.setBrush(QColor(0xe5, 0x48, 0x4d));
	p.drawEllipse(QPoint(win.x() + 12, win.y() + 8), 3, 3);
	p.setBrush(QColor(0xd2, 0x99, 0x22));
	p.drawEllipse(QPoint(win.x() + 22, win.y() + 8), 3, 3);
	p.setBrush(QColor(0x3f, 0xb9, 0x50));
	p.drawEllipse(QPoint(win.x() + 32, win.y() + 8), 3, 3);

	// Lines of "code", ragged so the eye reads it as text.
	p.setBrush(QColor(0x55, 0x5d, 0x6b));
	int y = win.y() + 28;
	int seed = 7;
	while (y < win.bottom() - 8) {
		seed = (seed * 1103515245 + 12345) & 0x7fffffff;
		const int len = win.width() / 3 + (seed % (win.width() / 2));
		const int indent = (seed >> 8) % 3 * 10;
		p.drawRect(QRect(win.x() + 12 + indent, y, len, 4));
		y += 10;
	}
}

void ZoomPreview::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	if (mock_.isNull())
		rebuildMock();
	if (mock_.isNull())
		return;

	// The recorder's own answer for what the frame shows, drawn to fill the
	// widget -- which is exactly what the capture does with the scene transform.
	const QRect visible = zoom_.visibleRect().intersected(mock_.rect());
	p.setRenderHint(QPainter::SmoothPixmapTransform, true);
	p.drawImage(rect(), mock_, visible.isEmpty() ? mock_.rect() : visible);

	// The cursor being followed, drawn last so it sits over the picture.
	const QPoint c = hovering_ ? cursor_ : autoCursor(clock_.elapsed());
	const QRect src = visible.isEmpty() ? mock_.rect() : visible;
	if (src.width() > 0 && src.height() > 0) {
		const QPointF onScreen((c.x() - src.x()) * double(width()) / src.width(),
				       (c.y() - src.y()) * double(height()) / src.height());
		p.setPen(QPen(QColor(0, 0, 0, 160), 2));
		p.setBrush(QColor(0xff, 0xff, 0xff));
		const QPointF tip = onScreen;
		const QPolygonF arrow({tip, tip + QPointF(0, 13), tip + QPointF(4, 9.5),
				       tip + QPointF(7, 14), tip + QPointF(9.5, 12.5),
				       tip + QPointF(6.5, 8), tip + QPointF(11, 7)});
		p.drawPolygon(arrow);
	}

	p.setPen(QColor(0x3a, 0x3f, 0x48));
	p.setBrush(Qt::NoBrush);
	p.drawRect(rect().adjusted(0, 0, -1, -1));
}

} // namespace harpia
