#pragma once

// "Draw the area": drag over what you want to record.
//
// The Custom Region frame used to start as a rectangle in the middle of the
// screen, and the only way to the area you wanted was moving it and dragging
// its handles until it fitted. This is the direct way: the display being
// recorded dims, you press where the area starts and drag to where it ends,
// and on release that rectangle is the region -- like the Snipping Tool. The
// frame and its handles are still there afterwards, for fine adjustment.
//
// While dragging, the size shows beside the pointer in the pixels it will be
// recorded at, and the edges snap to the screen's edges and the size to common
// resolutions (landscape and vertical), as the frame's own handles do. Esc or
// a right-click cancels and leaves the region as it was. A click without a
// drag does nothing: it is too easy to make by accident.
//
// Coordinates: drawn() reports device pixels relative to the screen's
// top-left -- the space RegionTool and the crop filter use.

#include <QPoint>
#include <QRect>
#include <QWidget>

class QScreen;

namespace harpia {

class RegionDraw : public QWidget {
	Q_OBJECT
public:
	explicit RegionDraw(QWidget *parent = nullptr);

	// Cover `screen` (the display being recorded) and wait for a drag.
	void start(QScreen *screen);
	void cancel();
	bool active() const { return isVisible(); }

	// The rectangle a drag from `a` to `b` makes, in the layer's own logical
	// coordinates: clamped to the screen, edges snapped to its edges, the size
	// snapped to common resolutions (the side being dragged gives way). Pure,
	// so the snapping can be checked without a screen.
	static QRect dragRect(const QPoint &a, const QPoint &b, const QSize &screen, qreal dpr);
	// A logical rectangle on the layer as device pixels (what gets recorded).
	static QRect toDevicePx(const QRect &logical, qreal dpr);
	// Below this (logical px, either side) a release is a click, not an area.
	static constexpr int kMinDrag = 16;

signals:
	void drawn(const QRect &devicePx);
	void cancelled();

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void keyPressEvent(QKeyEvent *) override;
	void showEvent(QShowEvent *) override;

private:
	QRect current() const; // the rectangle being dragged, or null
	// A rectangle on the layer as device px of the display, from where the
	// layer really is (see NativeWindowOps.hpp).
	QRect devicePxOnScreen(const QRect &logical) const;
	qreal dpr_ = 1.0;
	QPoint origin_; // the display's top-left (physical px)
	QPoint anchor_;          // where the drag started (local)
	QPoint pointer_{-1, -1}; // where the pointer is (local), for the guides
	bool dragging_ = false;
};

} // namespace harpia
