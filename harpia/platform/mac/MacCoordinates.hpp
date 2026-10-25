#pragma once

#include <QRect>
#include <QRectF>
#include <QSize>

#include <algorithm>
#include <cmath>
#include <optional>

namespace harpia::mac_platform {

// A mixed-DPI desktop has no single global physical-pixel coordinate system.
// Intersect in Quartz points first, then scale only the selected display's
// local rectangle into its captured pixels. This also handles negative origins.
inline std::optional<QRect> physicalRegion(const QRect &windowPoints, const QRect &displayPoints,
					 const QSize &capturePixels)
{
	if (displayPoints.isEmpty() || capturePixels.isEmpty())
		return std::nullopt;
	const QRectF overlap = QRectF(windowPoints).intersected(QRectF(displayPoints));
	if (overlap.isEmpty())
		return std::nullopt;
	const double sx = double(capturePixels.width()) / displayPoints.width();
	const double sy = double(capturePixels.height()) / displayPoints.height();
	const int left = std::clamp(int(std::floor((overlap.left() - displayPoints.x()) * sx)),
				  0, capturePixels.width());
	const int top = std::clamp(int(std::floor((overlap.top() - displayPoints.y()) * sy)),
				 0, capturePixels.height());
	const int right = std::clamp(int(std::ceil((overlap.right() - displayPoints.x()) * sx)),
				   left, capturePixels.width());
	const int bottom = std::clamp(int(std::ceil((overlap.bottom() - displayPoints.y()) * sy)),
				    top, capturePixels.height());
	QRect result(left, top, (right - left) & ~1, (bottom - top) & ~1);
	return result.width() >= 16 && result.height() >= 16 ? std::optional<QRect>(result) : std::nullopt;
}

} // namespace harpia::mac_platform
