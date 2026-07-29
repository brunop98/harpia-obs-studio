#pragma once

// The animatable pose of a clip on the output canvas.
//
// In its own header because both the timeline model and the component runtime
// need it, and each needs the other: a clip holds components, and a component
// reads and writes a pose. One tiny shared type breaks that circle without
// either side having to know the other exists.

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

} // namespace harpia
