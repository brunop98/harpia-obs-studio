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

	void configure(bool showArea, const QColor &areaColor, int areaSize, bool showClicks,
		       const QColor &clickColor);

protected:
	void paintEvent(QPaintEvent *) override;

private:
	bool showArea_ = false;
	QColor areaColor_ = QColor(0xff, 0xd5, 0x4a);
	int areaSize_ = 60;
	bool showClicks_ = false;
	QColor clickColor_ = QColor(0x4a, 0x90, 0xe2);

	QTimer *timer_ = nullptr;
	int phase_ = 0; // drives the looping ripple animation
};

} // namespace harpia
