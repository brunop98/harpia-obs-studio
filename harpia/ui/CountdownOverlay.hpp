#pragma once

#include <QWidget>

class QTimer;
class QScreen;

namespace harpia {

// A frameless, translucent, always-on-top overlay that shows a large countdown
// number centered on a screen before recording begins. It is dismissed (and the
// recording started) when the count reaches zero, or cancelled with Esc / click.
// Because it is only shown *before* recording starts, it never appears in the
// captured video.
class CountdownOverlay : public QWidget {
	Q_OBJECT
public:
	explicit CountdownOverlay(QWidget *parent = nullptr);

	// Begin a countdown of `seconds` over the given screen (or the primary
	// screen if null). Emits tick() each second, finished() at zero.
	void start(int seconds, QScreen *screen);

	// Hide and stop without emitting finished()/cancelled().
	void stop();

signals:
	void tick(int remaining);
	void finished();
	void cancelled();

protected:
	void paintEvent(QPaintEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;

private:
	void onTimeout();

	int remaining_ = 0;
	QTimer *timer_ = nullptr;
};

} // namespace harpia
