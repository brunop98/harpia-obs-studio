#pragma once

// Stop the scroll wheel from changing values in a scrolling panel.
//
// Qt's spin boxes, sliders and combo boxes all take the wheel as input. Inside
// a scrollable Inspector that is a trap rather than a feature: the pointer
// happens to be over a control while you scroll the panel, and you have
// silently changed a number you were not even looking at. The wheel is how you
// move around the panel; it should never also be how you edit it.
//
// So the wheel over such a control scrolls the panel, exactly as it does over
// the empty space beside it. Values are changed by dragging, typing, or the
// spin box's own arrows -- all of which say what they are doing.
//
// Installed on the application rather than on each control, because the panel
// rebuilds its rows as the selection changes and anything installed per widget
// would have to be re-installed every time (and would be forgotten the first
// time somebody adds a control). The filter only ever looks at wheel events,
// and only walks a parent chain for the ones landing on a value control, so the
// cost is nothing on every other event in the program.
//
// Scoped to one root: the preview's wheel-to-zoom and the timeline's
// wheel-to-scrub are untouched, because they are not inside it.

#include <QObject>
#include <QPointer>

class QWidget;

namespace harpia {

class WheelGuard : public QObject {
	Q_OBJECT
public:
	// `root` is the subtree to guard -- normally a QScrollArea. `parent` owns
	// the guard; the filter installs itself on the application.
	WheelGuard(QWidget *root, QObject *parent = nullptr);

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	QPointer<QWidget> root_;
};

} // namespace harpia
