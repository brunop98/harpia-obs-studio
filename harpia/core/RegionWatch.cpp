#include "RegionWatch.hpp"

#include <cmath>

namespace harpia {

QPoint RegionWatch::toRegionSpace(QPoint globalLogical, QPoint screenOriginLogical, double dpr)
{
	if (dpr <= 0.0)
		dpr = 1.0;
	// Subtract the screen origin BEFORE scaling. The origin is a logical
	// coordinate, so scaling first would multiply a second monitor's offset by
	// the ratio and put the pointer somewhere off in space — and on a
	// single-screen 1x setup, which is where this gets tried first, the bug is
	// invisible because the origin is (0,0) and the ratio is 1.
	return QPoint(int(std::lround((globalLogical.x() - screenOriginLogical.x()) * dpr)),
		      int(std::lround((globalLogical.y() - screenOriginLogical.y()) * dpr)));
}

bool RegionWatch::contains(const CaptureRegion &region, QPoint devicePx)
{
	if (!region.enabled || region.width <= 0 || region.height <= 0)
		return true; // no region means nothing to be outside of
	return devicePx.x() >= region.x && devicePx.x() < region.x + region.width &&
	       devicePx.y() >= region.y && devicePx.y() < region.y + region.height;
}

bool RegionWatch::tick(bool armed, bool cursorInside, qint64 nowMs)
{
	if (!armed || timeoutSeconds_ < 0) {
		outsideSinceMs_ = -1;
		fired_ = false;
		return false;
	}
	if (cursorInside) {
		outsideSinceMs_ = -1;
		fired_ = false;
		return false;
	}
	if (outsideSinceMs_ < 0)
		outsideSinceMs_ = nowMs;
	if (fired_)
		return false; // already asked to stop; do not ask again every tick
	if (nowMs - outsideSinceMs_ >= qint64(timeoutSeconds_) * 1000) {
		fired_ = true;
		return true;
	}
	return false;
}

} // namespace harpia
