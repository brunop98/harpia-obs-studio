#pragma once

// Curved motion paths: the geometry.
//
// A position key can say the path LEAVING it is curved. That segment is then a
// cubic Bezier from this key to the next position key, bent by two handles:
// this key's OUT handle and the next key's IN handle. Each handle is an offset
// from its key, in canvas fractions, like the positions themselves.
//
// Handles start AUTOMATIC: a smooth curve through the keys (Catmull-Rom), so
// switching a key to curved bends the path the way you would draw it by hand
// with nothing more to do. Dragging a handle makes that key's handles manual;
// they stay mirrored (a smooth corner) unless the key is "broken" (Alt-drag),
// when in and out move on their own.
//
// The clip moves along a curve at an EVEN pace: the key's ease decides how far
// along the path it is (as a fraction of the path's LENGTH), and the arc-length
// table turns that into a point. Without it a clip would rush through wide
// bends and crawl through tight ones depending on how the handles were drawn.
//
// Pure: points in, points out.

#include <QPointF>

#include <algorithm>
#include <array>
#include <cmath>

namespace harpia {

namespace path_curve {

inline QPointF bezierPoint(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3, double t)
{
	const double u = 1.0 - t;
	return p0 * (u * u * u) + p1 * (3.0 * u * u * t) + p2 * (3.0 * u * t * t) + p3 * (t * t * t);
}

// Automatic handles for a key at `cur`, between `prev` and `next` (either may be
// missing at the ends of the path). Catmull-Rom: the tangent runs parallel to
// prev->next, a third of the way to each neighbour; at an end it points
// straight at the one neighbour, so a curve with just two keys is a line until
// a handle is dragged.
inline void autoHandles(const QPointF &cur, const QPointF *prev, const QPointF *next, QPointF &in, QPointF &out)
{
	QPointF tangent(0.0, 0.0);
	if (prev && next)
		tangent = (*next - *prev) / 2.0;
	else if (next)
		tangent = *next - cur;
	else if (prev)
		tangent = cur - *prev;
	out = tangent / 3.0;
	in = -tangent / 3.0;
	(void)cur;
}

// Arc-length parametrisation of one segment: `s` (0..1, a fraction of the
// segment's length) to the Bezier parameter t. Sampled -- 48 chords are far
// finer than a frame's movement -- and linear between samples.
class ArcTable {
public:
	static constexpr int kSamples = 48;

	ArcTable(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3)
	{
		len_[0] = 0.0;
		QPointF last = p0;
		for (int i = 1; i <= kSamples; ++i) {
			const QPointF q = bezierPoint(p0, p1, p2, p3, double(i) / kSamples);
			const QPointF d = q - last;
			len_[size_t(i)] = len_[size_t(i - 1)] + std::sqrt(d.x() * d.x() + d.y() * d.y());
			last = q;
		}
	}

	double length() const { return len_[kSamples]; }

	double tAt(double s) const
	{
		s = std::clamp(s, 0.0, 1.0);
		const double total = length();
		if (total <= 1e-12)
			return s; // a segment with no length: any t is the same point
		const double want = s * total;
		const auto it = std::lower_bound(len_.begin(), len_.end(), want);
		const int i = std::clamp(int(it - len_.begin()), 1, kSamples);
		const double a = len_[size_t(i - 1)], b = len_[size_t(i)];
		const double f = b > a ? (want - a) / (b - a) : 0.0;
		return (double(i - 1) + f) / kSamples;
	}

private:
	std::array<double, kSamples + 1> len_{};
};

// The point a fraction `s` of the way ALONG the segment (by length).
inline QPointF pointAlong(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3, double s)
{
	const ArcTable table(p0, p1, p2, p3);
	return bezierPoint(p0, p1, p2, p3, table.tAt(s));
}

} // namespace path_curve

} // namespace harpia
