// Automatic Zoom: the scene transform that magnifies the recorded picture.
//
// The first version of this cropped the capture source and assumed libobs
// would scale the crop back up to fill the canvas. It does not. A source bound
// straight to an output channel is drawn at its own size at the top-left
// corner, so a "2x zoom" recorded a quarter-size picture in the corner of an
// otherwise black frame -- and every check in the old version of this file
// passed, because they all tested the CROP RECTANGLE and the crop rectangle was
// perfectly correct. It was the wrong output entirely.
//
// So the checks here are written against the two things that actually decide
// what a viewer sees:
//
//   * the frame is always FULLY COVERED. At every magnification and every step
//     of the animation, the magnified item must reach every edge of the canvas.
//     A gap is black in the recording, which was the bug;
//   * the visible rectangle stays inside the picture, so a zoom in a corner
//     stops at the edge instead of showing what is not there.
//
// Plus the things that make it usable: scale is continuous rather than stepping
// in whole pixels, a settled zoom over a still cursor pushes nothing, and the
// pull-out outlives the toggle so it is not abandoned half-finished.
//
// Pure: the cursor is a parameter and the clock is a number.
#include "core/ZoomMode.hpp"

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

// THE check. The item is drawn at `pos` with size canvas*scale; the canvas is
// (0,0,W,H). Every canvas pixel must be inside the item, or the recording has
// black in it.
static bool coversCanvas(const ZoomTransform &xf, QSize canvas)
{
	const double right = xf.posX + canvas.width() * xf.scale;
	const double bottom = xf.posY + canvas.height() * xf.scale;
	// A hair of tolerance for float noise; a real gap is whole pixels wide.
	return xf.posX <= 0.01 && xf.posY <= 0.01 && right >= canvas.width() - 0.01 &&
	       bottom >= canvas.height() - 0.01;
}

// Run a zoom to completion and hand back the transform, checking coverage on
// every single frame along the way.
struct RunResult {
	ZoomTransform xf;
	bool coveredThroughout = true;
	int frames = 0;
	double minScale = 1e9, maxScale = 0.0;
};
static RunResult run(ZoomMode &z, QPoint cursor, const ZoomParams &p, qint64 &t, int frames)
{
	RunResult r;
	for (int i = 0; i < frames; ++i) {
		t += 16;
		z.tick(cursor, p, t);
		const ZoomTransform xf = z.transform();
		if (!coversCanvas(xf, z.canvas()))
			r.coveredThroughout = false;
		r.minScale = std::min(r.minScale, xf.scale);
		r.maxScale = std::max(r.maxScale, xf.scale);
		++r.frames;
	}
	r.xf = z.transform();
	return r;
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const QSize canvas(1920, 1080);

	std::printf("\n-- the magnification --\n");
	{
		ZoomParams p;
		ok(p.percent == 200 && std::abs(p.factor() - 2.0) < 1e-9, "the default is 200%% == 2x");
		ZoomParams lo;
		lo.percent = 10;
		ZoomParams hi;
		hi.percent = 5000;
		ok(std::abs(lo.factor() - ZoomParams::kMinPercent / 100.0) < 1e-9,
		   "a silly-small percent clamps up to the floor");
		ok(std::abs(hi.factor() - ZoomParams::kMaxPercent / 100.0) < 1e-9,
		   "a silly-large one clamps down");
		ZoomParams mid;
		mid.percent = 250;
		ok(std::abs(mid.factor() - 2.5) < 1e-9, "CONTROL: an in-range percent is untouched");
	}

	std::printf("\n-- the frame is never uncovered --\n");
	{
		// The bug, stated directly. At 2x centred, the item is 3840x2160 placed
		// at (-960,-540): it covers the canvas with a screen's worth to spare.
		// The old crop produced a 960x540 picture at (0,0) and three quarters
		// of black -- which is what this refuses to let happen again.
		ZoomMode z;
		z.setCanvas(canvas);
		ZoomParams p;
		z.toggle();
		qint64 t = 0;
		const RunResult r = run(z, QPoint(960, 540), p, t, 60);
		std::printf("     settled: scale %.3f at (%.1f, %.1f)\n", r.xf.scale, r.xf.posX,
			    r.xf.posY);
		ok(r.coveredThroughout,
		   "the canvas is fully covered on EVERY frame of the push-in — no black");
		ok(std::abs(r.xf.scale - 2.0) < 1e-6, "and it arrives at exactly 2x");
		ok(r.xf.posX < 0 && r.xf.posY < 0,
		   "with the item hanging off the top-left, which is what a zoom looks like");

		// CONTROL: the coverage check can actually fail. A transform that scales
		// but forgets to move the item -- the shape of the original bug, and the
		// most likely way to reintroduce it -- must be caught.
		ZoomTransform naive;
		naive.scale = 0.5; // what a crop-shaped mistake produces
		ok(!coversCanvas(naive, canvas),
		   "CONTROL: an item smaller than the canvas is caught as uncovered");
	}

	std::printf("\n-- covered at every magnification --\n");
	{
		bool allCovered = true;
		int worstPct = 0;
		for (int pct = ZoomParams::kMinPercent; pct <= ZoomParams::kMaxPercent; pct += 1) {
			ZoomMode z;
			z.setCanvas(canvas);
			ZoomParams p;
			p.percent = pct;
			z.toggle();
			qint64 t = 0;
			const RunResult r = run(z, QPoint(400, 900), p, t, 80);
			if (!r.coveredThroughout) {
				allCovered = false;
				worstPct = pct;
			}
		}
		std::printf("     swept 110..400%%%s\n",
			    allCovered ? "" : (" — first gap at " + std::to_string(worstPct) + "%").c_str());
		ok(allCovered, "every whole percent, all the way in, off-centre, stays covered");

		// A 16:10 canvas, where the halves are not round numbers.
		ZoomMode z;
		z.setCanvas(QSize(1680, 1050));
		ZoomParams p;
		z.toggle();
		qint64 t = 0;
		ok(run(z, QPoint(200, 200), p, t, 80).coveredThroughout,
		   "and on a 16:10 canvas with the cursor near a corner");
	}

	std::printf("\n-- the push-in does not jump to the centre first --\n");
	{
		// The bug: the focus used to be clamped to the rectangle legal AT THE
		// CURRENT SCALE, and on the first frame -- scale barely above 1 -- the
		// only legal focus is the screen centre. So every zoom snapped to the
		// middle and then slid out to the mouse.
		//
		// The property that makes that impossible: whatever is under the cursor
		// stays exactly under the cursor the whole way in. Its screen position
		// is C/2 + (a - focus) * s, which for a point away from the edges must
		// come back as the point itself on EVERY frame.
		const QPoint cursor(1400, 800); // off-centre, but not near an edge
		ZoomMode z;
		z.setCanvas(canvas);
		ZoomParams p;
		p.follow.smoothness = 0;
		z.toggle();
		qint64 t = 0;
		double worstDrift = 0.0;
		double firstFrameDrift = -1.0;
		for (int i = 0; i < 60; ++i) {
			t += 16;
			z.tick(cursor, p, t);
			const ZoomTransform xf = z.transform();
			// Where the anchored source pixel is drawn on the canvas.
			const double sx = xf.posX + cursor.x() * xf.scale;
			const double sy = xf.posY + cursor.y() * xf.scale;
			const double drift = std::max(std::abs(sx - cursor.x()), std::abs(sy - cursor.y()));
			if (firstFrameDrift < 0)
				firstFrameDrift = drift;
			worstDrift = std::max(worstDrift, drift);
		}
		std::printf("     anchored pixel drifts at most %.3f px (first frame %.3f)\n",
			    worstDrift, firstFrameDrift);
		ok(firstFrameDrift < 1.0, "the FIRST frame does not throw the picture at the centre");
		ok(worstDrift < 1.0, "and the pixel under the cursor holds still for the whole push-in");

		// CONTROL: the drift measure can detect a jump. A centre-anchored zoom
		// -- which is what the bug produced -- moves that pixel a long way.
		const double centreAnchored =
			std::abs((canvas.width() / 2.0 + (cursor.x() - canvas.width() / 2.0) * 2.0) -
				 cursor.x());
		std::printf("     a centre-anchored 2x would move it %.0f px\n", centreAnchored);
		ok(centreAnchored > 100.0, "CONTROL: the same measure is large for a centred zoom");
	}

	std::printf("\n-- and the pull-out retraces it --\n");
	{
		// Reversing the animation must put the picture back the way it came,
		// not slide off somewhere else on the way out.
		const QPoint cursor(600, 300);
		ZoomMode z;
		z.setCanvas(canvas);
		ZoomParams p;
		p.follow.smoothness = 0;
		z.toggle();
		qint64 t = 0;
		run(z, cursor, p, t, 60);
		z.toggle(); // back out, cursor held still
		double worstDrift = 0.0;
		for (int i = 0; i < 60; ++i) {
			t += 16;
			z.tick(cursor, p, t);
			const ZoomTransform xf = z.transform();
			const double sx = xf.posX + cursor.x() * xf.scale;
			const double sy = xf.posY + cursor.y() * xf.scale;
			worstDrift = std::max(worstDrift,
					      std::max(std::abs(sx - cursor.x()), std::abs(sy - cursor.y())));
		}
		std::printf("     drift on the way out: %.3f px\n", worstDrift);
		ok(worstDrift < 1.0, "the anchored pixel holds still on the way out too");
		ok(z.transform().identity(), "and it lands back at the identity");
	}

	std::printf("\n-- the SECOND zoom of a recording --\n");
	{
		// The bug this section exists for, and the reason the checks above did
		// not catch it: they toggle and tick in the same breath, which is the
		// one case that always worked.
		//
		// The caller's clock runs for the whole recording, but the 60 Hz tick
		// only runs while there is something to animate. Sit still for ten
		// seconds between zooms and the next tick arrives with a ten-second dt
		// -- enough to advance a 350 ms animation to "done" in a single step.
		// The picture snapped straight to full magnification at the mouse.
		const auto framesToArrive = [&](ZoomMode &z, QPoint cursor, const ZoomParams &p,
						qint64 &t) {
			z.toggle();
			int n = 0;
			while (z.transform().scale < p.factor() - 1e-6 && n < 400) {
				t += 16;
				z.tick(cursor, p, t);
				++n;
			}
			return n;
		};
		ZoomMode z;
		z.setCanvas(canvas);
		ZoomParams p; // 350 ms
		qint64 t = 0;
		const QPoint cursor(1200, 700);

		const int first = framesToArrive(z, cursor, p, t);
		// Back out, then sit idle for ten seconds of wall clock with the timer
		// stopped -- exactly what the app does once the zoom settles.
		z.toggle();
		for (int i = 0; i < 40; ++i) {
			t += 16;
			z.tick(cursor, p, t);
		}
		t += 10000;

		const int second = framesToArrive(z, cursor, p, t);
		std::printf("     first zoom took %d frames, second took %d\n", first, second);
		ok(first > 10, "the first zoom is animated");
		ok(second > 10, "and so is the second, after ten idle seconds — not one frame");
		ok(std::abs(first - second) <= 3, "both take about the same time");

		// And the anchor still holds on that second zoom: a one-frame jump
		// would have parked the anchored pixel correctly at the END, so
		// measuring only the destination would have missed this entirely.
		// Watching every frame is what makes it visible.
		ZoomMode z2;
		z2.setCanvas(canvas);
		qint64 t2 = 0;
		framesToArrive(z2, cursor, p, t2);
		z2.toggle();
		for (int i = 0; i < 40; ++i) {
			t2 += 16;
			z2.tick(cursor, p, t2);
		}
		t2 += 10000;
		z2.toggle();
		double worst = 0.0;
		int midFrames = 0;
		for (int i = 0; i < 60; ++i) {
			t2 += 16;
			z2.tick(cursor, p, t2);
			const ZoomTransform xf = z2.transform();
			worst = std::max(worst, std::abs(xf.posX + cursor.x() * xf.scale - cursor.x()));
			if (xf.scale > 1.02 && xf.scale < p.factor() - 0.02)
				++midFrames;
		}
		std::printf("     second zoom: %d part-way frames, anchor drift %.3f px\n", midFrames,
			    worst);
		ok(midFrames > 5, "the second zoom really does pass through the middle");
		ok(worst < 1.0, "and the anchored pixel holds still on it too");
	}

	std::printf("\n-- a stalled frame is not elapsed time --\n");
	{
		// A disk hitch or a long encoder frame must slow the animation down,
		// never skip it forward.
		ZoomMode z;
		z.setCanvas(canvas);
		ZoomParams p;
		z.toggle();
		qint64 t = 0;
		t += 16;
		z.tick(QPoint(1200, 700), p, t);
		const double afterOne = z.transform().scale;
		t += 5000; // the machine went away for five seconds
		z.tick(QPoint(1200, 700), p, t);
		const double afterStall = z.transform().scale;
		std::printf("     scale %.3f -> %.3f across a 5 s stall\n", afterOne, afterStall);
		ok(afterStall < p.factor() - 0.01, "the stall did not finish the zoom in one step");
		ok(afterStall > afterOne, "but it did move it along");
	}

	std::printf("\n-- the visible rectangle --\n");
	{
		ZoomMode z;
		z.setCanvas(canvas);
		ZoomParams p;
		z.toggle();
		qint64 t = 0;
		run(z, QPoint(960, 540), p, t, 60);
		const QRect v = z.visibleRect();
		std::printf("     2x centred shows %dx%d at (%d,%d)\n", v.width(), v.height(), v.x(),
			    v.y());
		ok(std::abs(v.width() - 960) <= 1 && std::abs(v.height() - 540) <= 1,
		   "2x shows half the width and half the height");
		ok(QRect(QPoint(0, 0), canvas).contains(v.adjusted(1, 1, -1, -1)),
		   "and it is inside the picture");
		// Aspect: the visible rectangle is the canvas over one number, so it
		// keeps the canvas shape by construction. Worth pinning anyway -- a
		// non-uniform scale would stretch faces and nothing else would report it.
		const double a = double(v.width()) / v.height();
		const double c = double(canvas.width()) / canvas.height();
		ok(std::abs(a - c) / c < 0.005, "with the canvas aspect intact");
	}

	std::printf("\n-- corners --\n");
	{
		// The cursor parked in a corner. The visible rectangle must stop at the
		// edge, not run past it into pixels that do not exist.
		for (const QPoint &corner : {QPoint(0, 0), QPoint(1919, 0), QPoint(0, 1079),
					     QPoint(1919, 1079)}) {
			ZoomMode z;
			z.setCanvas(canvas);
			ZoomParams p;
			p.follow.smoothness = 0; // instant, so it settles where it aims
			z.toggle();
			qint64 t = 0;
			const RunResult r = run(z, corner, p, t, 200);
			const QRect v = z.visibleRect();
			const bool inside = v.x() >= -1 && v.y() >= -1 &&
					    v.x() + v.width() <= canvas.width() + 1 &&
					    v.y() + v.height() <= canvas.height() + 1;
			std::printf("     cursor (%d,%d) -> %dx%d at (%d,%d)\n", corner.x(),
				    corner.y(), v.width(), v.height(), v.x(), v.y());
			ok(inside && r.coveredThroughout, "a zoom in a corner stays on the picture");
		}
	}

	std::printf("\n-- the scale is continuous --\n");
	{
		// The old crop stepped in whole pixels, so a slow push-in ratcheted. A
		// float scale should produce a different value on essentially every
		// frame of a long animation.
		ZoomMode z;
		z.setCanvas(canvas);
		ZoomParams p;
		p.animMs = 1000;
		z.toggle();
		qint64 t = 0;
		double prev = -1;
		int distinct = 0, steps = 0;
		while (z.transform().scale < 1.999 && steps < 200) {
			t += 16;
			z.tick(QPoint(960, 540), p, t);
			const double s = z.transform().scale;
			if (std::abs(s - prev) > 1e-9)
				++distinct;
			prev = s;
			++steps;
		}
		std::printf("     %d distinct scales over %d frames\n", distinct, steps);
		ok(steps > 40, "a 1000 ms zoom really does take about a second");
		ok(distinct >= steps - 1, "and essentially every frame is a new scale, not a step");
	}

	std::printf("\n-- following the cursor --\n");
	{
		ZoomMode z;
		z.setCanvas(canvas);
		ZoomParams p;
		p.follow.smoothness = 0;
		z.toggle();
		qint64 t = 0;
		run(z, QPoint(960, 540), p, t, 60);
		const QRect centred = z.visibleRect();
		run(z, QPoint(1700, 540), p, t, 120);
		const QRect chased = z.visibleRect();
		std::printf("     centred x=%d -> chased x=%d\n", centred.x(), chased.x());
		ok(chased.x() > centred.x(), "the frame followed the cursor right");
		ok(chased.y() == centred.y(), "and held its row");
		ok(std::abs(chased.width() - centred.width()) <= 1,
		   "the magnification did not change while it panned");

		// CONTROL: inside the dead zone, nothing moves.
		ZoomMode z2;
		z2.setCanvas(canvas);
		z2.toggle();
		t = 0;
		run(z2, QPoint(960, 540), p, t, 60);
		const QRect before = z2.visibleRect();
		run(z2, QPoint(975, 548), p, t, 60);
		ok(z2.visibleRect() == before, "CONTROL: a nudge inside the dead zone moves nothing");
	}

	std::printf("\n-- follow can be switched off --\n");
	{
		// Region recording with Follow Mouse on: the region is already tracking
		// the cursor, so the zoom must not track it too.
		ZoomMode z;
		z.setCanvas(canvas);
		ZoomParams p;
		p.followCursor = false;
		p.follow.smoothness = 0;
		z.toggle();
		qint64 t = 0;
		run(z, QPoint(960, 540), p, t, 60);
		const QRect start = z.visibleRect();
		run(z, QPoint(1900, 1000), p, t, 120);
		std::printf("     centre %dx%d at (%d,%d) after the cursor ran to the corner\n",
			    z.visibleRect().width(), z.visibleRect().height(), z.visibleRect().x(),
			    z.visibleRect().y());
		ok(z.visibleRect() == start, "the frame ignored the cursor entirely");
		ok(coversCanvas(z.transform(), canvas), "and still covers the canvas");
	}

	std::printf("\n-- settling and toggling off --\n");
	{
		ZoomMode z;
		z.setCanvas(canvas);
		ZoomParams p;
		z.toggle();
		qint64 t = 0;
		run(z, QPoint(960, 540), p, t, 60);

		int churn = 0;
		for (int i = 0; i < 200; ++i) {
			t += 16;
			if (z.tick(QPoint(960, 540), p, t))
				++churn;
		}
		std::printf("     updates over 200 idle ticks: %d\n", churn);
		ok(churn == 0, "a settled zoom over a still cursor pushes nothing to the scene");

		ok(!z.toggle(), "the second press reports unzoomed");
		ok(z.isBusy(), "but it is still busy — the caller must tick through the pull-out");
		bool sawPartial = false;
		for (int i = 0; i < 60; ++i) {
			t += 16;
			z.tick(QPoint(960, 540), p, t);
			const double s = z.transform().scale;
			if (s > 1.05 && s < 1.95)
				sawPartial = true;
			if (!coversCanvas(z.transform(), canvas))
				ok(false, "covered on the way out too");
		}
		ok(sawPartial, "the way out is animated, not a cut");
		ok(!z.isBusy(), "and it eventually stops being busy");
		ok(z.transform().identity(), "ending fully zoomed out is the IDENTITY transform");
	}

	std::printf("\n-- a new recording starts clean --\n");
	{
		ZoomMode z;
		z.setCanvas(canvas);
		ZoomParams p;
		z.toggle();
		qint64 t = 0;
		run(z, QPoint(300, 300), p, t, 40);
		ok(z.isZoomed() && !z.transform().identity(), "zoomed mid-recording");
		z.setCanvas(canvas);
		ok(!z.isZoomed() && !z.isBusy(), "the next recording starts unzoomed");
		ok(z.transform().identity(), "with no magnification left over — read BEFORE any tick");
		ok(!z.tick(QPoint(300, 300), p, t + 16), "and its first tick changes nothing");
	}

	std::printf("\n-- a region-sized canvas --\n");
	{
		// Zoom works in Custom Region too now: the canvas is the region rather
		// than the display, and nothing else about it differs.
		const QSize region(720, 1280); // a tall mobile strip
		ZoomMode z;
		z.setCanvas(region);
		ZoomParams p;
		z.toggle();
		qint64 t = 0;
		const RunResult r = run(z, QPoint(360, 640), p, t, 80);
		ok(r.coveredThroughout, "a portrait region canvas is covered throughout");
		const QRect v = z.visibleRect();
		ok(std::abs(v.width() - 360) <= 1 && std::abs(v.height() - 640) <= 1,
		   "and 2x means half of the REGION, not half of a display");
	}

	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "all zoom mode checks passed",
		    failures);
	return failures ? 1 : 0;
}
