#pragma once

// In / Out times on every component.
//
// A component used to be all-or-nothing for the whole length of its clip: a
// Saturation boost was simply ON, arriving and leaving on a hard cut. Getting
// it to ease in meant placing keyframes by hand on every property, which is a
// lot of clicking for what is nearly always the same shape -- ramp up, hold,
// ramp down.
//
// So every component carries two numbers, both 0 by default (instant, exactly
// as before): inMs and outMs. They produce a WEIGHT over the clip's life:
//
//   0 ....... inMs ................ dur-outMs ....... dur
//   |  ramp 0->1  |     hold at 1      |   ramp 1->0   |
//
// What the weight then does depends on the component's stage, because "half a
// saturation boost" and "half a move" are not the same kind of thing:
//
//   * Pixel / Composite: the component's OUTPUT is blended with its input by
//     the weight. Universal -- no component has to declare anything -- and for
//     effects that are linear in the image (saturation, brightness, tint) it
//     is exactly the same result as ramping the parameter. Skipped entirely at
//     weight 1, so the cost only exists during the ramps.
//   * Transform / Audio: the PARAMETERS ramp instead, from the clip's base
//     pose toward what the component asks for. Blending a moved image with an
//     unmoved one gives a double exposure, not motion.
//   * Time / Source: no envelope. There is no meaningful half-way between two
//     source instants, and fading a clip's own picture in against nothing
//     would just be an opacity animation wearing a disguise.
//
// The weight multiplies ON TOP of any keyframes: the keyframes say what the
// value is at each moment, the envelope says how much of the component is
// present. That composition is the point -- a hand-keyframed wobble can still
// ease in.
//
// The arithmetic lives here, pure, because it is the part that goes quietly
// wrong: an envelope that never reaches 1 leaves an effect permanently at
// half strength, and one that divides by a zero-length clip takes the whole
// render down.

#include "../timeline/Ease.hpp"

#include <QtGlobal> // qint64

#include <algorithm>
#include <cmath>

namespace harpia {

// The weight of a component at `tMs` into a clip of `durMs`, given its ramp
// times. Always in [0, 1]. Both ramps zero (the default) means a flat 1: the
// behaviour every existing project already has.
inline double componentWeight(qint64 tMs, qint64 durMs, qint64 inMs, qint64 outMs,
			      TlEase ease = TlEase::EaseInOut)
{
	if (durMs <= 0)
		return 1.0; // a zero-length clip renders nothing; never divide by it
	qint64 in = std::max<qint64>(0, inMs);
	qint64 out = std::max<qint64>(0, outMs);
	if (in == 0 && out == 0)
		return 1.0; // the default: no envelope at all, and no work done

	// Overlapping ramps are scaled down PROPORTIONALLY rather than clipped, so
	// the effect still reaches full strength in the middle. Trimming a clip
	// shorter must not make its effect quietly never arrive.
	if (in + out > durMs) {
		const double scale = double(durMs) / double(in + out);
		in = qint64(in * scale);
		out = qint64(out * scale);
	}

	const qint64 t = std::clamp<qint64>(tMs, 0, durMs);
	if (in > 0 && t < in)
		return std::clamp(tlEaseAt(ease, double(t) / double(in)), 0.0, 1.0);
	if (out > 0 && t > durMs - out) {
		const double u = double(durMs - t) / double(out);
		return std::clamp(tlEaseAt(ease, u), 0.0, 1.0);
	}
	return 1.0; // the hold
}

// Does this component need envelope work at all right now? Lets every caller
// take the cheap path -- no image copy, no blend -- for the whole hold, which
// is the overwhelming majority of frames.
inline bool envelopeIsFull(double w)
{
	return w >= 0.999;
}

// Nothing of the component is present: callers skip it entirely rather than
// blending toward an output nobody will see.
inline bool envelopeIsEmpty(double w)
{
	return w <= 0.001;
}

} // namespace harpia
