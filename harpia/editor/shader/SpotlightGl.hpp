#pragma once

// The Inverse Selection effect, on the GPU.
//
// Why: measured at 1080p, the whole effect costs 0.9 ms with no blur and 47 ms
// with it — 42 ms of that inside the box blur alone, which is more than a
// 30 fps frame budget for one effect, and 197 ms at 4K. Blurring at reduced
// resolution took it to about 11 ms and no further, because Qt's rescale then
// dominates. So the blur is the reason this exists; the mask and the dim come
// along because once the frame is on the GPU there is no sense bringing it back
// down to do them.
//
// This is a SECOND implementation of something that already worked, which is a
// thing worth being nervous about: preview and export both call
// Spotlight::apply, and if the two paths disagree then what you see stops being
// what you get. Two things keep that honest. The shapes are evaluated from the
// same numbers QPainterPath is built from, with hard edges rather than
// antialiased ones because that is what the CPU path does. And the Gaussian's
// sigma is derived from the CPU's three box passes rather than picked by eye —
// three boxes of radius r have variance 3((2r+1)^2-1)/12, and that is what the
// shader uses. spotlightgl_test holds the two outputs against each other,
// including a rotated mask, which is where a coordinate-convention slip shows
// up first.
//
// Falls back silently: tryApply returns false when there is no GL 3.3 here (a
// headless export box, a remote session, a driver that will not give an
// offscreen context), when the frame is tiny enough that the round trip is not
// worth it, or when there are more masks than the shader has room for. The
// caller then runs the CPU path, which remains the reference.

#include <QtGlobal>

class QImage;

namespace harpia {

struct SpotlightSpec;

// The uniform arrays are fixed-size; a spec with more areas than this takes the
// CPU path rather than being silently truncated.
inline constexpr int kMaxGlMasks = 16;

class SpotlightGl {
public:
	// Apply `spec` to `frame` in place on the GPU. Returns false if it could
	// not, having left the frame untouched — the caller must then do it on the
	// CPU. Safe to call from any thread: the context is per-thread, built on
	// first use, which is what lets the preview and the export worker each have
	// one.
	static bool tryApply(QImage &frame, const SpotlightSpec &spec, qint64 outMs);

	// Whether this thread has (or can get) a usable context. Exposed for tests
	// and for reporting; tryApply calls it itself.
	static bool available();
};

} // namespace harpia
