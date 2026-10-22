#pragma once

// Where Windows really has one of our own windows, in physical pixels.
//
// Qt places a window from its own scaled coordinates, and across displays
// with different scales -- a second monitor at 125% to the left of a main
// one at 100%, say -- where Windows puts a window and where Qt works out it
// is can disagree. Two overlays cannot afford that: the region frame, whose
// crop has to be what is visibly inside it, and the draw layer, whose dragged
// rectangle has to be what was under the pointer. Both check here.
//
// Windows only. Elsewhere there is nothing to ask, the answers are empty and
// the overlays keep Qt's own arithmetic. Replaceable, so a test can play a
// Windows that put the window somewhere other than where it was asked.

#include <QPoint>
#include <QRect>

#include <functional>
#include <optional>

class QWidget;

namespace harpia {

namespace native_window {

// The window's rectangle in physical px of the virtual desktop; nullopt when
// it has no native window yet (asking never creates one), or the platform
// cannot say.
std::optional<QRect> rect(const QWidget *w);

// Move the window so its top-left is at `topLeft` (physical px), keeping its
// size. False when it could not.
bool moveTo(QWidget *w, const QPoint &topLeft);

struct Ops {
	std::function<std::optional<QRect>(const QWidget *)> rect;
	std::function<bool(QWidget *, const QPoint &)> moveTo;
};
// Stand in for the platform (tests). Empty functions put the platform back.
void setOpsForTest(Ops ops);

} // namespace native_window

} // namespace harpia
