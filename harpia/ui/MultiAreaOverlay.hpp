#pragma once

#include "AreaStyle.hpp"

#include <QPoint>
#include <QSize>
#include <QVector>
#include <QWidget>

class QScreen;

namespace harpia {

// The Multi-Area layout on the desktop: one transparent window over the whole
// recorded screen.
//
//   Passive  -- the other areas as dashed outlines with their numbers, so you
//               can see where the recording will switch to. Click-through: it
//               never takes the mouse. The area being recorded is left to the
//               region frame (RegionTool), which already draws it.
//   Arrange  -- editing the layout. The screen dims a little and the areas are
//               cut out of the dim. Click an empty spot to stamp another area
//               (same size, centred on the click), drag any area to move it,
//               right-click an area -- or hover it and press Delete -- to remove
//               it. Area 1 is the region itself: it moves but is never removed.
//               Enter, Esc or the Done button finishes.
//
// Coordinates: areas are device pixels from the screen's top-left (the space
// CaptureRegion uses); the widget covers the screen, so local = device / dpr.
// Kept out of the recording on Windows, like the region frame.
class MultiAreaOverlay : public QWidget {
	Q_OBJECT
public:
	enum class Mode { Passive, Arrange };

	explicit MultiAreaOverlay(QWidget *parent = nullptr);

	// Cover `screen`. Call before showing.
	void setScreen(QScreen *screen);
	// The layout: every area `size` big, area 1 at `tops[0]`.
	void setAreas(const QVector<QPoint> &tops, QSize size);
	QVector<QPoint> areas() const { return tops_; }
	QSize areaSize() const { return size_; }
	// How it is drawn (Developer Panel → Areas).
	void setStyle(const AreaStyle &style);
	const AreaStyle &style() const { return style_; }
	// The area being recorded -- not outlined in Passive (the frame is there).
	void setActive(int index);
	// Dimmer while recording, so it reads as a guide, not as part of the shot.
	void setRecording(bool on);

	void setMode(Mode m);
	Mode mode() const { return mode_; }

	// For tests: the same geometry the mouse handlers use.
	int areaAtLocal(const QPoint &local) const;
	QRect areaRectLocal(int i) const;
	QRect doneRectLocal() const;
	// Stamp a new area centred on this local point, if there is room for one.
	// Returns its index, or -1 at the limit.
	int addAtLocal(const QPoint &local);
	void removeArea(int i);

signals:
	// The layout changed in Arrange (added, moved or removed an area).
	void areasEdited(const QVector<QPoint> &tops);
	// Arrange is over (Done, Enter or Esc).
	void arrangeFinished();

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void keyPressEvent(QKeyEvent *) override;
	void leaveEvent(QEvent *) override;
	void showEvent(QShowEvent *) override;

private:
	QPoint toDevice(const QPointF &local) const;
	QSize screenDevice() const;
	void applyInputFlags();

	QScreen *screen_ = nullptr;
	qreal dpr_ = 1.0;
	QVector<QPoint> tops_; // device px
	QSize size_;           // device px
	int active_ = 0;
	bool recording_ = false;
	Mode mode_ = Mode::Passive;
	AreaStyle style_;

	int hover_ = -1;       // area under the pointer (Arrange)
	QPoint pointer_;       // local, for the stamp ghost
	bool pointerIn_ = false;
	int drag_ = -1;        // area being dragged
	QPoint dragOffset_;    // device px, pointer minus the area's top-left
	bool dragMoved_ = false;
};

} // namespace harpia
