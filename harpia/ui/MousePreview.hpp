#pragma once

#include <QColor>
#include <QWidget>

class QTimer;

namespace harpia {

// A small animated preview of the mouse recording effects: a sample cursor with
// its highlight ring and a looping click ripple, using the currently-selected
// colors and size. Lets the user see the look before recording.
class MousePreview : public QWidget {
	Q_OBJECT
public:
	explicit MousePreview(QWidget *parent = nullptr);

	// The ripple loop alternates between the left- and right-click colors so
	// both choices are visible in the preview.
	void configure(bool showArea, const QColor &areaColor, int areaSize, bool showClicks,
		       const QColor &leftClickColor, const QColor &rightClickColor);

protected:
	void paintEvent(QPaintEvent *) override;

private:
	bool showArea_ = false;
	QColor areaColor_ = QColor(0xff, 0xd5, 0x4a);
	int areaSize_ = 60;
	bool showClicks_ = false;
	QColor leftClickColor_ = QColor(0x4a, 0x90, 0xe2);
	QColor rightClickColor_ = QColor(0xe2, 0x53, 0x4a);

	QTimer *timer_ = nullptr;
	int phase_ = 0;       // drives the looping ripple animation
	int rippleIndex_ = 0; // even = left color, odd = right color
};

} // namespace harpia
