#pragma once

// Spotlight: press a key and the screen goes dark except for a patch around the
// cursor, so a viewer's eye lands where you are pointing. Press it again and
// the dark lifts.
//
// It is drawn by the desktop overlay that already carries the cursor highlight
// and the click ripples (MouseFxOverlay), for the same reason: an overlay on
// the desktop is IN the screen capture, so nothing custom is needed on the
// libobs side and what you see is exactly what lands in the file. The cost is
// that your own screen really does go dark -- which is also what makes it
// usable, since you can see what the viewer will see while you are recording.
//
// Everything here is pure: the geometry, the shading and the fade are functions
// of their arguments with an injected clock, so they can be tested without a
// window and reused by the settings preview without duplicating the maths. The
// preview and the recording therefore cannot drift apart.

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace harpia {

struct SpotlightParams {
	// The lit patch, in device pixels across. A diameter, matching how the
	// cursor highlight's size is expressed on the same settings page.
	int sizePx = 320;
	// How dark everything else goes, 0..100. Not allowed all the way to 100:
	// a fully black screen with a hole in it makes it impossible to find the
	// window you are about to click, and you are still driving the machine.
	int darkPct = 70;
	// 0 = a hard-edged rectangle, 100 = as round as the patch can be (a circle
	// when the patch is square). In between, a rounded rectangle.
	int roundnessPct = 100;

	static constexpr int kMinSize = 80;
	static constexpr int kMaxSize = 1600;
	static constexpr int kMaxDark = 95;
};

// The lit patch for a cursor position. Not clamped to the screen: walking the
// cursor into a corner should slide the patch off the edge with it, not park it
// against the edge and make the cursor drift out of its own spotlight.
inline QRect spotlightHole(QPoint cursor, int sizePx)
{
	const int s = std::clamp(sizePx, SpotlightParams::kMinSize, SpotlightParams::kMaxSize);
	return QRect(cursor.x() - s / 2, cursor.y() - s / 2, s, s);
}

// The corner radius for a patch of this shape. 100% of a square patch is half
// its side, which is exactly a circle; anything less rounds the corners off a
// rectangle by that fraction.
inline int spotlightRadius(QRect hole, int roundnessPct)
{
	const int pct = std::clamp(roundnessPct, 0, 100);
	const int maxR = std::min(hole.width(), hole.height()) / 2;
	return maxR * pct / 100;
}

// The alpha of the darkening, 0..255, at a given fade progress. Progress 0 is
// fully lifted (nothing drawn at all) and 1 is the configured darkness.
inline int spotlightAlpha(int darkPct, double progress)
{
	const int pct = std::clamp(darkPct, 0, SpotlightParams::kMaxDark);
	const double p = std::clamp(progress, 0.0, 1.0);
	return int(std::lround(255.0 * (pct / 100.0) * p));
}

// The screen area a repaint has to cover when the cursor moves from `from` to
// `to` with the spotlight settled. Only the two patches change -- everything
// else was dark before and is dark after -- so a 60 Hz spotlight does not have
// to repaint the whole display on every mouse move. The margin covers the
// antialiased edge.
inline QRect spotlightDirty(QPoint from, QPoint to, int sizePx, int marginPx = 4)
{
	return spotlightHole(from, sizePx).united(spotlightHole(to, sizePx)).adjusted(-marginPx, -marginPx,
										      marginPx, marginPx);
}

// The toggle, its fade, and whether anything needs drawing. Same shape as
// ZoomMode: a shortcut flips it, a clock drives the animation, and the caller
// keeps ticking until it says it is done.
class SpotlightMode {
public:
	// A fade rather than a cut. Going from a clear screen to a dark one in a
	// single frame is startling to watch and worse to record; 180 ms reads as
	// a light coming up.
	static constexpr int kFadeMs = 180;

	// Reset for a new recording. `startOn` is the preset's choice of whether a
	// recording opens with the spotlight already lit.
	void reset(bool startOn = false)
	{
		on_ = startOn;
		progress_ = startOn ? 1.0 : 0.0;
		lastMs_ = -1;
	}

	bool toggle()
	{
		on_ = !on_;
		return on_;
	}
	bool isOn() const { return on_; }
	// Is there anything to draw? True while lit AND while fading back out, so
	// the caller does not stop ticking and leave the screen half dark.
	bool isBusy() const { return on_ || progress_ > 0.0005; }
	double progress() const { return progress_; }

	// Advance the fade. Returns true when the shading changed and the whole
	// overlay needs repainting; false means the fade is settled and a cursor
	// move only needs the two patches (see spotlightDirty).
	bool tick(qint64 nowMs)
	{
		double dtMs = 16.0;
		if (lastMs_ >= 0 && nowMs > lastMs_)
			dtMs = double(nowMs - lastMs_);
		lastMs_ = nowMs;

		const double want = on_ ? 1.0 : 0.0;
		if (std::abs(progress_ - want) < 1e-9)
			return false;
		const double step = dtMs / double(kFadeMs);
		progress_ = progress_ < want ? std::min(want, progress_ + step)
					     : std::max(want, progress_ - step);
		return true;
	}

	// Eased for painting. Linear in time so the configured duration means
	// something, smoothstepped on the way to the screen so the light does not
	// arrive and stop with a jolt.
	double shade() const
	{
		const double t = std::clamp(progress_, 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

private:
	bool on_ = false;
	double progress_ = 0.0;
	qint64 lastMs_ = -1;
};

} // namespace harpia
