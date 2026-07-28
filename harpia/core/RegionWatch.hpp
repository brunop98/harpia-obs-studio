#pragma once

// "Stop recording once the pointer has been outside the region for N seconds."
//
// Only meaningful in Region mode: in full-monitor mode the pointer is always
// inside what is being recorded, so there is nothing to leave.
//
// The whole thing lives here rather than inline in MainWindow for one reason —
// the part most likely to be wrong is the coordinate conversion, and MainWindow
// cannot be constructed without libobs and a display. A CaptureRegion is in
// DEVICE pixels relative to its screen's top-left, while QCursor::pos() is in
// global LOGICAL pixels; getting that backwards on a HiDPI screen, or forgetting
// the screen origin on a second monitor, would stop recordings at apparently
// random moments. Both are pure functions here, and regionwatch_test pins them.

#include "CaptureManager.hpp" // CaptureRegion

#include <QPoint>
#include <QtGlobal>

namespace harpia {

// The dropdown's "Off". Not 0 — 0 is a real choice meaning "the moment it
// leaves", so the two cannot share a sentinel the way idleTimeoutSeconds does.
inline constexpr int kRegionWatchOff = -1;

// The values the dropdown offers, in order, after "Off".
inline constexpr int kRegionWatchSeconds[] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20};

class RegionWatch {
public:
	// A global logical cursor position, expressed the way CaptureRegion is:
	// device pixels from the top-left of the screen the region lives on.
	static QPoint toRegionSpace(QPoint globalLogical, QPoint screenOriginLogical, double dpr);

	// Half-open on the far edges, matching how the crop is applied: a pixel at
	// x + width is the first one NOT recorded.
	static bool contains(const CaptureRegion &region, QPoint devicePx);

	// Feed this once per timer tick. Returns true exactly once, on the tick that
	// the pointer has been continuously outside for `timeoutSeconds` — the
	// caller then stops the recording. Any tick that is not a live, armed,
	// region-mode recording clears the timer, so returning to the region (or
	// pausing, or switching mode) starts the count from zero next time.
	bool tick(bool armed, bool cursorInside, qint64 nowMs);

	// Forget any accumulated time. Call when a recording starts or stops so one
	// recording's wandering cannot end the next.
	void reset() { outsideSinceMs_ = -1; }

	// How long the pointer has been away, or -1 if it is inside. For the UI.
	qint64 outsideForMs(qint64 nowMs) const
	{
		return outsideSinceMs_ < 0 ? -1 : nowMs - outsideSinceMs_;
	}

	void setTimeoutSeconds(int s) { timeoutSeconds_ = s; }
	int timeoutSeconds() const { return timeoutSeconds_; }
	bool enabled() const { return timeoutSeconds_ >= 0; }

private:
	int timeoutSeconds_ = kRegionWatchOff;
	qint64 outsideSinceMs_ = -1;
	bool fired_ = false;
};

} // namespace harpia
