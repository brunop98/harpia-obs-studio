#include "MultiAreaOverlay.hpp"

#include "core/MultiArea.hpp"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cmath>

namespace harpia {

namespace {
const QColor kAreaColor(0x3d, 0xd6, 0x8c);  // the region frame's editing green
const QColor kGhostColor(255, 255, 255, 170);
constexpr int kBadge = 26;                  // number badge, px across
} // namespace

MultiAreaOverlay::MultiAreaOverlay(QWidget *parent) : QWidget(parent)
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_ShowWithoutActivating);
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	applyInputFlags();
}

void MultiAreaOverlay::setScreen(QScreen *screen)
{
	// Called from a 4 Hz state tick: nothing to do when nothing moved.
	if (screen == screen_ && (!screen || geometry() == screen->geometry()))
		return;
	screen_ = screen;
	dpr_ = screen ? screen->devicePixelRatio() : 1.0;
	if (screen)
		setGeometry(screen->geometry());
	update();
}

void MultiAreaOverlay::setAreas(const QVector<QPoint> &tops, QSize size)
{
	if (tops == tops_ && size == size_)
		return;
	tops_ = tops;
	size_ = size;
	hover_ = std::min(hover_, int(tops_.size()) - 1);
	update();
}

void MultiAreaOverlay::setActive(int index)
{
	if (index == active_)
		return;
	active_ = index;
	update();
}

void MultiAreaOverlay::setRecording(bool on)
{
	if (on == recording_)
		return;
	recording_ = on;
	update();
}

void MultiAreaOverlay::setMode(Mode m)
{
	if (m == mode_)
		return;
	mode_ = m;
	drag_ = -1;
	hover_ = -1;
	applyInputFlags();
	if (m == Mode::Arrange) {
		raise();
		activateWindow();
		setFocus();
	}
	update();
}

void MultiAreaOverlay::applyInputFlags()
{
	// Passive must never take a click meant for the app underneath -- the
	// whole screen is covered. Changing the flag recreates the native window
	// on some platforms, so re-show it if it was up.
	const bool wasVisible = isVisible();
	setWindowFlag(Qt::WindowTransparentForInput, mode_ == Mode::Passive);
	setWindowFlag(Qt::WindowDoesNotAcceptFocus, mode_ == Mode::Passive);
	if (wasVisible)
		show();
}

void MultiAreaOverlay::showEvent(QShowEvent *e)
{
	QWidget::showEvent(e);
#if defined(_WIN32)
	// A guide, not content: omitted from anything that captures the screen
	// (see RegionTool::excludeFromCapture for the details).
	SetWindowDisplayAffinity((HWND)winId(), WDA_EXCLUDEFROMCAPTURE);
#endif
}

QSize MultiAreaOverlay::screenDevice() const
{
	const QRect g = screen_ ? screen_->geometry() : geometry();
	return QSize(int(std::lround(g.width() * dpr_)), int(std::lround(g.height() * dpr_)));
}

QPoint MultiAreaOverlay::toDevice(const QPointF &local) const
{
	return QPoint(int(std::lround(local.x() * dpr_)), int(std::lround(local.y() * dpr_)));
}

QRect MultiAreaOverlay::areaRectLocal(int i) const
{
	if (i < 0 || i >= tops_.size())
		return QRect();
	const QPoint t = tops_[i];
	return QRect(int(std::lround(t.x() / dpr_)), int(std::lround(t.y() / dpr_)),
		     int(std::lround(size_.width() / dpr_)), int(std::lround(size_.height() / dpr_)));
}

int MultiAreaOverlay::areaAtLocal(const QPoint &local) const
{
	// Topmost first: later areas are drawn over earlier ones.
	const QPoint d = toDevice(local);
	for (int i = int(tops_.size()) - 1; i >= 0; --i)
		if (AreaSwitcher::areaAt({tops_[i]}, size_, d) == 0)
			return i;
	return -1;
}

QRect MultiAreaOverlay::doneRectLocal() const
{
	const int w = 380, h = 40;
	return QRect((width() - w) / 2, 24, w, h);
}

int MultiAreaOverlay::addAtLocal(const QPoint &local)
{
	if (tops_.size() >= kMaxAreas || size_.isEmpty())
		return -1;
	const QPoint d = toDevice(local);
	const QPoint t = AreaSwitcher::clampTop(d - QPoint(size_.width() / 2, size_.height() / 2), size_,
						screenDevice());
	tops_.push_back(t);
	update();
	emit areasEdited(tops_);
	return int(tops_.size()) - 1;
}

void MultiAreaOverlay::removeArea(int i)
{
	// Area 1 is the region: it can move, but the recording needs it.
	if (i <= 0 || i >= tops_.size())
		return;
	tops_.remove(i);
	hover_ = -1;
	if (active_ >= tops_.size())
		active_ = 0;
	update();
	emit areasEdited(tops_);
}

void MultiAreaOverlay::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);
	QFont f = font();
	f.setBold(true);
	f.setPixelSize(14);
	p.setFont(f);

	auto badge = [&](const QRect &r, int i, const QColor &c) {
		const QRect b(r.left() + 8, r.top() + 8, kBadge, kBadge);
		p.setPen(Qt::NoPen);
		p.setBrush(c);
		p.drawEllipse(b);
		p.setPen(Qt::black);
		p.drawText(b, Qt::AlignCenter, QString::number(i + 1));
	};

	if (mode_ == Mode::Passive) {
		// Faint while recording: a map of where the shot can go, not a frame.
		p.setOpacity(recording_ ? 0.35 : 0.85);
		for (int i = 0; i < tops_.size(); ++i) {
			if (i == active_)
				continue;
			const QRect r = areaRectLocal(i);
			QPen pen(kAreaColor, 2, Qt::DashLine);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			p.drawRect(r.adjusted(1, 1, -1, -1));
			badge(r, i, kAreaColor);
		}
		return;
	}

	// ---- Arrange ----
	QPainterPath dim;
	dim.addRect(rect());
	for (int i = 0; i < tops_.size(); ++i)
		dim.addRect(areaRectLocal(i));
	dim.setFillRule(Qt::OddEvenFill);
	p.fillPath(dim, QColor(0, 0, 0, 90));

	for (int i = 0; i < tops_.size(); ++i) {
		const QRect r = areaRectLocal(i);
		const bool hot = i == hover_ || i == drag_;
		p.setPen(QPen(kAreaColor, hot ? 3 : 2, i == 0 ? Qt::SolidLine : Qt::DashLine));
		p.setBrush(hot ? QColor(61, 214, 140, 40) : QColor(0, 0, 0, 1)); // 1: keeps it hit-testable
		p.drawRect(r.adjusted(1, 1, -1, -1));
		badge(r, i, kAreaColor);
		if (i == 0) {
			p.setPen(Qt::white);
			p.drawText(r.adjusted(8 + kBadge + 8, 8, -8, -8), Qt::AlignLeft | Qt::AlignTop,
				   QStringLiteral("Region"));
		}
	}

	// Where a click would stamp the next one.
	if (pointerIn_ && hover_ < 0 && drag_ < 0 && tops_.size() < kMaxAreas && !size_.isEmpty() &&
	    !doneRectLocal().contains(pointer_)) {
		const QPoint d = toDevice(pointer_);
		const QPoint t = AreaSwitcher::clampTop(d - QPoint(size_.width() / 2, size_.height() / 2), size_,
							screenDevice());
		const QRect g(int(std::lround(t.x() / dpr_)), int(std::lround(t.y() / dpr_)),
			      int(std::lround(size_.width() / dpr_)), int(std::lround(size_.height() / dpr_)));
		p.setPen(QPen(kGhostColor, 2, Qt::DotLine));
		p.setBrush(Qt::NoBrush);
		p.drawRect(g.adjusted(1, 1, -1, -1));
		badge(g, int(tops_.size()), kGhostColor);
	}

	// Instructions and Done.
	const QRect done = doneRectLocal();
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(20, 22, 26, 230));
	p.drawRoundedRect(done, 10, 10);
	QFont small = font();
	small.setPixelSize(12);
	p.setFont(small);
	p.setPen(QColor(230, 230, 230));
	const QString hint =
		tops_.size() >= kMaxAreas
			? QStringLiteral("%1 areas (the most). Drag to move · right-click to remove").arg(kMaxAreas)
			: QStringLiteral("Click to add an area · drag to move · right-click to remove");
	p.drawText(done.adjusted(14, 0, -86, 0), Qt::AlignVCenter | Qt::AlignLeft, hint);
	const QRect btn(done.right() - 76, done.top() + 7, 66, done.height() - 14);
	p.setBrush(kAreaColor);
	p.drawRoundedRect(btn, 6, 6);
	p.setPen(Qt::black);
	p.setFont(f);
	p.drawText(btn, Qt::AlignCenter, QStringLiteral("Done"));
}

void MultiAreaOverlay::mousePressEvent(QMouseEvent *e)
{
	if (mode_ != Mode::Arrange)
		return;
	const QPoint pos = e->position().toPoint();
	if (doneRectLocal().contains(pos)) {
		if (e->button() == Qt::LeftButton)
			emit arrangeFinished();
		return;
	}
	const int i = areaAtLocal(pos);
	if (e->button() == Qt::RightButton) {
		removeArea(i);
		return;
	}
	if (e->button() != Qt::LeftButton)
		return;
	if (i >= 0) {
		drag_ = i;
		dragOffset_ = toDevice(pos) - tops_[i];
		dragMoved_ = false;
	} else {
		// Stamp, and keep hold of it: press-and-drag places it precisely.
		drag_ = addAtLocal(pos);
		if (drag_ >= 0) {
			dragOffset_ = toDevice(pos) - tops_[drag_];
			dragMoved_ = false;
		}
	}
	hover_ = drag_;
	update();
}

void MultiAreaOverlay::mouseMoveEvent(QMouseEvent *e)
{
	pointer_ = e->position().toPoint();
	pointerIn_ = true;
	if (mode_ != Mode::Arrange)
		return;
	if (drag_ >= 0 && drag_ < tops_.size()) {
		const QPoint t = AreaSwitcher::clampTop(toDevice(pointer_) - dragOffset_, size_, screenDevice());
		if (t != tops_[drag_]) {
			tops_[drag_] = t;
			dragMoved_ = true;
		}
		setCursor(Qt::ClosedHandCursor);
	} else {
		hover_ = areaAtLocal(pointer_);
		setCursor(hover_ >= 0 ? Qt::OpenHandCursor
			  : doneRectLocal().contains(pointer_) ? Qt::PointingHandCursor
							       : Qt::CrossCursor);
	}
	update();
}

void MultiAreaOverlay::mouseReleaseEvent(QMouseEvent *e)
{
	if (mode_ != Mode::Arrange || e->button() != Qt::LeftButton || drag_ < 0)
		return;
	const bool moved = dragMoved_;
	drag_ = -1;
	hover_ = areaAtLocal(e->position().toPoint());
	if (moved)
		emit areasEdited(tops_);
	update();
}

void MultiAreaOverlay::keyPressEvent(QKeyEvent *e)
{
	if (mode_ != Mode::Arrange) {
		QWidget::keyPressEvent(e);
		return;
	}
	switch (e->key()) {
	case Qt::Key_Escape:
	case Qt::Key_Return:
	case Qt::Key_Enter:
		emit arrangeFinished();
		break;
	case Qt::Key_Delete:
	case Qt::Key_Backspace:
		removeArea(hover_);
		break;
	default:
		QWidget::keyPressEvent(e);
	}
}

void MultiAreaOverlay::leaveEvent(QEvent *e)
{
	pointerIn_ = false;
	if (drag_ < 0)
		hover_ = -1;
	update();
	QWidget::leaveEvent(e);
}

} // namespace harpia
