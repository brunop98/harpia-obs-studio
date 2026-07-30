#pragma once

// The animatable pose of a clip on the output canvas.
//
// In its own header because both the timeline model and the component runtime
// need it, and each needs the other: a clip holds components, and a component
// reads and writes a pose. One tiny shared type breaks that circle without
// either side having to know the other exists.

#include <cmath>

namespace harpia {

// Resolved per output frame — from the clip's base pose, or interpolated
// between its keyframes, then handed to whatever components want a say.
struct TlTransform {
	double posX = 0.5;     // centre X (0..1 across the canvas)
	double posY = 0.5;     // centre Y (0..1 down the canvas)
	double scale = 1.0;    // 1 = fit the canvas; >1 zooms in; <1 = picture-in-picture
	double rotation = 0.0; // degrees clockwise, about the clip's own centre
	double opacity = 1.0;
};

// Pull a pose onto the canvas centre when it is nearly there.
//
// Dragging a clip to "exactly centred" by hand is a game of one-pixel
// adjustments that you lose: 0.4997 looks centred, reads as centred, and is
// not -- and the difference shows up as a shimmer against anything that IS
// centred. The Snap toggle already means "help me line things up" for clip
// edges on the timeline; this is the same promise for the picture.
//
// Each axis snaps on its own, so sliding something down the middle of the frame
// keeps its horizontal centring instead of having to hold both at once.
// `threshold` is in canvas fractions. Reports which axes were caught so the
// caller can show a guide -- a snap you cannot see is indistinguishable from
// the drag having stuck.
inline TlTransform snapPoseToCentre(const TlTransform &in, double threshold, bool *snappedX,
				    bool *snappedY)
{
	TlTransform out = in;
	const bool sx = threshold > 0.0 && std::abs(in.posX - 0.5) <= threshold;
	const bool sy = threshold > 0.0 && std::abs(in.posY - 0.5) <= threshold;
	if (sx)
		out.posX = 0.5;
	if (sy)
		out.posY = 0.5;
	if (snappedX)
		*snappedX = sx;
	if (snappedY)
		*snappedY = sy;
	return out;
}

} // namespace harpia
