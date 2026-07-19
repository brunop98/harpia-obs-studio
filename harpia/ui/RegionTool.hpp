#pragma once

#include "core/CaptureManager.hpp"

#include <QRect>
#include <QWidget>

class QScreen;

namespace harpia {

// An interactive, always-on-top region selector that lives on the desktop as a
// lightweight overlay (independent of the recording pipeline). It draws a thin
// colored border with eight resize handles; the user drags the interior to move
// it and the handles to resize. Every change emits regionChanged() so the owner
// can update the live crop — this works even while recording.
//
// Interior input:
//  - Editing (not recording): the whole rectangle grabs the mouse so it can be
//    dragged to move.
//  - Recording: the interior becomes click-through (mask = border+handles only)
//    and opacity drops, so it acts as a subtle recording indicator while apps
//    under the region stay usable; handles/border remain grabbable to live-edit.
//
// Coordinates: regionChanged() reports device-pixel coordinates relative to the
// target screen's top-left — the space crop_filter expects.
class RegionTool : public QWidget {
	Q_OBJECT
public:
	explicit RegionTool(QWidget *parent = nullptr);

	// Bind to a screen and set the initial region (device pixels rel. to screen).
	void setScreen(QScreen *screen);
	void setRegionDevicePx(const QRect &deviceRect);

	// Current region as a CaptureRegion (device px rel. to screen).
	CaptureRegion region() const;

	// Recording mode: dim + interior click-through, still resizable.
	void setRecordingMode(bool recording);

	// Paused state — only meaningful while recording; drives the border color
	// (yellow when paused, red while actively recording).
	void setPaused(bool paused);

signals:
	void regionChanged(const CaptureRegion &region);
	void cancelled(); // Esc pressed while not recording

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void mouseDoubleClickEvent(QMouseEvent *) override;
	void keyPressEvent(QKeyEvent *) override;

private:
	// Which part of the frame a press landed on.
	enum class Zone { None, Move, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };

	Zone zoneAt(const QPoint &localPos) const;
	void applyGeometry(const QRect &globalRect); // clamp to screen, set geometry, emit
	void rebuildMask();
	void emitRegion();
	QRect innerRectLocal() const; // the region rect within the widget (handle margin inset)
	void snap(QRect &globalRect) const;

	QScreen *screen_ = nullptr;
	qreal dpr_ = 1.0;
	bool recording_ = false;
	bool paused_ = false;

	Zone dragZone_ = Zone::None;
	QPoint dragStartGlobal_;
	QRect dragStartGeom_;
	bool showDims_ = false;
};

} // namespace harpia
