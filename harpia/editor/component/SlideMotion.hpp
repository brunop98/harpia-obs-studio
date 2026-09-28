#pragma once

// Slide to Position: the clip arrives from outside the picture and settles at
// its own position over a set time, and optionally leaves again at the end.
//
// The clip's own position is the destination -- whatever the pose, keyframes
// and earlier Transform components say -- so the slide composes with them
// rather than replacing them: move the clip and it slides to the new spot.
//
// "Outside the picture" is worked out from the clip's real size on the canvas
// (its natural size fitted and scaled, and rotated), so a small logo starts
// just past the edge rather than a whole screen away, and a large one starts
// far enough that none of it shows on the first frame.
//
// Pure functions, header-only, so the test can hold them still.

#include "../timeline/TlTransform.hpp"

#include <QSize>
#include <QSizeF>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace harpia {

// Where it comes from (or goes to). Corners combine two edges.
enum class SlideEdge { Left = 0, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight, Count };
inline constexpr int kSlideEdgeCount = int(SlideEdge::Count);

inline QStringList slideEdgeNames()
{
	return {QStringLiteral("Left"),       QStringLiteral("Right"),       QStringLiteral("Top"),
		QStringLiteral("Bottom"),     QStringLiteral("Top left"),    QStringLiteral("Top right"),
		QStringLiteral("Bottom left"), QStringLiteral("Bottom right")};
}

// The curve. The first four are the editor's usual eases; the last three
// arrive with character: Back overshoots and settles, Bounce lands and
// bounces, Elastic springs.
enum class SlideCurve { Linear = 0, EaseIn, EaseOut, EaseInOut, Back, Bounce, Elastic, Count };
inline constexpr int kSlideCurveCount = int(SlideCurve::Count);

inline QStringList slideCurveNames()
{
	return {QStringLiteral("Linear"),   QStringLiteral("Ease in"), QStringLiteral("Ease out"),
		QStringLiteral("Ease in-out"), QStringLiteral("Back (overshoot)"), QStringLiteral("Bounce"),
		QStringLiteral("Elastic")};
}

// 0..1 progress through the curve. 0 -> 0 and 1 -> 1 for every curve; Back
// and Elastic pass beyond 1 on the way, which is the overshoot.
inline double slideCurveAt(SlideCurve c, double u)
{
	u = std::clamp(u, 0.0, 1.0);
	constexpr double kPi = 3.14159265358979323846;
	switch (c) {
	case SlideCurve::Linear:
		return u;
	case SlideCurve::EaseIn:
		return u * u * u;
	case SlideCurve::EaseOut: {
		const double v = 1.0 - u;
		return 1.0 - v * v * v;
	}
	case SlideCurve::EaseInOut:
		return u < 0.5 ? 4.0 * u * u * u : 1.0 - std::pow(-2.0 * u + 2.0, 3.0) / 2.0;
	case SlideCurve::Back: {
		const double c1 = 1.70158, c3 = c1 + 1.0;
		const double v = u - 1.0;
		return 1.0 + c3 * v * v * v + c1 * v * v;
	}
	case SlideCurve::Bounce: {
		const double n1 = 7.5625, d1 = 2.75;
		double x = u;
		if (x < 1.0 / d1)
			return n1 * x * x;
		if (x < 2.0 / d1) {
			x -= 1.5 / d1;
			return n1 * x * x + 0.75;
		}
		if (x < 2.5 / d1) {
			x -= 2.25 / d1;
			return n1 * x * x + 0.9375;
		}
		x -= 2.625 / d1;
		return n1 * x * x + 0.984375;
	}
	case SlideCurve::Elastic: {
		if (u <= 0.0 || u >= 1.0)
			return u;
		const double c4 = (2.0 * kPi) / 3.0;
		return std::pow(2.0, -10.0 * u) * std::sin((u * 10.0 - 0.75) * c4) + 1.0;
	}
	case SlideCurve::Count:
		break;
	}
	return u;
}

// The clip's bounding box on the canvas, in canvas pixels, at pose `xf`.
// `srcSize` empty = unknown: the fitted box is then taken as the whole canvas
// at that scale, which is never smaller than the real one, so the clip is
// still fully off-screen (it just travels a little further than needed).
inline QSizeF slideClipExtent(const TlTransform &xf, QSize canvas, QSize srcSize)
{
	const double W = std::max(1, canvas.width());
	const double H = std::max(1, canvas.height());
	const double s = std::max(0.001, xf.scale);
	double w = W * s, h = H * s;
	if (srcSize.width() > 0 && srcSize.height() > 0) {
		const double fit = std::min(W / srcSize.width(), H / srcSize.height());
		w = srcSize.width() * fit * s;
		h = srcSize.height() * fit * s;
	}
	const double a = xf.rotation * 3.14159265358979323846 / 180.0;
	const double ca = std::abs(std::cos(a)), sa = std::abs(std::sin(a));
	return QSizeF(w * ca + h * sa, w * sa + h * ca);
}

// The position (posX, posY as canvas fractions) that puts the clip just
// outside the picture on `edge`, keeping the other axis where it is.
inline void slideOffPosition(SlideEdge edge, const TlTransform &target, QSize canvas, QSize srcSize,
			     double *posX, double *posY)
{
	const double W = std::max(1, canvas.width());
	const double H = std::max(1, canvas.height());
	const QSizeF ext = slideClipExtent(target, canvas, srcSize);
	constexpr double kMargin = 2.0; // px: no sliver of an antialiased edge on frame one
	const double left = (-ext.width() / 2.0 - kMargin) / W;
	const double right = (W + ext.width() / 2.0 + kMargin) / W;
	const double top = (-ext.height() / 2.0 - kMargin) / H;
	const double bottom = (H + ext.height() / 2.0 + kMargin) / H;
	double x = target.posX, y = target.posY;
	switch (edge) {
	case SlideEdge::Left: x = left; break;
	case SlideEdge::Right: x = right; break;
	case SlideEdge::Top: y = top; break;
	case SlideEdge::Bottom: y = bottom; break;
	case SlideEdge::TopLeft: x = left; y = top; break;
	case SlideEdge::TopRight: x = right; y = top; break;
	case SlideEdge::BottomLeft: x = left; y = bottom; break;
	case SlideEdge::BottomRight: x = right; y = bottom; break;
	case SlideEdge::Count: break;
	}
	*posX = x;
	*posY = y;
}

struct SlideSettings {
	SlideEdge from = SlideEdge::Left;
	qint64 durationMs = 500;
	SlideCurve curve = SlideCurve::EaseOut;
	bool slideOut = false;
	// -1 = leave the way it came in; otherwise the edge it leaves by.
	int outEdge = -1;
};

// The pose at `tMs` into a clip of `durMs`. `target` is where the clip sits
// when it is not sliding. In the first durationMs it travels from outside to
// the target; with slideOut, in the last durationMs it travels from the target
// to outside, on the same curve run backwards (so an Ease out arrival that
// settles gently becomes a departure that starts gently and speeds away). A
// clip too short for both halves gives each half of it.
inline TlTransform slidePoseAt(const SlideSettings &s, const TlTransform &target, qint64 tMs, qint64 durMs,
			       QSize canvas, QSize srcSize)
{
	TlTransform out = target;
	durMs = std::max<qint64>(1, durMs);
	qint64 d = std::max<qint64>(0, s.durationMs);
	if (d == 0)
		return out;
	d = std::min(d, s.slideOut ? durMs / 2 : durMs);
	if (d <= 0)
		return out;
	if (tMs < d) {
		double ox = 0, oy = 0;
		slideOffPosition(s.from, target, canvas, srcSize, &ox, &oy);
		const double p = slideCurveAt(s.curve, double(std::max<qint64>(0, tMs)) / double(d));
		out.posX = ox + (target.posX - ox) * p;
		out.posY = oy + (target.posY - oy) * p;
		return out;
	}
	if (s.slideOut && tMs > durMs - d) {
		const SlideEdge to = (s.outEdge >= 0 && s.outEdge < kSlideEdgeCount) ? SlideEdge(s.outEdge) : s.from;
		double ox = 0, oy = 0;
		slideOffPosition(to, target, canvas, srcSize, &ox, &oy);
		const double v = double(std::min(tMs, durMs) - (durMs - d)) / double(d); // 0..1 leaving
		const double p = slideCurveAt(s.curve, 1.0 - v);                            // 1..0
		out.posX = ox + (target.posX - ox) * p;
		out.posY = oy + (target.posY - oy) * p;
		return out;
	}
	return out;
}

} // namespace harpia
