// The keyframe list's reorder, reset and delete, on the model.
//
//   * Moving a key in the list moves its POSE: the times stay where they were,
//     and the pose carries its channels, easing and path shape with it.
//   * Move to first / last, a drag between, and a full permutation all agree.
//   * Anything that is not a real move (out of range, same slot, not a
//     permutation) changes nothing.
//   * Reset value frames the whole picture again (100%, centred, upright) and
//     pins position and scale so it shows; opacity is not framing.
//   * Delete takes the whole key, and a lone key left becomes the base pose.
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

static TlKeyframe key(qint64 t, double x, double scale)
{
	TlKeyframe k;
	k.tMs = t;
	k.tf.posX = x;
	k.tf.posY = 0.5;
	k.tf.scale = scale;
	return k;
}

static QVector<TlKeyframe> three()
{
	// A at 0 ms, B at 300 ms, C at 5400 ms -- the times from the screenshot.
	QVector<TlKeyframe> keys = {key(0, 0.1, 1.0), key(300, 0.5, 2.0), key(5400, 0.9, 3.0)};
	keys[1].curvedPath = true;
	keys[1].handlesManual = true;
	keys[1].outX = 0.07;
	keys[1].channel(TlLanePos).ease = TlEase::Linear;
	keys[1].rot.on = false;
	return keys;
}

static bool timesAre(const QVector<TlKeyframe> &k, qint64 a, qint64 b, qint64 c)
{
	return k.size() == 3 && k[0].tMs == a && k[1].tMs == b && k[2].tMs == c;
}

static bool posesAre(const QVector<TlKeyframe> &k, double a, double b, double c)
{
	return k.size() == 3 && k[0].tf.posX == a && k[1].tf.posX == b && k[2].tf.posX == c;
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);

	std::printf("\n-- move to first / last --\n");
	{
		QVector<TlKeyframe> k = three();
		ok(moveKeyPose(k, 2, 0), "C to first");
		ok(timesAre(k, 0, 300, 5400), "the times stay where they were");
		ok(posesAre(k, 0.9, 0.1, 0.5), "C plays first, then A, then B");
		ok(k[0].tf.scale == 3.0, "C's whole pose went with it (its zoom too)");
		ok(k[2].curvedPath && k[2].handlesManual && k[2].outX == 0.07, "B took its curve and handles along");
		ok(k[2].channel(TlLanePos).ease == TlEase::Linear && !k[2].rot.on, "and its easing and channels");
	}
	{
		QVector<TlKeyframe> k = three();
		ok(moveKeyPose(k, 0, 2), "A to last");
		ok(timesAre(k, 0, 300, 5400) && posesAre(k, 0.5, 0.9, 0.1), "B, C, then A");
	}
	{
		QVector<TlKeyframe> k = three();
		ok(moveKeyPose(k, 0, 1), "a drag one row down");
		ok(posesAre(k, 0.5, 0.1, 0.9), "swaps the first two framings");
	}

	std::printf("\n-- a whole new order --\n");
	{
		QVector<TlKeyframe> k = three();
		ok(reorderKeyPoses(k, {2, 0, 1}), "order {C, A, B}");
		QVector<TlKeyframe> viaMove = three();
		moveKeyPose(viaMove, 2, 0);
		ok(k == viaMove, "the same as moving C to first");
	}

	std::printf("\n-- nothing to move --\n");
	{
		const QVector<TlKeyframe> before = three();
		QVector<TlKeyframe> k = before;
		ok(!moveKeyPose(k, 1, 1), "onto itself: no");
		ok(!moveKeyPose(k, -1, 0) && !moveKeyPose(k, 0, 3), "out of range: no");
		ok(!reorderKeyPoses(k, {0, 1, 2}), "the order it already has: no");
		ok(!reorderKeyPoses(k, {0, 0, 1}), "not a permutation: no");
		ok(!reorderKeyPoses(k, {1, 0}), "the wrong length: no");
		ok(k == before, "and the keys are untouched");
	}

	std::printf("\n-- the clip plays the new order --\n");
	{
		TlClip c;
		c.type = TlClip::Type::Video;
		c.srcEndMs = 6000;
		c.outStartMs = 1000;
		c.keys = three();
		for (TlKeyframe &k : c.keys)
			k.curvedPath = false;
		moveKeyPose(c.keys, 2, 0);
		const TlTransform at0 = c.transformAt(1000);
		const TlTransform atEnd = c.transformAt(1000 + 5400);
		std::printf("     at 0:00 x=%.2f scale=%.1f, at the end x=%.2f\n", at0.posX, at0.scale, atEnd.posX);
		ok(std::abs(at0.posX - 0.9) < 1e-9 && std::abs(at0.scale - 3.0) < 1e-9, "at the first time it is C's framing");
		ok(std::abs(atEnd.posX - 0.5) < 1e-9, "and at the last time B's");
	}

	std::printf("\n-- reset value --\n");
	{
		TlKeyframe k = key(300, 0.2, 2.5);
		k.tf.posY = 0.8;
		k.tf.rotation = 30.0;
		k.tf.opacity = 0.4;
		k.pos.on = false;
		k.scale.on = false;
		k.rot.on = true;
		resetKeyFraming(k);
		ok(k.tf.posX == 0.5 && k.tf.posY == 0.5, "centred");
		ok(k.tf.scale == 1.0, "100% of the screen");
		ok(k.tf.rotation == 0.0, "upright");
		ok(k.pos.on && k.scale.on, "position and scale pinned, so the reset shows");
		ok(k.tf.opacity == 0.4, "opacity left alone");
		ok(k.tMs == 300, "at the same time");
	}

	std::printf("\n-- delete --\n");
	{
		TlClip c;
		c.keys = three();
		ok(removeKeyframe(c, 1) && c.keys.size() == 2, "the middle key goes");
		ok(c.keys[0].tMs == 0 && c.keys[1].tMs == 5400, "the others keep their times");
		ok(!removeKeyframe(c, 5), "out of range: no");
		ok(removeKeyframe(c, 0) && c.keys.isEmpty(), "down to one key: the animation ends");
		const TlTransform base = c.baseTransform();
		ok(std::abs(base.posX - 0.9) < 1e-9 && std::abs(base.scale - 3.0) < 1e-9,
		   "and the clip holds the framing that was left");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
