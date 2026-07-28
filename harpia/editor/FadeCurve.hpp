#pragma once

// The shape of a clip's fade, in one place.
//
// The timeline draws the envelope, the inspector edits it, the preview mixer
// applies it and the exporter applies it again — all four have to agree to the
// sample, or what you hear while editing isn't what lands in the file. So the
// curve lives here rather than in any one of them, next to nothing but <cmath>.

#include <algorithm>
#include <cmath>

namespace harpia {

enum class FadeCurve {
	Linear = 0,
	EaseIn,      // slow start, then rushes in
	EaseOut,     // jumps in, then eases to full
	EaseInOut,   // smooth at both ends
	Exponential, // stays quiet a long time (close to how loudness is heard)
	Logarithmic, // rises fast, then creeps to full
	// sin/cos pair: two clips crossfading on this curve keep constant POWER, so
	// the mix does not dip in the middle the way two linear fades do.
	EqualPower,
};

inline constexpr int kFadeCurveCount = 7;

inline const char *fadeCurveName(FadeCurve c)
{
	switch (c) {
	case FadeCurve::Linear: return "Linear";
	case FadeCurve::EaseIn: return "Ease In";
	case FadeCurve::EaseOut: return "Ease Out";
	case FadeCurve::EaseInOut: return "Ease In-Out";
	case FadeCurve::Exponential: return "Exponential";
	case FadeCurve::Logarithmic: return "Logarithmic";
	case FadeCurve::EqualPower: return "Equal power";
	}
	return "Linear";
}

inline FadeCurve fadeCurveFromInt(int v)
{
	return (v >= 0 && v < kFadeCurveCount) ? FadeCurve(v) : FadeCurve::Linear;
}

// Gain for a FADE-IN at progress `t` (0 = start of the fade, 1 = its end).
// Always fadeGain(c, 0) == 0 and fadeGain(c, 1) == 1, so a fade never clicks.
// A fade-out is the same curve read backwards: fadeGain(c, 1 - t).
inline double fadeGain(FadeCurve c, double t)
{
	t = std::clamp(t, 0.0, 1.0);
	switch (c) {
	case FadeCurve::Linear:
		return t;
	case FadeCurve::EaseIn:
		return t * t;
	case FadeCurve::EaseOut:
		return 1.0 - (1.0 - t) * (1.0 - t);
	case FadeCurve::EaseInOut:
		return t * t * (3.0 - 2.0 * t); // smoothstep
	case FadeCurve::Exponential: {
		// Normalised so it still reaches exactly 0 and 1 at the ends.
		constexpr double k = 5.0;
		return (std::exp(k * t) - 1.0) / (std::exp(k) - 1.0);
	}
	case FadeCurve::Logarithmic: {
		constexpr double k = 9.0;
		return std::log1p(k * t) / std::log1p(k);
	}
	case FadeCurve::EqualPower:
		// A fade-out is read as fadeGain(c, 1 - t), which turns this into
		// cos(t*pi/2) -- so one curve gives both halves of the pair.
		return std::sin(t * 3.14159265358979323846 / 2.0);
	}
	return t;
}

} // namespace harpia
