#pragma once

#include "core/ZoomMode.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QWidget>

class QTimer;

namespace harpia {

// A live preview of Automatic Zoom for the preset editor: a mock desktop that
// pushes in on a cursor and pulls back out, over and over.
//
// It matters more here than on the Spotlight page. Spotlight is three numbers
// about a shape, and a still picture says most of it. Zoom is a magnification,
// a push-in duration, a chase speed and a dead zone -- four settings whose whole
// character is how they behave over time, and none of which a number conveys.
// "350 ms" and "smoothness 40" mean nothing until you watch them.
//
// It runs the recorder's own ZoomMode, ticked at the same rate with the same
// parameters, so what is being tuned here and what lands in the recording
// cannot drift apart. The cursor follows the real mouse while it is over the
// widget, and walks a slow path of its own when it is not, so the preview
// demonstrates itself without being touched.
class ZoomPreview : public QWidget {
	Q_OBJECT
public:
	explicit ZoomPreview(QWidget *parent = nullptr);

	// Called whenever a slider on the Zoom page moves.
	void configure(const ZoomParams &params);

	// Stop the timer while the page is not visible: this repaints at 60 Hz and
	// there is no reason to do that behind another page.
	void setRunning(bool on);

protected:
	void paintEvent(QPaintEvent *) override;
	void resizeEvent(QResizeEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void leaveEvent(QEvent *) override;
	void showEvent(QShowEvent *) override;
	void hideEvent(QHideEvent *) override;

private:
	void tick();
	void rebuildMock();
	QPoint autoCursor(qint64 ms) const;

	ZoomMode zoom_;
	ZoomParams params_;
	QImage mock_;          // the fake desktop, rebuilt on resize
	QElapsedTimer clock_;  // drives both the animation and the auto-toggle
	QTimer *timer_ = nullptr;
	qint64 nextToggleMs_ = 0;
	QPoint cursor_;
	bool hovering_ = false;
};

} // namespace harpia
