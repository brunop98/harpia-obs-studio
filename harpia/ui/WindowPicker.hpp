#pragma once

// "Pick a window": set the recording area to a window by pointing at it.
//
// A see-through layer over every screen. As the pointer moves, the window
// under it is outlined -- the window as you see it, no shadow -- with its
// name and size; a click picks it, Esc or a right-click cancels. Like ShareX
// or the Snipping Tool's window mode.
//
// The window list is taken once, when picking starts (window_list::windows(),
// topmost first): the layer covers the whole screen, so asking Windows what is
// under the pointer while it is up would only ever answer "the layer".
//
// Coordinates: the windows are in physical pixels of the virtual desktop; each
// layer knows its screen's physical rectangle and scale, which is all it takes
// to draw a window's outline where the window is.

#include "platform/WindowList.hpp"

#include <QObject>
#include <QPointer>
#include <QVector>

class QScreen;
class QWidget;

namespace harpia {

class WindowPickerLayer;

class WindowPicker : public QObject {
	Q_OBJECT
public:
	explicit WindowPicker(QObject *parent = nullptr);
	~WindowPicker() override;

	// Show the layers, with the platform's window list.
	void start();
	// The same with a given list and screens (physical rectangles): tests, and
	// any platform that has its own way to list windows.
	struct ScreenArea {
		QScreen *screen = nullptr;
		QRect physical;
	};
	void startWith(const QVector<DesktopWindow> &topmostFirst, const QVector<ScreenArea> &screens);

	bool active() const { return !layers_.isEmpty(); }
	void cancel();

	// The window under a point (physical px): what the layers outline.
	void hoverPhysical(const QPoint &p);
	void pickPhysical(const QPoint &p);
	int hovered() const { return hovered_; }
	const QVector<DesktopWindow> &windowsForTest() const { return windows_; }
	QVector<QWidget *> layersForTest() const;

signals:
	void picked(const harpia::DesktopWindow &window);
	void cancelled();

private:
	friend class WindowPickerLayer;
	void close();

	QVector<DesktopWindow> windows_;
	QVector<QPointer<WindowPickerLayer>> layers_;
	int hovered_ = -1;
};

} // namespace harpia
