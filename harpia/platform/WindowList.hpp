#pragma once

// The desktop's top-level windows, for setting the recording area to one.
//
// Everything is in PHYSICAL pixels of the virtual desktop -- the space the
// capture's crop is measured in -- not Qt's scaled coordinates. On Windows the
// bounds are what DWM draws (DWMWA_EXTENDED_FRAME_BOUNDS): the window as you
// see it, title bar included, WITHOUT the invisible resize border and the drop
// shadow that GetWindowRect adds (about 7 px a side on Windows 10/11, which is
// why a naive "record this window" comes out slightly too big).
//
// Only windows a person would call "a window" are listed: visible, not
// minimised, not cloaked (a UWP app on another virtual desktop, a suspended
// Store app), not a tool window or popup with no owner, and not the desktop or
// the taskbar.
//
// Implemented per platform (platform/win, platform/nix, platform/mac); where
// there is nothing to ask, the list is empty and the feature hides itself.

#include <QRect>
#include <QString>
#include <QVector>

#include <optional>

namespace harpia {

struct DesktopWindow {
	quintptr id = 0;    // the native handle (HWND on Windows)
	QRect bounds;       // physical px, virtual desktop
	QString title;
	bool ours = false;  // belongs to this process (our own overlays and windows)
};

namespace window_list {

// Whether this platform can list windows at all.
bool available();

// Every listed window, TOPMOST FIRST (z-order), so the first one under a
// point is the one you see there.
QVector<DesktopWindow> windows();

// One window's current bounds; nullopt once it is closed, hidden or minimised.
std::optional<QRect> boundsOf(quintptr id);

// A monitor's rectangle in physical px, by the name Qt gives its screen
// (QScreen::name(), "\\.\DISPLAY1" on Windows). Null when unknown.
QRect monitorRect(const QString &screenName);

// The pointer, in physical px.
std::optional<QPoint> cursorPos();

} // namespace window_list

// ---- Pure helpers (no platform calls) --------------------------------------

namespace window_region {

// The topmost window under `p`, skipping our own; -1 if none.
inline int windowAt(const QVector<DesktopWindow> &topmostFirst, const QPoint &p)
{
	for (int i = 0; i < topmostFirst.size(); ++i)
		if (!topmostFirst[i].ours && topmostFirst[i].bounds.contains(p))
			return i;
	return -1;
}

// The monitor a window is on: the one it overlaps most (a window straddling
// two is recorded on the one holding most of it). -1 if it is on none.
inline int monitorFor(const QRect &window, const QVector<QRect> &monitors)
{
	int best = -1;
	qint64 bestArea = 0;
	for (int i = 0; i < monitors.size(); ++i) {
		const QRect o = window.intersected(monitors[i]);
		const qint64 a = qint64(o.width()) * o.height();
		if (a > bestArea) {
			bestArea = a;
			best = i;
		}
	}
	return best;
}

// The recording area for a window: its bounds clipped to the monitor, made
// relative to the monitor's top-left (the crop's space), with an even width
// and height (the encoders' 4:2:0 needs them). nullopt when too little of it
// is on that monitor to record (under 16 px either way).
inline std::optional<QRect> regionForWindow(const QRect &window, const QRect &monitor)
{
	QRect r = window.intersected(monitor);
	if (r.width() < 16 || r.height() < 16)
		return std::nullopt;
	r.translate(-monitor.topLeft());
	r.setWidth(r.width() & ~1);
	r.setHeight(r.height() & ~1);
	return r;
}

} // namespace window_region

} // namespace harpia
