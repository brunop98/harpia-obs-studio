#pragma once

#include <QColor>
#include <QWidget>

class QScreen;

namespace harpia {

// A thin colored border drawn around the perimeter of a monitor to show it's
// being recorded. The window is frameless, click-through, and always-on-top; on
// Windows it sets WDA_EXCLUDEFROMCAPTURE so it is visible on screen but NOT
// included in the screen capture (so it never appears in the recording).
class ScreenBorderOverlay : public QWidget {
	Q_OBJECT
public:
	explicit ScreenBorderOverlay(QWidget *parent = nullptr);

	// Show the border around `screen` (primary if null) with the given color and
	// thickness (px). Safe to call repeatedly to update color/size/screen.
	void showBorder(QScreen *screen, const QColor &color, int thickness);

	// Update just the color while shown (e.g. idle green -> recording red).
	void setColor(const QColor &color);

	void hideBorder();

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	void applyGeometryAndMask(QScreen *screen);
	void excludeFromCapture(); // Windows: WDA_EXCLUDEFROMCAPTURE

	QColor color_{0xe5, 0x48, 0x4d};
	int thickness_ = 4;
};

} // namespace harpia
