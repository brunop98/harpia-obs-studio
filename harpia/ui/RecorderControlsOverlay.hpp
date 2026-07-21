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

	// Reflect the recorder state: Pause↔Resume label, and per-button enabled.
	void setState(bool paused, bool pauseEnabled, bool stopEnabled);

signals:
	void pauseClicked();
	void stopClicked();

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void enterEvent(QEnterEvent *) override;
	void leaveEvent(QEvent *) override;
	bool eventFilter(QObject *obj, QEvent *event) override;

private:
	void updateHover();            // fade based on whether the cursor is over the panel
	void animateOpacity(qreal to); // smooth windowOpacity transition
	void restorePosition();        // last saved pos, clamped onto a visible screen
	void savePosition();
	void excludeFromCapture(); // Windows: WDA_EXCLUDEFROMCAPTURE

	QPushButton *pauseButton_ = nullptr;
	QPushButton *stopButton_ = nullptr;
	QPropertyAnimation *fade_ = nullptr;
	QPoint dragOffset_;
	bool dragging_ = false;
	bool paused_ = false;
};

} // namespace harpia
