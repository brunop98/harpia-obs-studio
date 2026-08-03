// In / Out times on components: the weight curve.
//
// The failure modes here are all quiet ones. An envelope that never reaches 1
// leaves the effect permanently at half strength and looks like the effect is
// simply weak. One that divides by a zero-length clip takes the render down.
// One that clips instead of scaling makes an effect vanish when a clip is
// trimmed shorter -- and the user blames the trim.
//
// Above all: both ramps at 0 is the DEFAULT, and it must mean "exactly what
// every existing project already does". That is the compatibility check.
#include "editor/component/ComponentEnvelope.hpp"

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static bool near(double a, double b, double tol = 1e-6)
{
	return std::abs(a - b) <= tol;
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	std::printf("\n-- the default is no envelope at all --\n");
	{
		// Every project written before this feature has in=out=0. If that is
		// not a flat 1, this change silently re-renders everyone's work.
		bool flat = true;
		for (qint64 t = 0; t <= 5000; t += 250)
			if (!near(componentWeight(t, 5000, 0, 0), 1.0))
				flat = false;
		ok(flat, "in=0, out=0 is a flat 1 across the whole clip -- existing work is untouched");
		ok(envelopeIsFull(componentWeight(0, 5000, 0, 0)),
		   "and it reports as FULL, so the renderer takes the no-work path");
	}

	std::printf("\n-- the three phases --\n");
	{
		const qint64 dur = 4000, in = 1000, out = 1000;
		std::printf("     t=0 %.3f  t=500 %.3f  t=1000 %.3f  t=2000 %.3f  t=3500 %.3f  t=4000 %.3f\n",
			    componentWeight(0, dur, in, out), componentWeight(500, dur, in, out),
			    componentWeight(1000, dur, in, out), componentWeight(2000, dur, in, out),
			    componentWeight(3500, dur, in, out), componentWeight(4000, dur, in, out));
		ok(near(componentWeight(0, dur, in, out), 0.0), "starts at nothing");
		ok(near(componentWeight(1000, dur, in, out), 1.0), "full strength exactly at the In time");
		ok(near(componentWeight(2000, dur, in, out), 1.0), "and holds there through the middle");
		ok(near(componentWeight(3000, dur, in, out), 1.0), "right up to where Out begins");
		ok(near(componentWeight(4000, dur, in, out), 0.0), "back to nothing at the very end");
		// Monotonic on each side -- a ramp that wobbles reads as a glitch.
		bool rising = true, falling = true;
		for (qint64 t = 0; t < 1000; t += 50)
			if (componentWeight(t + 50, dur, in, out) < componentWeight(t, dur, in, out))
				rising = false;
		for (qint64 t = 3000; t < 4000; t += 50)
			if (componentWeight(t + 50, dur, in, out) > componentWeight(t, dur, in, out))
				falling = false;
		ok(rising, "the In ramp only ever rises");
		ok(falling, "and the Out ramp only ever falls");
	}

	std::printf("\n-- smooth, not linear --\n");
	{
		// Ease-in-out means the midpoint of the ramp is 0.5 but the quarter
		// points are pulled toward the ends -- that is what removes the kink.
		const double quarter = componentWeight(250, 4000, 1000, 0);
		const double half = componentWeight(500, 4000, 1000, 0);
		std::printf("     25%% through the In ramp: %.3f (linear would be 0.250)\n", quarter);
		ok(near(half, 0.5, 1e-3), "the middle of the ramp is still half");
		ok(quarter < 0.25, "but a quarter of the way in is BELOW a quarter -- it eases");
	}

	std::printf("\n-- one-sided ramps --\n");
	{
		ok(near(componentWeight(0, 3000, 500, 0), 0.0), "In only: starts at nothing");
		ok(near(componentWeight(3000, 3000, 500, 0), 1.0),
		   "...and stays full to the end, with no Out ramp");
		ok(near(componentWeight(0, 3000, 0, 500), 1.0), "Out only: starts at full");
		ok(near(componentWeight(3000, 3000, 0, 500), 0.0), "...and falls away at the end");
	}

	std::printf("\n-- ramps that do not fit are SCALED, not clipped --\n");
	{
		// 3 s of ramps on a 2 s clip. Clipping would mean the effect never
		// reaches full strength and the user blames the trim; scaling keeps
		// the shape and still peaks in the middle.
		const qint64 dur = 2000, in = 2000, out = 1000;
		const double mid = componentWeight(dur * 2 / 3, dur, in, out); // the scaled peak
		std::printf("     2 s clip with 2 s in + 1 s out -> peak weight %.3f at the joint\n", mid);
		ok(near(mid, 1.0, 1e-3), "the effect still reaches full strength");
		ok(near(componentWeight(0, dur, in, out), 0.0), "still starts at nothing");
		ok(near(componentWeight(dur, dur, in, out), 0.0), "still ends at nothing");
		// And the ratio between the two ramps is preserved: 2:1 in, so the
		// peak sits two-thirds of the way through.
		ok(componentWeight(1200, dur, in, out) > componentWeight(600, dur, in, out),
		   "and the 2:1 shape survives the scaling");
	}

	std::printf("\n-- degenerate input cannot take the render down --\n");
	{
		ok(near(componentWeight(0, 0, 500, 500), 1.0),
		   "a zero-length clip returns 1 rather than dividing by zero");
		ok(near(componentWeight(-100, 4000, 1000, 1000), 0.0), "a time before the clip clamps");
		ok(near(componentWeight(99999, 4000, 1000, 1000), 0.0), "and one past its end clamps too");
		ok(near(componentWeight(2000, 4000, -500, -500), 1.0), "negative ramps are treated as none");
	}

	std::printf("\n-- the fast-path flags --\n");
	{
		// The renderer skips the blend entirely on the hold, which is most
		// frames; without that this feature would cost a full-canvas copy per
		// component per frame forever.
		ok(envelopeIsFull(componentWeight(2000, 4000, 500, 500)), "the hold is FULL: no blend needed");
		ok(!envelopeIsFull(componentWeight(250, 4000, 500, 500)), "mid-ramp is not");
		ok(envelopeIsEmpty(componentWeight(0, 4000, 500, 500)),
		   "and the very start is EMPTY: the component is skipped outright");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
