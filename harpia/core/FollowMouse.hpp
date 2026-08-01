#pragma once

// Follow Mouse: while recording a Custom Region, the region pans to keep the
// cursor framed. This is for recording tutorials in a small (often vertical,
// mobile-shaped) region without editing afterwards to re-frame everything: the
// camera does the re-framing live.
//
// The design splits the same way RegionWatch does, and for the same reason:
// MainWindow cannot be constructed without libobs and a display, so everything
// that can go quietly wrong -- the dead-zone geometry, the target arithmetic,
// the smoothing -- lives here as pure functions plus a small stateful stepper,
// and is pinned by followmouse_test. MainWindow's tick is reduced to plumbing:
// read the cursor, call tick(), push the result to the crop filter and the
// overlay.
//
// Model:
//   * The DEAD ZONE is the region inset by a padding percentage per side.
//     While the cursor is inside it, nothing moves -- so ordinary mousing
//     around the middle of the frame does not swim the camera.
//   * When the cursor crosses the padding, the TARGET shifts by exactly the
//     amount that puts the cursor back on the dead-zone edge. The frame
//     therefore trails the cursor rather than gluing it to the centre.
//   * The actual position CHASES the target with an exponential step whose
//     time constant comes from the Smoothness setting. 0 is instant; higher
//     values are progressively more cinematic. The step is computed from real
//     elapsed time, so a late tick takes a proportionally bigger step and the
//     feel does not depend on timer jitter.
//   * The region's SIZE never changes. Only its position does -- the encoder
//     is committed to one frame size for the whole file (the same rule that
//     forbids resizing the region mid-recording).
//
// Coordinates: everything here is device pixels relative to the screen's
// top-left -- the space CaptureRegion and crop_filter use. Use
// RegionWatch::toRegionSpace for the cursor.

#include "CaptureManager.hpp"

#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>

namespace harpia {

// Which way the region is allowed to move. A vertical mobile strip usually
// wants Horizontal (it slides along a toolbar); a full-width bar over a page
// wants Vertical (it follows the scroll).
enum class FollowAxis {
	Both = 0,
	Horizontal = 1,
	Vertical = 2,
};

// Everything the stepper needs from the preset, gathered once per tick by the
// caller. Plain data so tests can build it inline.
struct FollowParams {
	int paddingPct = 20;                 // dead-zone inset per side, 0..45 (% of region size)
	int smoothness = 50;                 // 0 = instant .. 100 = most cinematic
	FollowAxis axis = FollowAxis::Both;
};

class FollowMouse {
public:
	// The slider bounds, stated once. 45 is the ceiling because at 50 the two
	// insets meet in the middle and the dead zone inverts.
	static constexpr int kMaxPaddingPct = 45;
	static constexpr int kMaxSmoothness = 100;

	// The region inset by paddingPct per side. Padding 0 means "the whole
	// region is safe" -- following only starts when the cursor actually leaves
	// the frame.
	static QRect deadZone(const CaptureRegion &region, int paddingPct);

	// How far the region must move so the cursor lands back on the dead-zone
	// edge. Zero when the cursor is already inside the zone. Pure geometry;
	// axis lock and clamping are applied by the caller (tick).
	static QPoint targetShift(const CaptureRegion &region, QPoint cursorDevicePx, int paddingPct);

	// Where a region top-left may actually be: clamped so the region stays on
	// the screen. screenDevicePx is the full screen size in device pixels.
	static QPoint clampTopLeft(QPoint topLeft, QSize regionSize, QSize screenDevicePx);

	// The fraction of the remaining distance covered in one step of dtMs, for
	// a given smoothness. 1.0 at smoothness 0 (instant); falls toward 0 as
	// smoothness rises. Exponential in dt, so two 8 ms steps land exactly
	// where one 16 ms step would -- the feel is frame-rate independent.
	static double stepAlpha(int smoothness, double dtMs);

	// ---- the stateful part ----

	// Start following from this region. Position state is seeded from it, so
	// the first tick chases from where the region actually is rather than
	// jumping from wherever the last recording left off.
	void arm(const CaptureRegion &region);
	void disarm();
	bool armed() const { return armed_; }

	// Re-seed the position without disarming -- for when something else moved
	// the region under us (the user dragging the frame mid-recording).
	void rebase(const CaptureRegion &region);

	// One step: chase the cursor. Returns true if the region MOVED, in which
	// case region() is the new rectangle to apply. nowMs is an injected clock
	// (monotonic ms); the first tick after arm() takes a nominal step.
	bool tick(QPoint cursorDevicePx, QSize screenDevicePx, const FollowParams &params, qint64 nowMs);

	// The region at its current (smoothed) position. Size is always the size
	// given to arm()/rebase().
	CaptureRegion region() const;

private:
	bool armed_ = false;
	CaptureRegion base_;   // size + enabled flag; x/y tracked in pos_
	QPointF pos_;          // smoothed top-left, kept fractional so slow chases
	                       // do not stall on integer rounding
	QPointF target_;       // where the top-left is headed
	qint64 lastMs_ = -1;
};

} // namespace harpia
