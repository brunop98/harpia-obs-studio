#pragma once

#include <QColor>
#include <QPoint>
#include <QPointF>
#include <QWidget>
#include <vector>

class QScreen;
class QTimer;

namespace harpia {

// A click-through, always-on-top, transparent desktop overlay that draws a
// highlight ring around the cursor and expanding "ripple" animations on mouse
// clicks. Because it lives on the desktop, the screen capture records it — no
// custom obs source needed. Active only while recording.
//
// Click detection is Windows-only for now (GetAsyncKeyState polling); on other
// platforms the cursor highlight still works but clicks are not detected.
class MouseFxOverlay : public QWidget {
	Q_OBJECT
public:
	struct Config {
		bool showArea = false;
		QColor areaColor = QColor(0xff, 0xd5, 0x4a);
		int areaSize = 60; // diameter in px
		bool showClicks = false;
		QColor leftColor = QColor(0x4a, 0x90, 0xe2);
		QColor rightColor = QColor(0xe2, 0x53, 0x4a);
	};

	explicit MouseFxOverlay(QWidget *parent = nullptr);

	void configure(const Config &cfg) { cfg_ = cfg; }
	void setScreen(QScreen *screen);

	void start(); // show + begin the animation timer
	void stop();  // hide + stop

protected:
	void paintEvent(QPaintEvent *) override;

private:
	void tick();

	struct Ripple {
		QPointF center;
		qint64 startMs;
		QColor color;
	};

	Config cfg_;
	QScreen *screen_ = nullptr;
	QTimer *timer_ = nullptr;
	QPoint cursorLocal_;
	std::vector<Ripple> ripples_;
	bool lastLeft_ = false;
	bool lastRight_ = false;
};

} // namespace harpia
