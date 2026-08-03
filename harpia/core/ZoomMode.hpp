#pragma once

// Press a key, the recording zooms in and follows the mouse; press it again,
// it zooms back out.
//
// This is a SCENE TRANSFORM, not a crop, and the difference is the whole
// feature. The first version cropped the capture source to a smaller rectangle
// and assumed libobs would scale it back up to fill the canvas. It does not:
// a source bound straight to an output channel is drawn at its own size at the
// top-left corner, so a 2x "zoom" produced a quarter-size picture in the corner
// of an otherwise black frame. Scaling only happens for a source inside a
// SCENE, whose item carries a transform.
//
// So the capture source now lives in a scene, and zooming means giving that
// scene item a scale and a position:
//
//   * SCALE is the magnification. The item grows past the edges of the canvas
//     and the canvas clips it -- which is exactly what a zoom looks like, with
//     no black anywhere, because the item always covers the frame.
//   * POSITION decides which part of the picture the canvas is looking at. It
//     is driven by the same dead-zone chase Follow Mouse uses: the frame holds
//     still while the cursor stays near the middle and trails it past the
//     padding.
//   * Both are DOUBLES. The old crop stepped in whole pixels, so a slow push-in
//     visibly ratcheted; a float scale eases smoothly all the way in.
//
// Everything is clamped so the visible rectangle stays inside the picture: a
// zoom near a corner stops at the edge rather than showing black past it.
//
// Nothing here knows about capture modes -- it is given a canvas and a cursor
// and hands back a transform -- which is why the same code serves Full Screen
// and Custom Region.

#include "FollowMouse.hpp"

#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>

#include <algorithm>
#include <cmath>

namespace harpia {

struct ZoomParams {
	// 200 = 2x magnification: the frame shows half the width and half the
	// height, blown up to fill it. Clamped on the way in -- below ~110% there
	// is nothing to see, and above 400% a 1080p display has no real pixels
	// left to show.
	int percent = 200;
	// How long the push-in and the pull-out take, in ms. Distinct from the
	// follow speed below: how fast the camera moves in is a different decision
	// from how tightly it tracks the mouse once it is there.
	int animMs = 350;
	// The cursor chase.
	FollowParams follow;
	// Region recording with Follow Mouse on: the region is ALREADY keeping the
	// cursor framed, so a zoom that also chased it would track the same mouse
	// twice and the two would fight. The zoom then just magnifies about the
	// middle of the frame and lets Follow Mouse do the following.
	bool followCursor = true;

	static constexpr int kMinPercent = 110;
	static constexpr int kMaxPercent = 400;
	double factor() const
	{
		return std::clamp(percent, kMinPercent, kMaxPercent) / 100.0;
	}
};

// What to hand the scene item. `scale` multiplies its natural size; `posX/posY`
// place its top-left corner on the canvas, in canvas pixels (negative, because
// a magnified item hangs off the top-left).
struct ZoomTransform {
	double scale = 1.0;
	double posX = 0.0;
	double posY = 0.0;

	// No zoom at all -- the caller can skip touching the scene entirely.
	bool identity() const
	{
		return scale <= 1.0001 && std::abs(posX) < 0.01 && std::abs(posY) < 0.01;
	}

	// Which part of the source the canvas is actually showing, in SOURCE
	// pixels. This is what the on-screen border draws around, so it has to be
	// derived from the same numbers the recording uses rather than recomputed
	// alongside them -- otherwise the border and the file drift apart by a
	// pixel or two and nobody can tell which one is lying.
	QRect visibleRect(QSize canvas) const
	{
		if (canvas.isEmpty() || scale <= 0.0)
			return QRect(QPoint(0, 0), canvas);
		const double w = canvas.width() / scale;
		const double h = canvas.height() / scale;
		return QRect(int(std::lround(-posX / scale)), int(std::lround(-posY / scale)),
			     int(std::lround(w)), int(std::lround(h)));
	}
};

class ZoomMode {
public:
	// The canvas being recorded, in device pixels -- which is also the scene
	// item's natural size, since the item is the capture at 1:1. Resets the
	// state: a new recording starts unzoomed.
	void setCanvas(QSize canvasDevicePx)
	{
		canvas_ = canvasDevicePx;
		active_ = false;
		progress_ = 0.0;
		lastMs_ = -1;
		haveFocus_ = false;
		xf_ = ZoomTransform{};
	}
	QSize canvas() const { return canvas_; }

	// The shortcut. Returns the new state so the caller can log it, badge it
	// and drop a marker in the recording.
	bool toggle()
	{
		active_ = !active_;
		return active_;
	}
	bool isZoomed() const { return active_; }
	// Anything to apply? True while zoomed AND while easing back out, so the
	// caller keeps ticking until the frame is genuinely back to full size.
	bool isBusy() const { return active_ || progress_ > 0.0001; }

	// One step. Returns true when the transform changed and should be pushed to
	// the scene. nowMs is an injected monotonic clock.
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

		const ZoomTransform before = xf_;

		if (progress_ <= 0.0001) {
			haveFocus_ = false;
			xf_ = ZoomTransform{};
			return !before.identity();
		}

		// Smoothstepped, so the push-in does not arrive and stop dead.
		const double s = 1.0 + (params.factor() - 1.0) * easeSmooth(progress_);
		const double vw = canvas_.width() / s;  // visible size, source px
		const double vh = canvas_.height() / s;

		// The focus point: which source pixel sits at the centre of the frame.
		// Kept in doubles so a slow glide never stalls half a pixel short the
		// way the old integer crop did.
		if (!haveFocus_) {
			// First frame of a zoom: push in on what is being pointed at.
			focus_ = QPointF(cursorDevicePx.x(), cursorDevicePx.y());
			haveFocus_ = true;
			target_ = focus_;
		}

		QPointF target = target_;
		if (params.followCursor) {
			// The dead zone, in source pixels around the TARGET (not around
			// the smoothed position -- measured against the smoothed one the
			// goal drifts with every step and the chase creeps forever; that
			// bug is written up in FollowMouse.cpp).
			const int pad = std::clamp(params.follow.paddingPct, 0,
						   FollowMouse::kMaxPaddingPct);
			const double insetX = vw * pad / 100.0;
			const double insetY = vh * pad / 100.0;
			const double left = target.x() - vw / 2 + insetX;
			const double right = target.x() + vw / 2 - insetX;
			const double top = target.y() - vh / 2 + insetY;
			const double bottom = target.y() + vh / 2 - insetY;
			if (params.follow.axis != FollowAxis::Vertical) {
				if (cursorDevicePx.x() < left)
					target.setX(target.x() + (cursorDevicePx.x() - left));
				else if (cursorDevicePx.x() > right)
					target.setX(target.x() + (cursorDevicePx.x() - right));
			}
			if (params.follow.axis != FollowAxis::Horizontal) {
				if (cursorDevicePx.y() < top)
					target.setY(target.y() + (cursorDevicePx.y() - top));
				else if (cursorDevicePx.y() > bottom)
					target.setY(target.y() + (cursorDevicePx.y() - bottom));
			}
		}
		// Keep the visible rectangle inside the picture. Clamped at the TARGET
		// rather than after the step, so pressure against an edge does not
		// accumulate as an error the frame later pays back with a lurch.
		target = clampFocus(target, vw, vh);
		target_ = target;

		// The chase itself.
		const double a = FollowMouse::stepAlpha(params.follow.smoothness, dtMs);
		focus_ += (target_ - focus_) * a;
		if (std::abs(target_.x() - focus_.x()) < 0.02 &&
		    std::abs(target_.y() - focus_.y()) < 0.02)
			focus_ = target_;
		// Re-clamped after the step too: the size shrinks during the push-in,
		// so a focus that was legal last frame can be out of bounds this one.
		focus_ = clampFocus(focus_, vw, vh);

		xf_.scale = s;
		xf_.posX = canvas_.width() / 2.0 - focus_.x() * s;
		xf_.posY = canvas_.height() / 2.0 - focus_.y() * s;

		return changed(before, xf_);
	}

	ZoomTransform transform() const { return xf_; }
	// The part of the source on screen, for the border overlay.
	QRect visibleRect() const { return xf_.visibleRect(canvas_); }

private:
	// A focus point whose visible rectangle fits inside the canvas. When the
	// magnification is barely above 1 the rectangle is nearly the whole
	// picture, so the legal range collapses to the centre -- max() rather than
	// an assert, because that is a legitimate state on the way in and out.
	QPointF clampFocus(QPointF f, double vw, double vh) const
	{
		const double loX = vw / 2.0, hiX = canvas_.width() - vw / 2.0;
		const double loY = vh / 2.0, hiY = canvas_.height() - vh / 2.0;
		return QPointF(hiX < loX ? canvas_.width() / 2.0 : std::clamp(f.x(), loX, hiX),
			       hiY < loY ? canvas_.height() / 2.0 : std::clamp(f.y(), loY, hiY));
	}

	// Worth pushing to the scene? Sub-pixel changes are invisible and the
	// update crosses to the graphics thread, so there is no point sending one
	// per frame while the picture is parked.
	static bool changed(const ZoomTransform &a, const ZoomTransform &b)
	{
		return std::abs(a.scale - b.scale) > 1e-4 || std::abs(a.posX - b.posX) > 0.05 ||
		       std::abs(a.posY - b.posY) > 0.05;
	}

	// The same smoothstep the rest of the app eases with, so a zoom feels like
	// the other animations rather than like a separate mechanism.
	static double easeSmooth(double u)
	{
		const double t = std::clamp(u, 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	QSize canvas_;
	ZoomTransform xf_;
	QPointF focus_;
	QPointF target_;
	bool haveFocus_ = false;
	bool active_ = false;
	double progress_ = 0.0;
	qint64 lastMs_ = -1;
};

} // namespace harpia
