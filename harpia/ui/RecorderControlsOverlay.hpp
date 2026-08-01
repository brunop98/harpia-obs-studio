#pragma once

#include <QPoint>
#include <QWidget>

class QPropertyAnimation;
class QPushButton;

namespace harpia {

// A minimal, frameless, always-on-top floating panel shown on the desktop while
// recording, so Pause/Stop are reachable without switching back to the main
// window. It is:
//   - draggable anywhere (press the background and drag), across all monitors;
//   - dimmed while the mouse is away and smoothly fades to full opacity on hover;
//   - excluded from the screen capture on Windows (WDA_EXCLUDEFROMCAPTURE), so it
//     never appears in the recording;
//   - non-activating, so showing it doesn't steal focus from the app being used.
// Its last position is remembered across sessions.
class RecorderControlsOverlay : public QWidget {
	Q_OBJECT
public:
	explicit RecorderControlsOverlay(QWidget *parent = nullptr);

	// Show the panel (restoring its saved position) and keep it excluded from
	// capture. No-op if already visible.
	void showControls();
	void hideControls();

	// Reflect the recorder state. Idle shows a single green Record button and
	// nothing else; recording swaps it for Pause and Stop. Hiding rather than
	// disabling, so the pill is one button wide when there is only one thing to
	// do -- and there are never two dead buttons sitting on the desktop.
	void setState(bool recording, bool paused, bool pauseEnabled, bool stopEnabled);

signals:
	void startClicked();
	void pauseClicked();
	void stopClicked();
	// "Hide until next recording" from the right-click menu. The panel is on
	// screen whenever the app is, so it needs a way to be got rid of that does
	// not mean quitting.
	void dismissed();

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void contextMenuEvent(QContextMenuEvent *) override;
	void enterEvent(QEnterEvent *) override;
	void leaveEvent(QEvent *) override;
	bool eventFilter(QObject *obj, QEvent *event) override;

private:
	void updateHover();            // fade based on whether the cursor is over the panel
	void animateOpacity(qreal to); // smooth windowOpacity transition
	void restorePosition();        // last saved pos, clamped onto a visible screen
	void savePosition();
	void excludeFromCapture(); // Windows: WDA_EXCLUDEFROMCAPTURE

	QPushButton *startButton_ = nullptr;
	QPushButton *pauseButton_ = nullptr;
	QPushButton *stopButton_ = nullptr;
	QPropertyAnimation *fade_ = nullptr;
	QPoint dragOffset_;
	bool dragging_ = false;
	bool paused_ = false;
	bool recording_ = false;
	bool stateApplied_ = false; // setState has run at least once
};

} // namespace harpia
