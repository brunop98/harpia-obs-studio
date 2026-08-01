// Follow Mouse: the dead zone, the target, and the chase.
//
// While recording a Custom Region, the region pans to keep the cursor framed.
// The failure modes worth pinning are the quiet ones:
//
//   * a dead zone computed wrong makes the camera swim on every tiny mouse
//     move -- the exact thing the padding exists to prevent;
//   * a chase that is not frame-rate independent feels different on a busy
//     machine than on an idle one, and nobody can tune that;
//   * an integer lerp stalls one pixel short of its target forever, leaving
//     the cursor permanently half a step from where it should be;
//   * clamping applied after the step instead of at the target accumulates
//     error against the screen edge that gets "paid back" later as a lurch;
//   * and above all, the SIZE must never change -- the encoder is committed
//     to one frame size for the whole file.
//
// Everything is pure or injected (cursor as a parameter, clock as a number),
// same shape as regionwatch_test, so no window and no real mouse is needed.
#include "core/FollowMouse.hpp"

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

static CaptureRegion reg(int x, int y, int w, int h)
{
	CaptureRegion r;
	r.enabled = true;
	r.x = x;
	r.y = y;
	r.width = w;
	r.height = h;
	return r;
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const QSize screen(3840, 2160);

	std::printf("\n-- the dead zone --\n");
	{
		const CaptureRegion r = reg(1000, 500, 600, 1000); // a tall mobile strip
		const QRect z20 = FollowMouse::deadZone(r, 20);
		std::printf("     region 600x1000 at (1000,500), 20%% -> zone %dx%d at (%d,%d)\n",
			    z20.width(), z20.height(), z20.x(), z20.y());
		ok(z20 == QRect(1120, 700, 360, 600), "20%% insets each side by exactly 20%% of that axis");
		ok(FollowMouse::deadZone(r, 0) == QRect(1000, 500, 600, 1000),
		   "0%% padding means the whole region is safe");
		// 45 is the ceiling; anything sillier is clamped rather than letting the
		// zone invert and the region oscillate.
		ok(FollowMouse::deadZone(r, 90) == FollowMouse::deadZone(r, 45),
		   "padding beyond 45%% clamps instead of inverting the zone");
	}

	std::printf("\n-- the target: move exactly enough, never more --\n");
	{
		const CaptureRegion r = reg(1000, 500, 600, 1000);
		// Zone is x 1120..1479, y 700..1299 (half-open, 20%).
		ok(FollowMouse::targetShift(r, QPoint(1300, 1000), 20) == QPoint(0, 0),
		   "a cursor inside the zone asks for nothing -- the camera holds still");
		ok(FollowMouse::targetShift(r, QPoint(1479, 1299), 20) == QPoint(0, 0),
		   "including on the zone's last inside pixel");
		ok(FollowMouse::targetShift(r, QPoint(1480, 1000), 20) == QPoint(1, 0),
		   "one pixel past the right edge asks for exactly one pixel");
		ok(FollowMouse::targetShift(r, QPoint(1100, 1000), 20) == QPoint(-20, 0),
		   "20 px past the left edge asks for exactly -20");
		ok(FollowMouse::targetShift(r, QPoint(1300, 650), 20) == QPoint(0, -50),
		   "and the axes are independent");
		const QPoint d = FollowMouse::targetShift(r, QPoint(1000, 400), 20);
		ok(d.x() < 0 && d.y() < 0, "a diagonal escape asks for both");
	}

	std::printf("\n-- clamping: the region never leaves the screen --\n");
	{
		ok(FollowMouse::clampTopLeft(QPoint(-50, 100), QSize(600, 1000), screen) == QPoint(0, 100),
		   "pushed past the left edge, it stops at 0");
		ok(FollowMouse::clampTopLeft(QPoint(3500, 1300), QSize(600, 1000), screen) ==
			   QPoint(3240, 1160),
		   "and at the far corner, at screen minus region");
		ok(FollowMouse::clampTopLeft(QPoint(100, 100), QSize(5000, 1000), screen) == QPoint(0, 100),
		   "a region wider than the screen pins to 0 rather than going negative");
	}

	std::printf("\n-- smoothness: instant at 0, frame-rate independent above --\n");
	{
		ok(FollowMouse::stepAlpha(0, 16.0) == 1.0, "0 covers the whole distance in one step");
		const double a50 = FollowMouse::stepAlpha(50, 16.0);
		const double a100 = FollowMouse::stepAlpha(100, 16.0);
		std::printf("     alpha per 16 ms: s=50 %.4f, s=100 %.4f\n", a50, a100);
		ok(a50 > 0.0 && a50 < 1.0, "50 is a partial step");
		ok(a100 < a50, "and 100 is a smaller one -- higher smoothness moves less per tick");

		// The property that makes the feel independent of timer jitter: two
		// 8 ms steps must land exactly where one 16 ms step does. For an
		// exponential chase, remaining fractions multiply.
		const double one = 1.0 - FollowMouse::stepAlpha(70, 16.0);
		const double half = 1.0 - FollowMouse::stepAlpha(70, 8.0);
		std::printf("     remaining after 16 ms: %.6f; after 8+8 ms: %.6f\n", one, half * half);
		ok(std::abs(one - half * half) < 1e-9,
		   "two 8 ms steps land exactly where one 16 ms step does");
	}

	std::printf("\n-- the chase: converges, and the size never changes --\n");
	{
		FollowMouse f;
		f.arm(reg(1000, 500, 600, 1000));
		ok(f.armed(), "armed from a real region");

		// Cursor parks well right of the zone. Tick at a steady 16 ms until
		// still; the region must end with the cursor exactly on the zone edge.
		// A fixed 600-tick window (~10 s of chase), no early break: a tick with
		// no whole-pixel movement mid-glide is normal -- sub-pixel steps round
		// to the same crop rectangle -- so "one quiet tick" is not "settled".
		const QPoint cursor(2000, 1000);
		FollowParams p; // padding 20, smoothness 50, both axes
		qint64 now = 0;
		int moves = 0, ticks = 600;
		for (int i = 0; i < ticks; ++i) {
			now += 16;
			if (f.tick(cursor, QSize(3840, 2160), p, now))
				++moves;
		}
		const CaptureRegion r = f.region();
		const QRect zone = FollowMouse::deadZone(r, 20);
		std::printf("     settled after %d ticks (%d moves) at (%d,%d), zone right edge %d, "
			    "cursor x %d\n",
			    ticks, moves, r.x, r.y, zone.x() + zone.width() - 1, cursor.x());
		ok(moves > 1, "the approach took several steps -- it glided, it did not teleport");
		ok(zone.x() + zone.width() - 1 == cursor.x(),
		   "and it settled with the cursor exactly on the dead-zone edge, not one short");
		ok(r.width == 600 && r.height == 1000, "the SIZE is untouched -- only the position moved");
		ok(r.y == 500, "and it did not drift vertically for a horizontal escape");

		// Once settled, further ticks must report no movement at all: a chase
		// that keeps jittering around its target would re-push the crop filter
		// 60 times a second forever.
		bool quiet = true;
		for (int i = 0; i < 60; ++i) {
			now += 16;
			if (f.tick(cursor, QSize(3840, 2160), p, now))
				quiet = false;
		}
		ok(quiet, "settled means SETTLED: a parked cursor causes zero further updates");
	}

	std::printf("\n-- instant profile really is instant --\n");
	{
		FollowMouse f;
		f.arm(reg(1000, 500, 600, 1000));
		FollowParams p;
		p.smoothness = 0;
		f.tick(QPoint(2000, 1000), QSize(3840, 2160), p, 16);
		const QRect zone = FollowMouse::deadZone(f.region(), 20);
		ok(zone.x() + zone.width() - 1 == 2000, "smoothness 0 arrives in a single tick");
	}

	std::printf("\n-- axis lock --\n");
	{
		FollowParams p;
		p.axis = FollowAxis::Horizontal;
		FollowMouse f;
		f.arm(reg(1000, 500, 600, 1000));
		qint64 now = 0;
		for (int i = 0; i < 400; ++i)
			f.tick(QPoint(2000, 2100), QSize(3840, 2160), p, now += 16); // far right AND below
		const CaptureRegion r = f.region();
		std::printf("     horizontal-only, diagonal escape: (1000,500) -> (%d,%d)\n", r.x, r.y);
		ok(r.x > 1000, "horizontal-only follows the sideways escape");
		ok(r.y == 500, "and holds the vertical line whatever the cursor does");

		p.axis = FollowAxis::Vertical;
		FollowMouse g;
		g.arm(reg(1000, 500, 600, 1000));
		now = 0;
		for (int i = 0; i < 400; ++i)
			g.tick(QPoint(2000, 2100), QSize(3840, 2160), p, now += 16);
		ok(g.region().x == 1000 && g.region().y > 500, "vertical-only is the mirror image");
	}

	std::printf("\n-- the screen edge --\n");
	{
		// Cursor parked at the very corner of the screen: the region must stop
		// flush with the edge and then hold perfectly still -- clamping at the
		// target means no accumulated error and no jitter against the edge.
		FollowMouse f;
		f.arm(reg(3000, 1000, 600, 1000));
		FollowParams p;
		qint64 now = 0;
		for (int i = 0; i < 800; ++i)
			f.tick(QPoint(3839, 2159), QSize(3840, 2160), p, now += 16);
		const CaptureRegion r = f.region();
		std::printf("     chased into the corner: region at (%d,%d)\n", r.x, r.y);
		ok(r.x == 3840 - 600 && r.y == 2160 - 1000, "flush with the corner, exactly");
		bool quiet = true;
		for (int i = 0; i < 60; ++i)
			if (f.tick(QPoint(3839, 2159), QSize(3840, 2160), p, now += 16))
				quiet = false;
		ok(quiet, "and pinned there without jitter");
	}

	std::printf("\n-- rebase: a manual drag wins --\n");
	{
		FollowMouse f;
		f.arm(reg(1000, 500, 600, 1000));
		FollowParams p;
		f.tick(QPoint(2000, 1000), QSize(3840, 2160), p, 16); // start chasing right
		// The user grabs the frame and drops it somewhere else entirely.
		f.rebase(reg(200, 200, 600, 1000));
		// With the cursor now inside the new region's zone, nothing should move:
		// the follow adopted the drag instead of fighting its way back.
		const bool moved = f.tick(QPoint(500, 700), QSize(3840, 2160), p, 32);
		ok(!moved, "after a rebase the dragged position is home, not an error to correct");
		ok(f.region().x == 200 && f.region().y == 200, "and region() reports the adopted spot");
	}

	std::printf("\n-- disarmed and degenerate --\n");
	{
		FollowMouse f;
		FollowParams p;
		ok(!f.tick(QPoint(0, 0), QSize(3840, 2160), p, 16), "unarmed ticks do nothing");
		CaptureRegion dead;
		dead.enabled = false;
		f.arm(dead);
		ok(!f.armed(), "a disabled region cannot arm");
		f.arm(reg(0, 0, 0, 0));
		ok(!f.armed(), "nor can a zero-size one");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
