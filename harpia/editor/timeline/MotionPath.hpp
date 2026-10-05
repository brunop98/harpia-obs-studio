#pragma once

// A clip's position animation, as something to draw and grab on the preview.
//
// Position is its own keyframe channel (TlLanePos): a key that pins only scale
// or rotation does not bend the path, so only the keys that pin position are
// points on it. Between two of them the clip travels in a straight line, or
// along a curve when the first key says so (PathCurve.hpp) -- easing changes
// how fast it goes, never where -- so the path is the line through those keys,
// sampled along each curve, and the per-frame dots along it are where the
// clip really is on each frame: bunched up where it is slow, spread out where
// it is fast. That is what makes an ease visible without playing it.
//
// Pure: no widgets, no painter. The preview draws what this returns and the
// window turns a click on the path back into a time with motionPathTimeAt.

#include "TimelineModel.hpp"

#include <QPointF>
#include <QSizeF>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

namespace harpia {

struct MotionPathKey {
	int keyIndex = -1; // index into TlClip::keys
	qint64 tMs = 0;    // clip-relative
	QPointF pos;       // the clip's centre, as fractions of the canvas
};

struct MotionPath {
	QVector<MotionPathKey> keys; // position keys, in time order
	QVector<QPointF> line;       // the path as drawn: through the keys, curves sampled
	QVector<QPointF> frames;     // the centre on every frame between the first and last key
	bool drawable() const { return keys.size() >= 2; }
};

// The path for `c` at `fps`. Frames are capped so a very long clip at a high
// frame rate does not become thousands of dots; past the cap they are evenly
// spread in time, which still shows speed.
inline MotionPath buildMotionPath(const TlClip &c, double fps, int maxFrames = 1500)
{
	MotionPath mp;
	for (int i = 0; i < c.keys.size(); ++i) {
		const TlKeyframe &k = c.keys[i];
		if (!k.channel(TlLanePos).on)
			continue;
		MotionPathKey pk;
		pk.keyIndex = i;
		pk.tMs = k.tMs;
		pk.pos = QPointF(k.tf.posX, k.tf.posY);
		mp.keys.append(pk);
	}
	if (mp.keys.size() < 2)
		return mp;
	// The drawn line: a straight segment is its two ends, a curve 24 chords --
	// smooth at any size the preview is shown at.
	for (int j = 0; j + 1 < mp.keys.size(); ++j) {
		const int a = mp.keys[j].keyIndex, b = mp.keys[j + 1].keyIndex;
		const QPointF p0 = mp.keys[j].pos, p3 = mp.keys[j + 1].pos;
		if (mp.line.isEmpty())
			mp.line.append(p0);
		if (c.keys[a].curvedPath) {
			QPointF inA, outA, inB, outB;
			c.pathHandles(a, inA, outA);
			c.pathHandles(b, inB, outB);
			for (int i = 1; i < 24; ++i)
				mp.line.append(path_curve::bezierPoint(p0, p0 + outA, p3 + inB, p3, i / 24.0));
		}
		mp.line.append(p3);
	}
	const qint64 t0 = mp.keys.first().tMs, t1 = mp.keys.last().tMs;
	const double step = std::max(1000.0 / std::max(1.0, fps), double(t1 - t0) / std::max(1, maxFrames));
	for (double t = double(t0); t <= double(t1) + 0.5; t += step) {
		const TlTransform tf = c.transformAt(c.outStartMs + qint64(std::llround(t)));
		mp.frames.append(QPointF(tf.posX, tf.posY));
	}
	return mp;
}

// Which handles key `keyIndex` shows on the preview, and where (canvas
// fractions, absolute): the OUT handle when the path leaving it is curved, the
// IN handle when the path arriving at it is. A straight path has no handles
// to show; neither does a key that does not pin position.
struct PathHandles {
	bool in = false, out = false;
	QPointF inPos, outPos;
};
inline PathHandles pathHandlesFor(const TlClip &c, int keyIndex)
{
	PathHandles h;
	if (keyIndex < 0 || keyIndex >= c.keys.size() || !c.keys[keyIndex].channel(TlLanePos).on)
		return h;
	const QVector<int> pk = c.keysOnLane(TlLanePos);
	const int j = int(pk.indexOf(keyIndex));
	const TlKeyframe &k = c.keys[keyIndex];
	const QPointF at(k.tf.posX, k.tf.posY);
	QPointF in, out;
	c.pathHandles(keyIndex, in, out);
	h.out = k.curvedPath && j + 1 < pk.size();
	h.in = j > 0 && c.keys[pk[j - 1]].curvedPath;
	h.inPos = at + in;
	h.outPos = at + out;
	return h;
}

// Distance from p to the segment ab, in the same units as the points.
inline double motionSegmentDistance(const QPointF &p, const QPointF &a, const QPointF &b)
{
	const QPointF ab = b - a;
	const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
	double u = 0.0;
	if (len2 > 1e-12)
		u = std::clamp(((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2, 0.0, 1.0);
	const QPointF q = a + ab * u - p;
	return std::sqrt(q.x() * q.x() + q.y() * q.y());
}

// The key under `px` (in pixels), given where each key is drawn: the NEAREST
// within `radius`, so two dots close together pick the one you aimed at.
// Returns an index into `keysPx`, or -1.
inline int motionPathKeyAt(const QVector<QPointF> &keysPx, const QPointF &px, double radius)
{
	int best = -1;
	double bestD = radius;
	for (int i = 0; i < keysPx.size(); ++i) {
		const QPointF d = keysPx[i] - px;
		const double dist = std::sqrt(d.x() * d.x() + d.y() * d.y());
		if (dist <= bestD) {
			bestD = dist;
			best = i;
		}
	}
	return best;
}

// Whether `px` lies on the drawn polyline (within `tol` pixels).
inline bool motionPathHit(const QVector<QPointF> &keysPx, const QPointF &px, double tol)
{
	for (int i = 0; i + 1 < keysPx.size(); ++i)
		if (motionSegmentDistance(px, keysPx[i], keysPx[i + 1]) <= tol)
			return true;
	return false;
}

// The clip-relative time at which the clip passes closest to `norm` (canvas
// fractions), searched between the first and last position key. `px` is the
// canvas's size on screen, so "closest" is measured in pixels rather than in
// fractions of a non-square picture. -1 when the path is not drawable or the
// point is further than `tolPx` from it.
//
// Sampled, not solved: an ease between two keys has no closed-form inverse
// worth writing, and 5 ms steps put the new key within a frame of the click.
inline qint64 motionPathTimeAt(const TlClip &c, const QPointF &norm, const QSizeF &px, double tolPx,
			       qint64 stepMs = 5)
{
	const MotionPath mp = buildMotionPath(c, 1000.0 / std::max<qint64>(1, stepMs), 1 << 20);
	if (!mp.drawable())
		return -1;
	const qint64 t0 = mp.keys.first().tMs;
	const double sx = std::max(1.0, px.width()), sy = std::max(1.0, px.height());
	qint64 bestT = -1;
	double bestD = std::numeric_limits<double>::max();
	for (int i = 0; i < mp.frames.size(); ++i) {
		const double dx = (mp.frames[i].x() - norm.x()) * sx;
		const double dy = (mp.frames[i].y() - norm.y()) * sy;
		const double d = std::sqrt(dx * dx + dy * dy);
		if (d < bestD) {
			bestD = d;
			bestT = t0 + qint64(i) * stepMs;
		}
	}
	if (bestD > tolPx)
		return -1;
	return std::min(bestT, mp.keys.last().tMs);
}

} // namespace harpia
