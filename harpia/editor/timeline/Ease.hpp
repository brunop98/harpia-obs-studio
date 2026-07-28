#pragma once

// Interpolation shapes, shared by anything on the timeline that animates:
// clip keyframes, and the Spotlight masks. Its own header so Spotlight.hpp and
// TimelineModel.hpp can both use it without including each other.

#include <algorithm>
#include <cmath>

namespace harpia {

// How a keyframe hands over to the next one, per channel.
enum class TlEase { Linear, EaseIn, EaseOut, EaseInOut, Bezier };

inline constexpr int kTlEaseCount = 5;

inline const char *tlEaseName(TlEase e)
{
	switch (e) {
	case TlEase::Linear: return "Linear";
	case TlEase::EaseIn: return "Ease In";
	case TlEase::EaseOut: return "Ease Out";
	case TlEase::EaseInOut: return "Ease In-Out";
	case TlEase::Bezier: return "Bezier";
	}
	return "Linear";
}

inline TlEase tlEaseFromInt(int v)
{
	return (v >= 0 && v < kTlEaseCount) ? TlEase(v) : TlEase::Linear;
}

// Remap 0..1 progress through an ease. `p1`/`p2` are the Bezier handles (the
// x of a standard CSS-style cubic-bezier(p1, p1, p2, p2) with the control
// points on the diagonal, which is the shape a two-number handle can describe).
inline double tlEaseAt(TlEase e, double u, double p1 = 0.42, double p2 = 0.58)
{
	u = std::clamp(u, 0.0, 1.0);
	switch (e) {
	case TlEase::Linear:
		return u;
	case TlEase::EaseIn:
		return u * u;
	case TlEase::EaseOut:
		return 1.0 - (1.0 - u) * (1.0 - u);
	case TlEase::EaseInOut:
		return u * u * (3.0 - 2.0 * u); // smoothstep
	case TlEase::Bezier: {
		// Cubic Bezier with control points (p1,p1) and (p2,p2): symmetric in x
		// and y, so y(u) is just the curve evaluated at u -- no root solve.
		const double a = std::clamp(p1, 0.0, 1.0), b = std::clamp(p2, 0.0, 1.0);
		const double v = 1.0 - u;
		return 3.0 * v * v * u * a + 3.0 * v * u * u * b + u * u * u;
	}
	}
	return u;
}

} // namespace harpia
