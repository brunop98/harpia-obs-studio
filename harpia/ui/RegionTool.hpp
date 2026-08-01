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

	// How the overlay behaves right now. Three states, not two, because
	// "visible" and "grabs the mouse" are different questions and conflating
	// them is why the overlay used to vanish the moment you clicked the app you
	// were trying to frame.
	// Which part of the frame a press landed on. Public only because
	// isInteracting() above is inline and needs the type.
	enum class Zone {
		None,
		Move,
		Left,
		Right,
		Top,
		Bottom,
		TopLeft,
		TopRight,
		BottomLeft,
		BottomRight,
		// The Record button below the frame. A zone rather than a child widget so
		// it lives in the same painted, masked surface as everything else here --
		// and so mouseMoveEvent's geometry switch can simply ignore it.
		StartButton,
	};

	enum class Mode {
		// Harpia is in front. The whole rectangle takes the mouse, so dragging
		// the interior moves the region -- the only state where that works.
		Editing,
		// Region capture is chosen but another app is in front. Still on
		// screen, so the frame can be seen and adjusted against the thing being
		// recorded; interior click-through, so that app stays usable.
		Watching,
		// Recording. Same input rules as Watching, dimmed further, and the
		// border turns red (or yellow when paused).
		Recording,
	};
	void setMode(Mode m);
	Mode mode() const { return mode_; }

	// The move handle: a small tab drawn just ABOVE the region's top edge.
	//
	// Dragging the interior only works in Editing, because the other two modes
	// mask the interior out so the app underneath stays clickable. That leaves
	// the region resizable but NOT movable in exactly the situation the overlay
	// exists for -- lining the frame up against the app you are about to record,
	// with that app in front -- and equally while recording. The tab lives
	// outside the region, so it can stay inside the input mask without covering a
	// single captured pixel.
	//
	// Off unless the active preset asks for it. Idempotent: the owner calls this
	// from a 250 ms tick.
	void setMoveHandleEnabled(bool on);
	bool moveHandleEnabled() const { return moveHandle_; }
	// Where the tab is, in local coordinates. Null when the handle is off.
	QRect moveHandleRect() const;

	// The green Record button below the frame.
	//
	// Starting a recording used to mean going back to the main window, which is
	// the one step of a region recording that pulled you away from the thing you
	// were framing. The button sits outside the region for the same reason the
	// move tab does: it covers no captured pixel, and it stays in the input mask
	// so it works with the app being recorded in front.
	//
	// Hidden while recording -- a green "start" beside a running recording would
	// be a lie, and Stop lives on the floating controls, which is where it
	// already was.
	// What the pointer should look like over a given zone. Static and pure so
	// the mapping can be checked without a screen -- and so the pointer is
	// always derived from the zone a press would actually use, rather than
	// worked out separately and left to drift out of step with it.
	static Qt::CursorShape cursorForZone(Zone z);
	// Public for the same reason: the "recording only moves" rule is a hit-test
	// rule, and it is worth being able to ask what a point does.
	Zone zoneAtForTest(const QPoint &localPos) const { return zoneAt(localPos); }

	bool startButtonVisible() const { return mode_ != Mode::Recording; }
	QRect startButtonRect() const; // local coords; null when not shown
	// True while a move or resize is under way. Callers must not change the
	// mode during one: switching to Watching re-masks the widget, and pulling
	// the interior out from under a drag that started there drops it halfway.
	bool isInteracting() const { return dragZone_ != Zone::None; }

	// Paused state — only meaningful in Recording mode; drives the border color
	// (yellow when paused, red while actively recording).
	void setPaused(bool paused);

signals:
	void regionChanged(const CaptureRegion &region);
	void cancelled();            // Esc pressed while not recording
	void startRecordingRequested(); // the Record button below the frame was pressed
	void saveRegionRequested();  // "Save Region…" chosen from the right-click menu
	void manageRegionsRequested(); // "Manage saved regions…" chosen
	// A move or resize finished. The owner defers mode changes while a drag is
	// in flight (see isInteracting), so it needs telling when to look again --
	// grabbing the overlay makes IT the active window, which usually means the
	// mode that was right before the drag is not the right one after it.
	void interactionFinished();

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void mouseDoubleClickEvent(QMouseEvent *) override;
	void leaveEvent(QEvent *) override;
	void showEvent(QShowEvent *) override;
	void contextMenuEvent(QContextMenuEvent *) override;
	void keyPressEvent(QKeyEvent *) override;

private:
	Zone zoneAt(const QPoint &localPos) const;
	void applyGeometry(const QRect &globalRect); // clamp to screen, set geometry, emit
	void rebuildMask();
	void emitRegion();
	QRect innerRectLocal() const; // the region rect within the widget (handle margin inset)
	// The widget's top inset. Larger than the side margin when the move handle is
	// on, since the tab sits above the frame. innerRectLocal() and applyGeometry()
	// both derive from this, which is what keeps switching the handle on from
	// moving the region.
	int topMargin() const;
	// The bottom inset, which always reserves room for the Record button.
	// Reserved even while recording, when the button is not drawn: recomputing
	// the widget's geometry every time setMode() flips would churn the window
	// and re-emit regionChanged() off a state tick, and the reserved band is
	// transparent, so leaving it there costs nothing.
	int bottomMargin() const;
	void snap(QRect &globalRect) const;
	// Windows: keep the frame off the recording. It sits on the boundary of the
	// captured rectangle, so without this it is baked into every frame.
	void excludeFromCapture();

	QScreen *screen_ = nullptr;
	qreal dpr_ = 1.0;
	Mode mode_ = Mode::Editing;
	bool paused_ = false;
	bool moveHandle_ = false;

	Zone dragZone_ = Zone::None;
	QPoint dragStartGlobal_;
	QRect dragStartGeom_;
	bool showDims_ = false;
	bool startPressed_ = false; // the Record button is held down
	bool startHover_ = false;
};

// What the overlay should be doing, given the three facts that decide it.
//
// A free function, and not a private branch inside MainWindow, because this is
// the whole policy: whether the region is on screen at all, and whether it
// takes the mouse. It got that wrong for a long time in a way no test could
// reach -- the overlay disappeared whenever focus left Harpia, so the region
// could only ever be adjusted against an empty desktop.
struct RegionOverlayState {
	bool visible = false;
	RegionTool::Mode mode = RegionTool::Mode::Editing;

	bool operator==(const RegionOverlayState &o) const
	{
		return visible == o.visible && mode == o.mode;
	}
};
RegionOverlayState regionOverlayState(bool regionCaptureMode, bool recording, bool harpiaFocused,
				      bool editorOpen);

} // namespace harpia
