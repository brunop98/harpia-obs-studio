#include "FollowMouse.hpp"

#include <algorithm>
#include <cmath>

namespace harpia {

QRect FollowMouse::deadZone(const CaptureRegion &region, int paddingPct)
{
	const int pct = std::clamp(paddingPct, 0, kMaxPaddingPct);
	const int insetX = region.width * pct / 100;
	const int insetY = region.height * pct / 100;
	return QRect(region.x + insetX, region.y + insetY, region.width - 2 * insetX,
		     region.height - 2 * insetY);
}

QPoint FollowMouse::targetShift(const CaptureRegion &region, QPoint cursorDevicePx, int paddingPct)
{
	const QRect zone = deadZone(region, paddingPct);
	QPoint shift(0, 0);
	// Move exactly far enough that the cursor sits on the zone's edge -- no
	// further. That is what makes the frame TRAIL the cursor instead of
	// snapping it to the centre, which reads as a camera operator rather than
	// a cursor-lock.
	//
	// Half-open on the far edges, same convention as RegionWatch::contains: a
	// zone right edge of x+w means the last inside pixel is x+w-1.
	if (cursorDevicePx.x() < zone.left())
		shift.setX(cursorDevicePx.x() - zone.left());
	else if (cursorDevicePx.x() >= zone.left() + zone.width())
		shift.setX(cursorDevicePx.x() - (zone.left() + zone.width() - 1));
	if (cursorDevicePx.y() < zone.top())
		shift.setY(cursorDevicePx.y() - zone.top());
	else if (cursorDevicePx.y() >= zone.top() + zone.height())
		shift.setY(cursorDevicePx.y() - (zone.top() + zone.height() - 1));
	return shift;
}

QPoint FollowMouse::clampTopLeft(QPoint topLeft, QSize regionSize, QSize screenDevicePx)
{
	const int maxX = std::max(0, screenDevicePx.width() - regionSize.width());
	const int maxY = std::max(0, screenDevicePx.height() - regionSize.height());
	return QPoint(std::clamp(topLeft.x(), 0, maxX), std::clamp(topLeft.y(), 0, maxY));
}

double FollowMouse::stepAlpha(int smoothness, double dtMs)
{
	const int s = std::clamp(smoothness, 0, kMaxSmoothness);
	if (s == 0)
		return 1.0; // instant: the whole remaining distance, every step
	// Time constant grows with the slider: ~8 ms at 1 (indistinguishable from
	// instant) up to ~800 ms at 100 (a long, deliberate glide). Exponential in
	// dt so composing two half-steps equals one full step -- the movement's
	// feel cannot depend on timer jitter.
	const double tauMs = 8.0 * s;
	return 1.0 - std::exp(-std::max(0.0, dtMs) / tauMs);
}

void FollowMouse::arm(const CaptureRegion &region)
{
	base_ = region;
	pos_ = QPointF(region.x, region.y);
	target_ = pos_;
	lastMs_ = -1;
	returning_ = false;
	armed_ = region.enabled && region.width > 0 && region.height > 0;
}

void FollowMouse::disarm()
{
	armed_ = false;
	returning_ = false;
	lastMs_ = -1;
}

void FollowMouse::returnTo(QPoint topLeftDevicePx)
{
	returning_ = true;
	target_ = QPointF(topLeftDevicePx.x(), topLeftDevicePx.y());
}

void FollowMouse::resumeFollowing()
{
	// No re-seeding: the position is already wherever the glide got to, and the
	// next tick will pick a target from the cursor relative to it. Snapping
	// here would undo the smoothing the glide just did.
	returning_ = false;
}

bool FollowMouse::settled() const
{
	return std::abs(target_.x() - pos_.x()) < 0.5 && std::abs(target_.y() - pos_.y()) < 0.5;
}

void FollowMouse::rebase(const CaptureRegion &region)
{
	// Something else moved (or resized) the region -- the user dragging the
	// frame mid-recording. Adopt it as the new truth instead of fighting the
	// drag by chasing back toward where we last were.
	base_ = region;
	pos_ = QPointF(region.x, region.y);
	target_ = pos_;
	returning_ = false;
}

bool FollowMouse::tick(QPoint cursorDevicePx, QSize screenDevicePx, const FollowParams &params, qint64 nowMs)
{
	if (!armed_)
		return false;

	// Elapsed time, from the injected clock. First tick uses a nominal 16 ms
	// so it neither jumps (huge dt) nor freezes (zero dt).
	double dtMs = 16.0;
	if (lastMs_ >= 0 && nowMs > lastMs_)
		dtMs = double(nowMs - lastMs_);
	lastMs_ = nowMs;

	// Update the TARGET from the cursor, measured against the target itself --
	// not against the smoothed position. Measured against the smoothed
	// position, the goal drifts with every step and the chase decays twice
	// over: the final approach creeps a fraction of a pixel per tick for
	// seconds after it looks finished, pushing pointless one-pixel crop
	// updates the whole time. Against the target, a parked cursor means a
	// parked target, the chase converges geometrically, and the snap below
	// genuinely ends it.
	// Gliding home: the target is fixed at where returnTo() put it, so the
	// cursor is ignored entirely and the chase below just eases into it.
	if (returning_) {
		const double aBack = stepAlpha(params.smoothness, dtMs);
		const QPointF beforeBack = pos_;
		pos_ += (target_ - pos_) * aBack;
		if (std::abs(target_.x() - pos_.x()) < 0.5 && std::abs(target_.y() - pos_.y()) < 0.5)
			pos_ = target_;
		return std::lround(beforeBack.x()) != std::lround(pos_.x()) ||
		       std::lround(beforeBack.y()) != std::lround(pos_.y());
	}

	CaptureRegion atTarget = base_;
	atTarget.x = int(std::lround(target_.x()));
	atTarget.y = int(std::lround(target_.y()));
	QPoint shift = targetShift(atTarget, cursorDevicePx, params.paddingPct);

	// Axis lock: the frozen axis never moves, whatever the cursor does.
	if (params.axis == FollowAxis::Horizontal)
		shift.setY(0);
	else if (params.axis == FollowAxis::Vertical)
		shift.setX(0);

	// Keep the whole region on the screen. Clamped at the target, not after
	// the step, so pressure against the screen edge does not accumulate as an
	// ever-growing error the frame later "pays back" with a lurch.
	const QPoint clamped = clampTopLeft(QPoint(atTarget.x + shift.x(), atTarget.y + shift.y()),
					    QSize(base_.width, base_.height), screenDevicePx);
	target_ = QPointF(clamped.x(), clamped.y());

	// The chase. Fractional position, so a slow glide never stalls a pixel
	// short the way an integer lerp does.
	const double a = stepAlpha(params.smoothness, dtMs);
	const QPointF before = pos_;
	pos_ += (target_ - pos_) * a;

	// Snap when the remaining distance rounds to nothing -- ends the glide on
	// the target instead of asymptotically hovering next to it.
	if (std::abs(target_.x() - pos_.x()) < 0.5 && std::abs(target_.y() - pos_.y()) < 0.5)
		pos_ = target_;

	// Moved, in whole device pixels? That is the only currency the crop
	// filter deals in, so it is the only change worth reporting.
	return std::lround(before.x()) != std::lround(pos_.x()) ||
	       std::lround(before.y()) != std::lround(pos_.y());
}

CaptureRegion FollowMouse::region() const
{
	CaptureRegion r = base_;
	r.x = int(std::lround(pos_.x()));
	r.y = int(std::lround(pos_.y()));
	return r;
}

} // namespace harpia
