#include "RegionTool.hpp"

#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
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
// The move handle: a grab tab above the top edge. The gap deliberately clears
// the top resize zone (kMargin + 2, see zoneAt) so the two never overlap and
// hit-testing needs no priority rule between them.
constexpr int kTabW = 48;
constexpr int kTabH = 14;
constexpr int kTabGap = kMargin + 3;
// The Record button below the frame. Same clearance trick against the Bottom
// resize zone that the tab uses against the Top one.
constexpr int kStartW = 84;
constexpr int kStartH = 24;
constexpr int kStartGap = kMargin + 3;
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

int RegionTool::topMargin() const
{
	return kMargin + (moveHandle_ ? kTabH + kTabGap : 0);
}

int RegionTool::bottomMargin() const
{
	return kMargin + kStartH + kStartGap;
}

QRect RegionTool::innerRectLocal() const
{
	return rect().adjusted(kMargin, topMargin(), -kMargin, -bottomMargin());
}

QRect RegionTool::startButtonRect() const
{
	if (!startButtonVisible())
		return QRect();
	const QRect inner = innerRectLocal();
	const int w = std::min(kStartW, inner.width());
	const int x = inner.center().x() - w / 2 + 1;
	// Below the frame, where it covers nothing being captured -- unless the
	// region runs to the bottom of the display, where there is no "below" and a
	// button off the edge of the screen could not be pressed at all.
	const int below = inner.bottom() + kStartGap;
	if (screen_ && mapToGlobal(QPoint(0, below + kStartH)).y() > screen_->geometry().bottom())
		return QRect(x, inner.bottom() - kStartH - 3, w, kStartH);
	return QRect(x, below, w, kStartH);
}

QRect RegionTool::moveHandleRect() const
{
	if (!moveHandle_)
		return QRect();
	const QRect inner = innerRectLocal();
	// Centred on the region, sitting in the band above it. A region narrower
	// than the tab still gets one -- clamped, so it never pokes out of the
	// widget and out of the input mask with it.
	const int w = std::min(kTabW, inner.width());
	const int x = inner.center().x() - w / 2 + 1;
	// Normally above the frame, where it covers nothing that is being captured.
	// But a region snapped to the top of the screen leaves no room up there, and
	// a tab hanging off the edge of the display cannot be grabbed at all -- so
	// there, and only there, it drops just inside the top edge.
	const int above = inner.top() - kTabGap - kTabH;
	if (screen_ && mapToGlobal(QPoint(0, above)).y() < screen_->geometry().top())
		return QRect(x, inner.top() + 3, w, kTabH);
	return QRect(x, above, w, kTabH);
}

void RegionTool::setMoveHandleEnabled(bool on)
{
	// Idempotent: this comes from the owner's state tick, four times a second.
	if (moveHandle_ == on)
		return;
	// The tab changes the widget's top inset, so the widget has to be re-laid
	// out around the SAME region. Read the region first, in global coordinates,
	// and put it back afterwards -- the alternative is the frame jumping up or
	// down by 27 px every time this is toggled, which would silently rewrite the
	// user's region through regionChanged().
	const QRect innerGlobal(mapToGlobal(innerRectLocal().topLeft()), innerRectLocal().size());
	moveHandle_ = on;
	// Before the first setRegionDevicePx there is no region to preserve, and
	// re-applying the empty one would emit a nonsense regionChanged() that the
	// owner would write straight into the preset.
	if (!screen_ || innerGlobal.width() < kMinSize || innerGlobal.height() < kMinSize) {
		update();
		return;
	}
	applyGeometry(innerGlobal);
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
	// Start from the frame band -- the region plus the handle margin -- rather
	// than the whole widget. With the move handle on, the widget extends well
	// above the frame to make room for the tab, and grabbing that whole empty
	// band would block clicks on whatever sits above the region.
	const QRect inner = innerRectLocal();
	QRegion mask(inner.adjusted(-kMargin, -kMargin, kMargin, kMargin));
	// Outside Editing the interior is click-through, so the app underneath stays
	// usable. That is not only a recording concern -- an overlay left up over
	// another window with a solid input area makes that window unclickable.
	if (mode_ != Mode::Editing)
		mask -= QRegion(inner.adjusted(2, 2, -2, -2));
	// The tab is the one piece deliberately outside the frame. It has to be in
	// the mask in EVERY mode, since being grabbable while another app is in
	// front is the whole reason it exists.
	if (moveHandle_)
		mask += moveHandleRect();
	// Likewise the Record button -- and only while it is actually drawn, so the
	// reserved band below the frame stops taking the mouse the moment recording
	// starts and the button goes away.
	if (startButtonVisible())
		mask += startButtonRect();
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
	// The widget is the region expanded by the handle margin, plus room for the
	// move tab above it when that is on.
	setGeometry(g.adjusted(-kMargin, -topMargin(), kMargin, bottomMargin()));
	rebuildMask();
	emitRegion();
	update();
}

RegionTool::Zone RegionTool::zoneAt(const QPoint &p) const
{
	// The tab first. It normally sits clear of every resize zone, but when it has
	// been flipped inside (region at the top of the screen) it lands on the Top
	// edge zone, and there it must win: it is the only way to move the region
	// while another app is in front.
	if (moveHandle_ && moveHandleRect().contains(p))
		return Zone::Move;
	// The Record button, before the resize zones for the same reason: flipped
	// inside for a region at the bottom of the screen it lands on the Bottom
	// edge zone, and pressing a button must not resize the thing it sits on.
	if (startButtonVisible() && startButtonRect().contains(p))
		return Zone::StartButton;

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
	showDims_ = (dragZone_ != Zone::Move && dragZone_ != Zone::None &&
		     dragZone_ != Zone::StartButton);
	startPressed_ = (dragZone_ == Zone::StartButton);
	update();
}

void RegionTool::mouseMoveEvent(QMouseEvent *e)
{
	if (dragZone_ == Zone::None || !(e->buttons() & Qt::LeftButton)) {
		// Not dragging: say what is under the pointer before it is pressed.
		// Without this the tab is a decoration that happens to be draggable and
		// the button is a picture that happens to be clickable.
		const bool overStart = startButtonVisible() && startButtonRect().contains(e->pos());
		if (overStart != startHover_) {
			startHover_ = overStart;
			update();
		}
		if (overStart)
			setCursor(Qt::PointingHandCursor);
		else if (moveHandle_ && moveHandleRect().contains(e->pos()))
			setCursor(Qt::SizeAllCursor);
		else
			setCursor(Qt::ArrowCursor);
		return;
	}

	// Holding the Record button and moving is not a drag of anything. Falling
	// through would re-apply the unchanged geometry on every move and emit a
	// regionChanged() per mouse event for a region that did not change.
	if (dragZone_ == Zone::StartButton) {
		const bool inside = startButtonRect().contains(e->pos());
		if (inside != startPressed_) {
			startPressed_ = inside; // un-press when slid off, re-press on return
			update();
		}
		return;
	}

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

void RegionTool::mouseReleaseEvent(QMouseEvent *e)
{
	const bool wasStart = dragZone_ == Zone::StartButton;
	const bool wasDragging = dragZone_ != Zone::None && !wasStart;
	dragZone_ = Zone::None;
	startPressed_ = false;
	showDims_ = false;
	update();

	if (wasStart) {
		// The ordinary button contract: a press you slide off before letting go
		// is cancelled. Starting a recording is not something to do by accident.
		if (startButtonRect().contains(e->pos()))
			emit startRecordingRequested();
		// Deliberately NOT interactionFinished(): nothing about the geometry
		// changed, and the owner only re-evaluates the overlay mode on that.
		return;
	}
	if (wasDragging)
		emit interactionFinished();
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

void RegionTool::leaveEvent(QEvent *e)
{
	// No move events arrive once the pointer is gone, so a hover left set here
	// would stay lit until the pointer came back.
	if (startHover_) {
		startHover_ = false;
		update();
	}
	QWidget::leaveEvent(e);
}

void RegionTool::contextMenuEvent(QContextMenuEvent *e)
{
	// Right-click the region to save it as a reusable named region, or manage
	// the saved list. Owner (MainWindow) handles the actual save/manage.
	QMenu menu;
	QAction *saveAct = menu.addAction(QStringLiteral("Save Region…"));
	QAction *manageAct = menu.addAction(QStringLiteral("Manage saved regions…"));
	QAction *chosen = menu.exec(e->globalPos());
	if (chosen == saveAct)
		emit saveRegionRequested();
	else if (chosen == manageAct)
		emit manageRegionsRequested();
}

void RegionTool::keyPressEvent(QKeyEvent *e)
{
	if (e->key() == Qt::Key_Escape && mode_ != Mode::Recording)
		emit cancelled();
	else
		QWidget::keyPressEvent(e);
}

void RegionTool::setMode(Mode m)
{
	// Idempotent: this is called from the state tick now, and rebuilding the
	// mask four times a second to arrive at the mask it already had is work
	// nobody asked for.
	if (mode_ == m)
		return;
	mode_ = m;
	if (m != Mode::Recording)
		paused_ = false;
	// Editing is the only state you are looking AT the overlay in; the other
	// two sit over somebody else's window, so they step back. Watching stays
	// well clear of the recording dim -- it has to be readable enough to line
	// up against the app underneath, which is the entire point of it.
	setWindowOpacity(m == Mode::Recording ? 0.28 : m == Mode::Watching ? 0.75 : 1.0);
	rebuildMask();
	update();
}

void RegionTool::setPaused(bool paused)
{
	if (paused_ == paused)
		return;
	paused_ = paused;
	update(); // repaint the border in the new color immediately
}

void RegionTool::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, false);

	const QRect inner = innerRectLocal();

	// Border color reflects the recording state at a glance:
	//   green = ready (not recording), red = recording, yellow = paused.
	QColor border;
	if (mode_ != Mode::Recording)
		border = QColor(0x3f, 0xb9, 0x50); // green — ready
	else if (paused_)
		border = QColor(0xd2, 0x99, 0x22); // yellow — paused
	else
		border = QColor(0xe5, 0x48, 0x4d); // red — recording
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

	// The move tab, in the same colour as the frame so it reads as part of it
	// (and turns red while recording along with everything else).
	if (moveHandle_) {
		const QRect tab = moveHandleRect();
		p.setRenderHint(QPainter::Antialiasing, true);
		p.setBrush(border);
		p.setPen(Qt::NoPen);
		p.drawRoundedRect(tab, 4, 4);
		// Three grip dots, so it looks like something you drag rather than a
		// button you click.
		p.setBrush(QColor(255, 255, 255, 220));
		for (int i = -1; i <= 1; ++i)
			p.drawEllipse(QPoint(tab.center().x() + i * 7, tab.center().y() + 1), 2, 2);
		p.setRenderHint(QPainter::Antialiasing, false);
	}

	// The Record button below the frame. Green: the same green the main window
	// uses for Resume and this frame uses for its ready border, so "green means
	// go" holds across all three.
	if (startButtonVisible()) {
		const QRect btn = startButtonRect();
		const QColor fill = startPressed_ ? QColor(0x35, 0xa0, 0x47)
				    : startHover_ ? QColor(0x4a, 0xc4, 0x62)
						  : QColor(0x3f, 0xb9, 0x50);
		p.setRenderHint(QPainter::Antialiasing, true);
		p.setBrush(fill);
		p.setPen(Qt::NoPen);
		p.drawRoundedRect(btn, 6, 6);
		// A record dot and the word, so it is unmistakably the start control and
		// not another handle.
		p.setBrush(Qt::white);
		p.drawEllipse(QPoint(btn.left() + 15, btn.center().y()), 4, 4);
		p.setPen(Qt::white);
		QFont f = p.font();
		f.setBold(true);
		p.setFont(f);
		p.drawText(btn.adjusted(26, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft,
			   QStringLiteral("Record"));
		p.setRenderHint(QPainter::Antialiasing, false);
	}

	// Live dimensions while resizing.
	if (showDims_) {
		const CaptureRegion r = region();
		const QString label = QStringLiteral("%1 × %2").arg(r.width).arg(r.height);
		p.setPen(Qt::white);
		p.drawText(inner.adjusted(6, 6, -6, -6), Qt::AlignTop | Qt::AlignLeft, label);
	}
}

RegionOverlayState regionOverlayState(bool regionCaptureMode, bool recording, bool harpiaFocused,
				      bool editorOpen)
{
	RegionOverlayState st;
	// Only Custom Region has a region to show. Entire Monitor has nothing to
	// frame, and leaving a rectangle floating over the desktop there would be
	// noise with no meaning.
	if (!regionCaptureMode)
		return st; // hidden

	// The editor is a window you work INSIDE, usually maximised. A frame
	// floating on top of it marks out a piece of the desktop that has nothing
	// to do with what is being edited, and it sits over the thing you are
	// trying to look at.
	//
	// Unless a recording is running: then the frame is saying "this is what is
	// going into the file", which is worth more than the nuisance -- and
	// hiding it would be hiding a recording in progress.
	if (editorOpen && !recording)
		return st;

	st.visible = true;
	if (recording) {
		st.mode = RegionTool::Mode::Recording;
		return st;
	}
	// The change that matters: NOT hidden when Harpia is in the background.
	// Lining the frame up against the app being recorded means clicking that
	// app, and the overlay used to leave with the focus.
	st.mode = harpiaFocused ? RegionTool::Mode::Editing : RegionTool::Mode::Watching;
	return st;
}

} // namespace harpia
