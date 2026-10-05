// Curved motion paths: the geometry and the movement.
//
//   * A straight key moves exactly as before (old projects are unchanged).
//   * Curved with automatic handles: the path still passes through every key
//     at its time, bends smoothly through the middle key, and two keys alone
//     stay a straight line until a handle is dragged.
//   * Dragged handles bend the segment toward them.
//   * Even pace: equal time steps cover equal distances along a lopsided
//     curve (raw Bezier parameter would not).
//   * The drawn line follows the curve; handles show only where a curve is.
#include "editor/timeline/MotionPath.hpp"
#include "editor/timeline/TimelineModel.hpp"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static TlKeyframe posKey(qint64 t, double x, double y)
{
	TlKeyframe k;
	k.tMs = t;
	k.tf.posX = x;
	k.tf.posY = y;
	for (int l = 0; l < kTlLaneCount; ++l)
		k.channel(l).on = l == TlLanePos;
	k.pos.ease = TlEase::Linear;
	return k;
}

static QPointF at(const TlClip &c, qint64 t)
{
	const TlTransform tf = c.transformAt(c.outStartMs + t);
	return QPointF(tf.posX, tf.posY);
}

static double dist(const QPointF &a, const QPointF &b)
{
	const QPointF d = a - b;
	return std::sqrt(d.x() * d.x() + d.y() * d.y());
}

// Distance from p to the straight line through a and b.
static double offLine(const QPointF &p, const QPointF &a, const QPointF &b)
{
	const QPointF ab = b - a, ap = p - a;
	return std::abs(ab.x() * ap.y() - ab.y() * ap.x()) / std::max(1e-12, dist(a, b));
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	TlClip c;
	c.type = TlClip::Type::Video;
	c.srcEndMs = 3000;
	c.outStartMs = 1000;

	std::printf("\n-- straight, as before --\n");
	{
		c.keys = {posKey(0, 0.2, 0.5), posKey(1000, 0.8, 0.5)};
		const QPointF mid = at(c, 500);
		ok(dist(mid, QPointF(0.5, 0.5)) < 1e-9, "half way in time, half way along the line");
		TlKeyframe k;
		ok(!k.curvedPath && !k.handlesManual, "a new key is straight, with automatic handles");
	}

	std::printf("\n-- curved, automatic handles --\n");
	{
		c.keys = {posKey(0, 0.2, 0.8), posKey(1000, 0.5, 0.2), posKey(2000, 0.8, 0.8)};
		for (TlKeyframe &k : c.keys)
			k.curvedPath = true;
		ok(dist(at(c, 0), QPointF(0.2, 0.8)) < 1e-9 && dist(at(c, 1000), QPointF(0.5, 0.2)) < 1e-9 &&
			   dist(at(c, 2000), QPointF(0.8, 0.8)) < 1e-9,
		   "passes through every key at its time");
		const double off = offLine(at(c, 500), QPointF(0.2, 0.8), QPointF(0.5, 0.2));
		std::printf("     half way to the middle key, %.4f off the straight chord\n", off);
		ok(off > 0.01, "bends between the keys");
		// Smooth through the middle: the turn measured across the key shrinks
		// with the step (a bend); a corner would keep its angle however close
		// you look.
		const auto turnAt = [&](qint64 key, qint64 step) {
			const QPointF b = at(c, key) - at(c, key - step), a = at(c, key + step) - at(c, key);
			return std::atan2(std::abs(b.x() * a.y() - b.y() * a.x()), b.x() * a.x() + b.y() * a.y()) * 180.0 /
			       3.14159265358979;
		};
		const double wide = turnAt(1000, 20), close = turnAt(1000, 2);
		std::printf("     turn across the middle key: %.2f deg at 20 ms, %.2f deg at 2 ms\n", wide, close);
		ok(close < wide / 4.0, "no corner at the middle key");
		QPointF in1, out1;
		c.pathHandles(1, in1, out1);
		ok(dist(in1, -out1) < 1e-12, "its automatic handles are mirrored");

		TlClip two = c;
		two.keys = {posKey(0, 0.2, 0.5), posKey(1000, 0.8, 0.5)};
		two.keys[0].curvedPath = true;
		ok(offLine(at(two, 300), QPointF(0.2, 0.5), QPointF(0.8, 0.5)) < 1e-9,
		   "two keys alone stay a line until a handle is dragged");
	}

	std::printf("\n-- dragged handles --\n");
	{
		c.keys = {posKey(0, 0.2, 0.5), posKey(1000, 0.8, 0.5)};
		c.keys[0].curvedPath = true;
		c.keys[0].handlesManual = true;
		c.keys[0].outX = 0.2;
		c.keys[0].outY = -0.4; // up
		c.keys[0].inX = -0.2;
		c.keys[0].inY = 0.4;
		const QPointF mid = at(c, 500);
		std::printf("     middle of the move at (%.3f, %.3f)\n", mid.x(), mid.y());
		ok(mid.y() < 0.45, "the path bends toward the handle");
		QPointF in, out;
		c.pathHandles(0, in, out);
		ok(dist(out, QPointF(0.2, -0.4)) < 1e-12, "the stored handle is the one used");
	}

	std::printf("\n-- an even pace along the curve --\n");
	{
		// A lopsided curve: a long handle at one end, none at the other. The
		// raw Bezier parameter would race through one end and crawl at the other.
		c.keys = {posKey(0, 0.1, 0.5), posKey(1000, 0.9, 0.5)};
		c.keys[0].curvedPath = true;
		c.keys[0].handlesManual = true;
		c.keys[0].outX = 0.7;
		c.keys[0].outY = -0.6;
		c.keys[1].handlesManual = true; // in = (0,0)
		double lo = 1e9, hi = 0;
		QPointF last = at(c, 0);
		for (int t = 50; t <= 1000; t += 50) {
			const QPointF p = at(c, t);
			const double d = dist(p, last);
			lo = std::min(lo, d);
			hi = std::max(hi, d);
			last = p;
		}
		std::printf("     steps from %.4f to %.4f\n", lo, hi);
		ok(hi / lo < 1.08, "equal time, equal distance");
		const QPointF p0(0.1, 0.5), p1(0.8, -0.1), p2(0.9, 0.5), p3(0.9, 0.5);
		const double rawA = dist(path_curve::bezierPoint(p0, p1, p2, p3, 0.05), p0);
		const double rawB = dist(path_curve::bezierPoint(p0, p1, p2, p3, 1.0), path_curve::bezierPoint(p0, p1, p2, p3, 0.95));
		ok(rawA / rawB > 2.0 || rawB / rawA > 2.0, "(where the raw parameter would not be)");
	}

	std::printf("\n-- what the preview draws --\n");
	{
		c.keys = {posKey(0, 0.2, 0.8), posKey(1000, 0.5, 0.2), posKey(2000, 0.8, 0.8)};
		c.keys[0].curvedPath = true; // first segment curved, second straight
		const MotionPath mp = buildMotionPath(c, 30.0);
		std::printf("     line has %d points\n", int(mp.line.size()));
		ok(mp.line.size() == 1 + 24 + 1, "the curved segment sampled, the straight one just its end");
		ok(dist(mp.line.first(), QPointF(0.2, 0.8)) < 1e-9 && dist(mp.line.last(), QPointF(0.8, 0.8)) < 1e-9,
		   "from the first key to the last");
		const PathHandles h0 = pathHandlesFor(c, 0), h1 = pathHandlesFor(c, 1), h2 = pathHandlesFor(c, 2);
		ok(h0.out && !h0.in, "the first key shows its out handle");
		ok(h1.in && !h1.out, "the middle key its in handle only (the path leaving it is straight)");
		ok(!h2.in && !h2.out, "the last key none");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
