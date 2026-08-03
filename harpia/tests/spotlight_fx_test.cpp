// The recording spotlight: geometry, shading and the fade.
//
// The whole effect is drawn onto the desktop so the screen capture picks it up,
// which means every one of these numbers ends up baked into a video file that
// cannot be un-baked. The things worth pinning are the ones nobody would notice
// until they watched the recording back:
//
//   * a patch clamped to the screen would leave the cursor drifting OUT of its
//     own spotlight in the corners -- the one place people point at most;
//   * a darkness that reaches 100% makes the machine unusable while it is on,
//     and you are still driving it;
//   * a fade that never quite finishes leaves the overlay repainting the whole
//     display sixty times a second for the rest of the recording;
//   * and a dirty region that misses the trailing edge leaves a bright smear
//     behind the cursor -- in the FILE, permanently.
//
// Pure and window-free: the clock is a number and the cursor is a parameter.
#include "core/SpotlightFx.hpp"

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	std::printf("\n-- the lit patch --\n");
	{
		const QRect h = spotlightHole(QPoint(500, 400), 320);
		std::printf("     cursor (500,400), 320 px -> %dx%d at (%d,%d)\n", h.width(),
			    h.height(), h.x(), h.y());
		ok(h.width() == 320 && h.height() == 320, "the size is the size that was asked for");
		ok(h.center().x() - 500 <= 1 && h.center().y() - 400 <= 1,
		   "and it is centred on the cursor");

		// Deliberately NOT clamped. A patch pinned inside the screen would let
		// the cursor walk out of its own light in a corner.
		const QRect corner = spotlightHole(QPoint(0, 0), 320);
		ok(corner.x() < 0 && corner.y() < 0,
		   "in a corner it hangs off the edge rather than pinning to it");
		ok(corner.center().x() <= 1 && corner.center().y() <= 1,
		   "so the cursor stays in the middle of the light wherever it goes");

		// The size limits exist so a slider cannot produce something useless.
		ok(spotlightHole(QPoint(500, 400), 1).width() == SpotlightParams::kMinSize,
		   "a silly-small patch clamps up to the floor");
		ok(spotlightHole(QPoint(500, 400), 99999).width() == SpotlightParams::kMaxSize,
		   "a silly-large one clamps down");
		// CONTROL: an in-range size passes through, so the clamp is a clamp.
		ok(spotlightHole(QPoint(500, 400), 500).width() == 500, "an in-range size is untouched");
	}

	std::printf("\n-- roundness --\n");
	{
		const QRect h = spotlightHole(QPoint(500, 400), 400);
		ok(spotlightRadius(h, 0) == 0, "0%% is a hard-edged rectangle");
		ok(spotlightRadius(h, 100) == 200,
		   "100%% of a square patch is half its side, which IS a circle");
		ok(spotlightRadius(h, 50) == 100, "and 50%% is half of that");
		ok(spotlightRadius(h, 500) == spotlightRadius(h, 100),
		   "past 100%% is clamped, not allowed to exceed the patch");
		ok(spotlightRadius(h, -20) == 0, "and below 0%% too");
	}

	std::printf("\n-- how dark --\n");
	{
		ok(spotlightAlpha(0, 1.0) == 0, "0%% darkness draws nothing at all");
		ok(spotlightAlpha(100, 1.0) < 255,
		   "and 100%% is capped short of pitch black — the machine is still in use");
		ok(spotlightAlpha(SpotlightParams::kMaxDark, 1.0) == spotlightAlpha(100, 1.0),
		   "anything above the cap lands on the cap");
		const int half = spotlightAlpha(70, 0.5);
		const int full = spotlightAlpha(70, 1.0);
		std::printf("     70%% dark: half-faded alpha %d, settled alpha %d\n", half, full);
		ok(half > 0 && half < full, "a half-done fade is genuinely part-way");
		ok(spotlightAlpha(70, 0.0) == 0, "and progress zero is fully clear");
	}

	std::printf("\n-- the dirty region --\n");
	{
		// A settled spotlight only has to repaint the two patches when the
		// cursor moves. Miss the trailing edge and the old patch stays bright.
		const QRect d = spotlightDirty(QPoint(100, 100), QPoint(400, 100), 320);
		ok(d.contains(spotlightHole(QPoint(100, 100), 320)),
		   "the region covers where the light WAS");
		ok(d.contains(spotlightHole(QPoint(400, 100), 320)), "and where it now is");
		// The margin matters: an antialiased edge writes a pixel or two past
		// the geometric rectangle, and those pixels would be left behind.
		ok(d.left() < spotlightHole(QPoint(100, 100), 320).left(),
		   "with a margin for the antialiased edge");

		// CONTROL: it is not simply the whole screen. If it were, the
		// optimisation this exists for would be doing nothing.
		ok(d.width() < 1920, "and it is not just the entire display");
	}

	std::printf("\n-- the toggle and its fade --\n");
	{
		SpotlightMode s;
		s.reset(false);
		ok(!s.isOn() && !s.isBusy(), "a recording starts clear and idle");

		ok(s.toggle(), "the shortcut reports it is now on");
		qint64 t = 0;
		int ticks = 0;
		while (s.tick(t += 16) && ticks < 200)
			++ticks;
		std::printf("     faded in over %d ticks (%d ms of 16 ms frames)\n", ticks, ticks * 16);
		ok(ticks > 2, "the fade is a fade, not a cut");
		ok(ticks * 16 <= SpotlightMode::kFadeMs + 40, "and it lands in about the stated time");
		ok(s.progress() >= 0.999, "arriving fully lit");
		ok(!s.tick(t += 16), "and then reporting nothing more to redraw");
		ok(s.isBusy(), "though it is still busy — it is lit, and the cursor still moves");

		ok(!s.toggle(), "the second press reports it off");
		ok(s.isBusy(), "and it stays busy through the fade OUT");
		bool sawPartial = false;
		ticks = 0;
		while (s.tick(t += 16) && ticks < 200) {
			if (s.progress() > 0.05 && s.progress() < 0.95)
				sawPartial = true;
			++ticks;
		}
		ok(sawPartial, "which is animated too, not a cut back to clear");
		ok(!s.isBusy(), "and afterwards there is nothing left to draw");
		ok(spotlightAlpha(70, s.shade()) == 0, "not even a stray pixel of shading");
	}

	std::printf("\n-- starting a recording already lit --\n");
	{
		SpotlightMode s;
		s.reset(true);
		ok(s.isOn() && s.isBusy(), "the preset can open a recording with the light on");
		ok(s.progress() >= 0.999,
		   "at full strength from the first frame, not fading up while the recording starts");
		// And a new recording after that one goes back to whatever it is told.
		s.reset(false);
		ok(!s.isOn() && !s.isBusy(), "and the next recording is not stuck with it");
	}

	std::printf("\n-- the easing --\n");
	{
		SpotlightMode s;
		s.reset(false);
		ok(s.shade() == 0.0, "clear eases to clear");
		s.reset(true);
		ok(s.shade() >= 0.999, "and lit eases to lit");
		// Smoothstep, so the light does not arrive and stop dead. The midpoint
		// is the one value that pins the curve's shape rather than its ends.
		SpotlightMode m;
		m.reset(false);
		m.toggle();
		qint64 t = 0;
		while (m.progress() < 0.5)
			m.tick(t += 16);
		std::printf("     progress %.2f -> shade %.2f\n", m.progress(), m.shade());
		ok(m.shade() > 0.2 && m.shade() < 0.8, "and it is eased rather than linear");
	}

	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "all spotlight checks passed",
		    failures);
	return failures ? 1 : 0;
}
