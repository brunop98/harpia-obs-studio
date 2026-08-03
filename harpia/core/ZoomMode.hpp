#pragma once

// Press a key, the recording zooms in and follows the mouse; press it again,
// it zooms back out.
//
// The mechanism is the one already under Follow Mouse, used the other way
// round. In Full Screen the encoder's canvas is fixed at the display's own
// resolution, so handing the crop filter a SMALLER rectangle makes libobs
// scale it back up to fill that canvas -- and that scaling IS the zoom. Which
// is exactly why the crop has to keep the canvas's aspect ratio: a rectangle
// of any other shape would be stretched to fit, and the zoom would distort.
//
// So a zoom is just an animated crop rectangle:
//   * its SIZE is the canvas divided by the magnification, eased between 1x
//     and the configured factor when the key is pressed;
//   * its POSITION follows the cursor through the same dead-zone chase
//     FollowMouse already implements -- the region holds still while the
//     cursor stays in the middle, and trails it past the padding;
//   * and it is clamped to the screen, so a zoom near a corner stops at the
//     edge rather than showing anything outside it.
//
// This lives apart from MainWindow, and takes the canvas rather than asking
// anything about capture modes, so enabling it for Region recording later is
// a matter of handing it a different rectangle -- not of rewriting it.

#include "CaptureManager.hpp"
#include "FollowMouse.hpp"

#include <QPoint>
#include <QSize>

#include <algorithm>
#include <cmath>

namespace harpia {

struct ZoomParams {
	// 200 = 2x magnification: the captured rectangle is half the width and
	// half the height, blown back up to fill the frame. Clamped on the way in
	// -- below ~110% there is nothing to see, and above 400% a 1080p display
	// has no real pixels left to show.
	int percent = 200;
	// How long the zoom itself takes to arrive or leave, in ms. Distinct from
	// the follow speed below: how fast the camera pushes in is a different
	// decision from how tightly it tracks the mouse once it is there.
	int animMs = 350;
	// The cursor chase, handed straight to FollowMouse.
	FollowParams follow;

	static constexpr int kMinPercent = 110;
	static constexpr int kMaxPercent = 400;
	double factor() const
	{
		return std::clamp(percent, kMinPercent, kMaxPercent) / 100.0;
	}
};

// The zoomed crop size for a canvas: the canvas divided by the magnification,
// keeping the canvas's aspect EXACTLY (both axes divided by the same number),
// which is what makes the scale-back-up a clean zoom rather than a stretch.
// Never larger than the canvas, and never degenerate.
inline QSize zoomedSize(QSize canvas, double factor)
{
	if (canvas.isEmpty())
		return canvas;
	const double f = std::max(1.0, factor);
	QSize s(int(std::lround(canvas.width() / f)), int(std::lround(canvas.height() / f)));
	s.setWidth(std::clamp(s.width(), 16, canvas.width()));
	s.setHeight(std::clamp(s.height(), 16, canvas.height()));
	return s;
}

// A zoom that is on, off, or part-way between. `p` is 0 at full screen and 1
// at full magnification; the size is interpolated so the push-in is smooth.
inline QSize sizeForProgress(QSize canvas, double factor, double p)
{
	const QSize target = zoomedSize(canvas, factor);
	const double u = std::clamp(p, 0.0, 1.0);
	return QSize(int(std::lround(canvas.width() + (target.width() - canvas.width()) * u)),
		     int(std::lround(canvas.height() + (target.height() - canvas.height()) * u)));
}

class ZoomMode {
public:
	// The canvas being recorded, in device pixels. Also resets the state: a
	// new recording starts unzoomed.
	void setCanvas(QSize canvasDevicePx)
	{
		canvas_ = canvasDevicePx;
		active_ = false;
		progress_ = 0.0;
		lastMs_ = -1;
		follow_.disarm();
		// The crop itself has to go too, not just the animation state. Left
		// behind, region() keeps handing out the PREVIOUS recording's zoomed
		// rectangle until the first tick clears it -- and a caller that reads
		// the region while arming the pipeline would start the new file
		// already cropped.
		cropped_ = false;
		region_ = CaptureRegion{};
	}

	// The shortcut. Returns the new state so the caller can log it, badge it
	// and drop a marker in the recording.
	bool toggle()
	{
		active_ = !active_;
		return active_;
	}
	bool isZoomed() const { return active_; }
	// Anything to apply? True while zoomed AND while easing back out, so the
	// caller keeps ticking until the frame is genuinely back to full screen.
	bool isBusy() const { return active_ || progress_ > 0.0001; }

	// One step. Returns true when the crop rectangle changed and should be
	// pushed to the capture. nowMs is an injected monotonic clock.
	bool tick(QPoint cursorDevicePx, const ZoomParams &params, qint64 nowMs)
	{
		if (canvas_.isEmpty())
			return false;

		double dtMs = 16.0;
		if (lastMs_ >= 0 && nowMs > lastMs_)
			dtMs = double(nowMs - lastMs_);
		lastMs_ = nowMs;

		// Ease the magnification toward wherever the toggle put it. Linear in
		// time over animMs, which is what "animation speed" means to someone
		// setting it -- an exponential chase would never quite arrive and the
		// number would not correspond to anything observable.
		const double step = params.animMs > 0 ? dtMs / double(params.animMs) : 1.0;
		const double want = active_ ? 1.0 : 0.0;
		if (progress_ < want)
			progress_ = std::min(want, progress_ + step);
		else if (progress_ > want)
			progress_ = std::max(want, progress_ - step);

		if (progress_ <= 0.0001) {
			// Fully out: no crop at all, so the recording is the whole screen
			// again rather than a crop that happens to be canvas-sized.
			follow_.disarm();
			const bool wasCropped = cropped_;
			cropped_ = false;
			region_ = CaptureRegion{};
			return wasCropped;
		}

		const QSize want_ = sizeForProgress(canvas_, params.factor(), easeSmooth(progress_));

		// The follow chase owns the POSITION. Re-armed whenever the size
		// changes, because FollowMouse holds the size it was armed with -- and
		// during the push-in it changes every frame.
		CaptureRegion seed = region_;
		if (!follow_.armed() || seed.width != want_.width() || seed.height != want_.height()) {
			CaptureRegion r;
			r.enabled = true;
			r.width = want_.width();
			r.height = want_.height();
			// Grow/shrink about the CURSOR, not the screen centre, so pushing
			// in lands on what is being pointed at.
			r.x = cursorDevicePx.x() - want_.width() / 2;
			r.y = cursorDevicePx.y() - want_.height() / 2;
			if (region_.enabled && region_.width > 0) {
				// Mid-animation: keep the centre we already had, so the
				// push-in does not jump sideways when the size ticks.
				r.x = region_.x + region_.width / 2 - want_.width() / 2;
				r.y = region_.y + region_.height / 2 - want_.height() / 2;
			}
			const QPoint tl = FollowMouse::clampTopLeft(QPoint(r.x, r.y), want_, canvas_);
			r.x = tl.x();
			r.y = tl.y();
			follow_.arm(r);
			region_ = r;
			cropped_ = true;
			follow_.tick(cursorDevicePx, canvas_, params.follow, nowMs);
			region_ = follow_.region();
			return true;
		}

		const bool moved = follow_.tick(cursorDevicePx, canvas_, params.follow, nowMs);
		region_ = follow_.region();
		cropped_ = true;
		return moved;
	}

	// The crop to hand the capture. Disabled (no crop) when fully zoomed out.
	CaptureRegion region() const { return region_; }

private:
	// The same smoothstep the rest of the app eases with, so a zoom feels like
	// the other animations rather than like a separate mechanism.
	static double easeSmooth(double u)
	{
		const double t = std::clamp(u, 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	QSize canvas_;
	FollowMouse follow_;
	CaptureRegion region_;
	bool active_ = false;
	bool cropped_ = false;
	double progress_ = 0.0;
	qint64 lastMs_ = -1;
};

} // namespace harpia
