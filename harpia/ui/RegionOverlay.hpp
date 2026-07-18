#pragma once

#include "core/CaptureManager.hpp"

#include <QRect>
#include <functional>

namespace harpia {

// Deferred feature: an always-on-top, click-through translucent overlay that
// outlines the region currently being recorded, plus an interactive
// drag-to-select mode for choosing that region.
//
// Planned implementation:
//   - RegionSelector: a fullscreen frameless dimmed window; user drags a
//     rectangle; on release emits the chosen CaptureRegion. The drag/handle math
//     can be adapted from frontend/widgets/OBSBasicPreview.cpp (CropItem and the
//     stretch-handle logic).
//   - RegionOverlay: a frameless, WA_TransparentForMouseEvents,
//     Qt::WindowStaysOnTopHint window sized to the region, painting only a
//     colored border so the user can verify the capture area at a glance.
//
// Declared now so MainWindow/CaptureManager can be wired to region selection
// without changing their interfaces when this lands.
class RegionSelector {
public:
	// Show fullscreen selection UI; call `onSelected` with the picked region
	// (region.enabled == false if the user cancelled).
	static void pick(std::function<void(const CaptureRegion &)> onSelected);
};

} // namespace harpia
