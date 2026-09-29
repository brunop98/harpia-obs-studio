#include "MultiAreaOverlay.hpp"

#include "core/MultiArea.hpp"

#include <QDateTime>
#include <QGuiApplication>
#include <QTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

#if defined(_WIN32)
// Without these, windows.h defines min/max (breaking std::min) and drags in
// rpcndr.h's `small` -- a macro for char.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011 // Windows 10 2004+; older SDKs lack the name
#endif
#endif

#include <cmath>

namespace harpia {

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

void MultiAreaOverlay::setStyle(const AreaStyle &style)
{
	style_ = style;
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
	const int w = 520, h = 40;
	return QRect((width() - w) / 2, 24, w, h);
}

QRect MultiAreaOverlay::doneButtonRectLocal() const
{
	const QRect bar = doneRectLocal();
	return QRect(bar.right() - 76, bar.top() + 7, 66, bar.height() - 14);
}

QRect MultiAreaOverlay::clearButtonRectLocal() const
{
	const QRect done = doneButtonRectLocal();
	return QRect(done.left() - 104, done.top(), 96, done.height());
}

bool MultiAreaOverlay::clearArmed() const
{
	return clearArmedMs_ > 0 && QDateTime::currentMSecsSinceEpoch() - clearArmedMs_ < 3000;
}

QRect MultiAreaOverlay::removeRectLocal(int i) const
{
	if (i < 0 || i >= tops_.size() || i == locked_)
		return QRect();
	const QRect r = areaRectLocal(i);
	const int d = 22;
	return QRect(r.right() - d - 6, r.top() + 6, d, d);
}

void MultiAreaOverlay::setNumbers(const QVector<int> &numbers)
{
	if (numbers == numbers_)
		return;
	numbers_ = numbers;
	update();
}

void MultiAreaOverlay::setLockedIndex(int i)
{
	if (i == locked_)
		return;
	locked_ = i;
	update();
}

void MultiAreaOverlay::setMaxAreas(int n)
{
	n = std::clamp(n, 0, kMaxAreas);
	if (n == maxAreas_)
		return;
	maxAreas_ = n;
	update();
}

bool MultiAreaOverlay::areaFits() const
{
	const QSize sd = screenDevice();
	return !size_.isEmpty() && (sd.isEmpty() || (size_.width() <= sd.width() && size_.height() <= sd.height()));
}

int MultiAreaOverlay::nextNumber() const
{
	// The number a stamp here would get: after every number shown anywhere,
	// which the owner reflects in the numbers it hands out.
	int n = int(tops_.size());
	for (int v : numbers_)
		n = std::max(n, v);
	return n + 1;
}

int MultiAreaOverlay::addAtLocal(const QPoint &local)
{
	if (tops_.size() >= maxAreas_ || !areaFits())
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
	// The region can move, but the recording needs it.
	if (i < 0 || i >= tops_.size() || i == locked_)
		return;
	tops_.remove(i);
	if (i < numbers_.size())
		numbers_.remove(i); // the owner renumbers after areasEdited
	if (locked_ > i)
		--locked_;
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

	const AreaStyle &st = style_;
	const int lw = std::max(1, st.lineWidth);
	const int badgeSize = st.badgeSize;
	auto badge = [&](const QRect &r, int number, const QColor &c) {
		if (badgeSize <= 0)
			return;
		const QRect b(r.left() + 8, r.top() + 8, badgeSize, badgeSize);
		p.setPen(Qt::NoPen);
		p.setBrush(c);
		p.drawEllipse(b);
		QFont bf = f;
		bf.setPixelSize(std::max(8, badgeSize * 14 / 26));
		p.setFont(bf);
		p.setPen(st.badgeTextColor);
		p.drawText(b, Qt::AlignCenter, QString::number(number));
		p.setFont(f);
	};
	// The outline sits inside the area, so a thick line never spills onto
	// pixels of the neighbouring area or past the screen edge.
	auto outline = [&](const QRect &r, const QPen &pen) {
		const qreal in = pen.widthF() / 2.0;
		const QRectF rr = QRectF(r).adjusted(in, in, -in, -in);
		p.setPen(pen);
		if (st.cornerRadius > 0)
			p.drawRoundedRect(rr, st.cornerRadius, st.cornerRadius);
		else
			p.drawRect(rr);
	};

	if (mode_ == Mode::Passive) {
		// Faint while recording: a map of where the shot can go, not a frame.
		p.setOpacity((recording_ ? st.recordingOpacity : st.idleOpacity) / 100.0);
		for (int i = 0; i < tops_.size(); ++i) {
			if (i == active_)
				continue;
			const QRect r = areaRectLocal(i);
			p.setBrush(Qt::NoBrush);
			outline(r, QPen(st.lineColor, lw, st.penStyle()));
			badge(r, numberOf(i), st.badgeColor);
		}
		return;
	}

	// ---- Arrange ----
	QPainterPath dim;
	dim.addRect(rect());
	for (int i = 0; i < tops_.size(); ++i)
		dim.addRect(areaRectLocal(i));
	dim.setFillRule(Qt::OddEvenFill);
	p.fillPath(dim, QColor(0, 0, 0, st.arrangeDimPct * 255 / 100));

	for (int i = 0; i < tops_.size(); ++i) {
		const QRect r = areaRectLocal(i);
		const bool hot = i == hover_ || i == drag_;
		QColor fill = st.lineColor;
		fill.setAlpha(st.hoverFillPct * 255 / 100);
		// Alpha 1 when not hot keeps the interior hit-testable on platforms
		// that pass fully transparent pixels through.
		p.setBrush(hot ? fill : QColor(0, 0, 0, 1));
		// Area 1 (the region) is always solid, so it reads apart from copies.
		outline(r, QPen(st.lineColor, hot ? lw + 1 : lw, i == locked_ ? Qt::SolidLine : st.penStyle()));
		badge(r, numberOf(i), st.badgeColor);
		// A visible way to remove it -- right-click and Delete work too, but
		// nothing on screen said so.
		const QRect x = removeRectLocal(i);
		if (!x.isNull()) {
			p.setPen(Qt::NoPen);
			p.setBrush(i == hoverRemove_ ? QColor(0xe5, 0x48, 0x4d) : QColor(20, 22, 26, 220));
			p.drawEllipse(x);
			p.setPen(QPen(Qt::white, 2));
			const QRect c = x.adjusted(7, 7, -7, -7);
			p.drawLine(c.topLeft(), c.bottomRight());
			p.drawLine(c.topRight(), c.bottomLeft());
		}
		if (i == locked_) {
			p.setPen(Qt::white);
			p.drawText(r.adjusted(8 + std::max(0, badgeSize) + 8, 8, -8, -8), Qt::AlignLeft | Qt::AlignTop,
				   QStringLiteral("Region"));
		}
	}

	// Where a click would stamp the next one.
	if (pointerIn_ && hover_ < 0 && drag_ < 0 && tops_.size() < maxAreas_ && areaFits() &&
	    !doneRectLocal().contains(pointer_)) {
		const QPoint d = toDevice(pointer_);
		const QPoint t = AreaSwitcher::clampTop(d - QPoint(size_.width() / 2, size_.height() / 2), size_,
							screenDevice());
		const QRect g(int(std::lround(t.x() / dpr_)), int(std::lround(t.y() / dpr_)),
			      int(std::lround(size_.width() / dpr_)), int(std::lround(size_.height() / dpr_)));
		QColor ghost = st.ghostColor;
		ghost.setAlpha(st.ghostOpacity * 255 / 100);
		p.setBrush(Qt::NoBrush);
		outline(g, QPen(ghost, lw, Qt::DotLine));
		badge(g, nextNumber(), ghost);
	}

	// Instructions and Done.
	const QRect done = doneRectLocal();
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(20, 22, 26, 230));
	p.drawRoundedRect(done, 10, 10);
	QFont hintFont = font();
	hintFont.setPixelSize(12);
	p.setFont(hintFont);
	p.setPen(QColor(230, 230, 230));
	const QString hint =
		!areaFits() ? QStringLiteral("The region is bigger than this screen · Done when finished")
		: tops_.size() >= maxAreas_
			? QStringLiteral("%1 areas (the most) · drag to move · × to remove").arg(kMaxAreas)
			: QStringLiteral("Click to add an area · drag to move · × to remove");
	const QRect clear = clearButtonRectLocal();
	p.drawText(QRect(done.left() + 14, done.top(), clear.left() - done.left() - 20, done.height()),
		   Qt::AlignVCenter | Qt::AlignLeft, hint);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0xe5, 0x48, 0x4d));
	p.drawRoundedRect(clear, 6, 6);
	p.setPen(Qt::white);
	p.setFont(f);
	p.drawText(clear, Qt::AlignCenter, clearArmed() ? QStringLiteral("Click again") : QStringLiteral("Remove all"));
	const QRect btn = doneButtonRectLocal();
	p.setPen(Qt::NoPen);
	p.setBrush(st.lineColor);
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
		if (e->button() == Qt::LeftButton) {
			if (doneButtonRectLocal().contains(pos))
				emit arrangeFinished();
			else if (clearButtonRectLocal().contains(pos)) {
				if (clearArmed()) {
					clearArmedMs_ = 0;
					emit clearAllRequested();
				} else {
					clearArmedMs_ = QDateTime::currentMSecsSinceEpoch();
					QTimer::singleShot(3100, this, qOverload<>(&QWidget::update));
				}
				update();
			}
		}
		return;
	}
	// An area's × (topmost first, like the hit test).
	if (e->button() == Qt::LeftButton)
		for (int k = int(tops_.size()) - 1; k >= 0; --k)
			if (removeRectLocal(k).contains(pos)) {
				removeArea(k);
				hoverRemove_ = -1;
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
		hoverRemove_ = -1;
		for (int k = int(tops_.size()) - 1; k >= 0 && hoverRemove_ < 0; --k)
			if (removeRectLocal(k).contains(pointer_))
				hoverRemove_ = k;
		setCursor(hoverRemove_ >= 0 || doneButtonRectLocal().contains(pointer_) ||
					  clearButtonRectLocal().contains(pointer_)
				  ? Qt::PointingHandCursor
			  : hover_ >= 0                  ? Qt::OpenHandCursor
			  : doneRectLocal().contains(pointer_) ? Qt::ArrowCursor
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
