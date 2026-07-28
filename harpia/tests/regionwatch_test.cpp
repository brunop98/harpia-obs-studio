// Auto-stop when the pointer leaves the recording region.
//
// Two things here can be wrong in ways nobody notices until a recording ends by
// itself in the middle of a take. The coordinate conversion — a CaptureRegion is
// DEVICE pixels from its screen's top-left, QCursor::pos() is global LOGICAL
// pixels — is correct by accident on a single 1x display, which is exactly where
// it gets tried. And the timer has to survive the pointer wandering back in,
// which resets the count rather than pausing it.
#include "core/RegionWatch.hpp"

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static void eqp(QPoint got, QPoint want, const char *w)
{
	const bool good = got == want;
	std::printf("  %s %s (got %d,%d want %d,%d)\n", good ? "PASS" : "FAIL", w, got.x(), got.y(),
		    want.x(), want.y());
	if (!good)
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

	std::printf("\n-- the pointer, in the region's coordinates --\n");
	// Primary screen, no scaling: the identity case, and the one that hides bugs.
	eqp(RegionWatch::toRegionSpace(QPoint(100, 50), QPoint(0, 0), 1.0), QPoint(100, 50),
	    "1x primary screen is a straight pass-through");

	// HiDPI: the region is in device pixels, so the logical position scales up.
	eqp(RegionWatch::toRegionSpace(QPoint(100, 50), QPoint(0, 0), 2.0), QPoint(200, 100),
	    "at 2x the device position is double the logical one");

	// A second monitor to the right. The origin is LOGICAL, so it must come off
	// before the scaling — doing it the other way multiplies the offset too.
	eqp(RegionWatch::toRegionSpace(QPoint(2000, 300), QPoint(1920, 0), 1.0), QPoint(80, 300),
	    "a second screen's origin is subtracted");
	eqp(RegionWatch::toRegionSpace(QPoint(2000, 300), QPoint(1920, 0), 2.0), QPoint(160, 600),
	    "and subtracted BEFORE scaling, not after");

	std::printf("\n-- inside and outside --\n");
	const CaptureRegion r = reg(100, 100, 400, 300);
	ok(RegionWatch::contains(r, QPoint(100, 100)), "the top-left corner is inside");
	ok(RegionWatch::contains(r, QPoint(499, 399)), "the last recorded pixel is inside");
	ok(!RegionWatch::contains(r, QPoint(500, 399)), "one past the right edge is outside");
	ok(!RegionWatch::contains(r, QPoint(499, 400)), "one past the bottom edge is outside");
	ok(!RegionWatch::contains(r, QPoint(99, 250)), "left of it is outside");
	ok(!RegionWatch::contains(r, QPoint(250, 99)), "above it is outside");
	// A disabled region is full-screen capture, where there is no outside.
	ok(RegionWatch::contains(CaptureRegion{}, QPoint(-5000, 9999)),
	   "with no region set, everywhere counts as inside");

	std::printf("\n-- the countdown --\n");
	{
		RegionWatch w;
		w.setTimeoutSeconds(4);
		ok(!w.tick(true, false, 1000), "leaving does not stop immediately");
		ok(!w.tick(true, false, 3000), "nor after two seconds");
		ok(!w.tick(true, false, 4999), "nor a millisecond early");
		ok(w.tick(true, false, 5000), "but does at exactly four seconds out");
		ok(!w.tick(true, false, 9000), "and only asks once, not on every tick after");
	}

	std::printf("\n-- coming back resets it --\n");
	{
		RegionWatch w;
		w.setTimeoutSeconds(4);
		w.tick(true, false, 1000);
		w.tick(true, false, 3000);
		ok(!w.tick(true, true, 3500), "back inside: nothing happens");
		// The clock restarts rather than resuming; three seconds already spent
		// outside must not count towards the next trip.
		ok(!w.tick(true, false, 4000), "leaving again starts from zero");
		ok(!w.tick(true, false, 7500), "so the old elapsed time is not carried over");
		ok(w.tick(true, false, 8000), "and it takes a fresh four seconds");
	}

	std::printf("\n-- Off, and 0 --\n");
	{
		RegionWatch w; // default
		ok(!w.enabled(), "the default is off");
		ok(!w.tick(true, false, 1000) && !w.tick(true, false, 999000),
		   "off never stops, however long the pointer is away");
	}
	{
		// 0 is a real setting, not a synonym for off: stop the moment it leaves.
		// This is why Off cannot be 0 the way the idle timeout's is.
		RegionWatch w;
		w.setTimeoutSeconds(0);
		ok(w.enabled(), "0 seconds is an enabled setting, not off");
		ok(w.tick(true, false, 5000), "and stops on the very first tick outside");
	}

	std::printf("\n-- disarmed --\n");
	{
		RegionWatch w;
		w.setTimeoutSeconds(2);
		// Not recording / not region mode / paused all arrive as armed=false.
		ok(!w.tick(false, false, 1000), "while disarmed nothing fires");
		ok(!w.tick(false, false, 99000), "however long it stays outside");
		// And the time spent disarmed is not banked: arming starts the clock.
		ok(!w.tick(true, false, 99500), "arming starts the count from then");
		ok(!w.tick(true, false, 101000), "not from when the pointer first left");
		ok(w.tick(true, false, 101500), "so the full timeout is served");
	}

	std::printf("\n-- what the UI shows --\n");
	{
		RegionWatch w;
		w.setTimeoutSeconds(10);
		ok(w.outsideForMs(1000) < 0, "inside reads as -1");
		w.tick(true, false, 1000);
		ok(w.outsideForMs(4000) == 3000, "outside reports how long it has been");
		w.tick(true, true, 4000);
		ok(w.outsideForMs(4000) < 0, "and goes back to -1 on return");
	}

	std::printf("\n-- the dropdown's values are the ones asked for --\n");
	{
		const int want[] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20};
		bool same = std::size(kRegionWatchSeconds) == std::size(want);
		for (size_t i = 0; same && i < std::size(want); ++i)
			same = kRegionWatchSeconds[i] == want[i];
		ok(same, "Off, 0, 2, 4 ... 20");
		ok(kRegionWatchOff < 0, "and Off is not one of them");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
