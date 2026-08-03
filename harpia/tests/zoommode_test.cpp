// Automatic Zoom: the animated crop that IS the zoom.
//
// In Full Screen the encoder's canvas is nailed to the display resolution at
// record start, so handing libobs a smaller crop makes it scale that rectangle
// back up to fill the canvas. That scaling is the whole feature -- which makes
// the failure modes geometric rather than visual, and invisible until someone
// watches the finished file:
//
//   * a crop whose aspect drifts from the canvas is STRETCHED on the way back
//     up. A 1% drift is not obvious on a still frame and is very obvious on a
//     face, so the aspect check here is tight enough to catch it (see the
//     control, which stretches by 1% deliberately and must fail);
//   * a crop that runs off the screen near a corner records pixels that do not
//     exist;
//   * a zoom-out that leaves a canvas-SIZED crop behind is not the same thing
//     as no crop at all -- it pins the recording to a resampled copy of itself
//     for the rest of the file;
//   * and a caller that stops ticking the moment the toggle goes off freezes
//     the frame part-way out, which is why isBusy() must outlive isZoomed().
//
// Pure: the cursor is a parameter and the clock is a number, so no window, no
// libobs and no real mouse -- the same shape as followmouse_test.
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

// The aspect of a rectangle, and how far two aspects are apart in relative
// terms. Rounding to whole pixels can move the ratio by about half a pixel on
// each axis; at 1080p that is ~0.15%, so 0.5% is comfortably above the noise
// and comfortably below a distortion anyone would notice.
static double aspect(QSize s)
{
	return double(s.width()) / double(s.height());
}
static double aspectDrift(QSize s, QSize canvas)
{
	return std::abs(aspect(s) - aspect(canvas)) / aspect(canvas);
}
static constexpr double kAspectTol = 0.005;

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const QSize canvas(1920, 1080);

	std::printf("\n-- the magnification --\n");
	{
		ZoomParams p;
		ok(p.percent == 200 && std::abs(p.factor() - 2.0) < 1e-9,
		   "the default is 200%% == 2x");
		const QSize two = zoomedSize(canvas, 2.0);
		std::printf("     1920x1080 at 2x -> %dx%d\n", two.width(), two.height());
		ok(two == QSize(960, 540), "2x captures exactly half the width AND half the height");

		// "2x" has to mean the same thing on both axes or it is not a zoom.
		ZoomParams p3;
		p3.percent = 150;
		ok(zoomedSize(canvas, p3.factor()) == QSize(1280, 720), "150%% is 1.5x on both axes");

		ok(zoomedSize(canvas, 0.5) == canvas,
		   "a factor below 1 cannot make the crop LARGER than the canvas");
		ok(zoomedSize(QSize(), 2.0).isEmpty(), "an empty canvas stays empty rather than 16x16");
	}

	std::printf("\n-- the percent clamp --\n");
	{
		ZoomParams lo;
		lo.percent = 10;
		ZoomParams hi;
		hi.percent = 5000;
		ok(std::abs(lo.factor() - ZoomParams::kMinPercent / 100.0) < 1e-9,
		   "a silly-small percent clamps up to the floor, not to a no-op zoom");
		ok(std::abs(hi.factor() - ZoomParams::kMaxPercent / 100.0) < 1e-9,
		   "a silly-large percent clamps down instead of cropping to 16x16 mush");
		// CONTROL: an in-range value passes through untouched, so the clamp is
		// clamping rather than pinning everything to one value.
		ZoomParams mid;
		mid.percent = 250;
		ok(std::abs(mid.factor() - 2.5) < 1e-9, "an in-range percent is not clamped");
	}

	std::printf("\n-- aspect is preserved at every magnification --\n");
	{
		bool allOk = true;
		double worst = 0.0;
		int worstPct = 0;
		for (int pct = ZoomParams::kMinPercent; pct <= ZoomParams::kMaxPercent; ++pct) {
			ZoomParams p;
			p.percent = pct;
			const double d = aspectDrift(zoomedSize(canvas, p.factor()), canvas);
			if (d > worst) {
				worst = d;
				worstPct = pct;
			}
			if (d > kAspectTol)
				allOk = false;
		}
		std::printf("     worst drift over 110..400%%: %.4f%% (at %d%%)\n", worst * 100.0,
			    worstPct);
		ok(allOk, "every whole percent keeps the canvas aspect within tolerance");

		// The same sweep on a 16:10 canvas -- a shape whose halves do not land
		// on round numbers, where a lazy "divide width, keep height" bug shows.
		const QSize wide(1680, 1050);
		bool wideOk = true;
		for (int pct = ZoomParams::kMinPercent; pct <= ZoomParams::kMaxPercent; ++pct) {
			ZoomParams p;
			p.percent = pct;
			if (aspectDrift(zoomedSize(wide, p.factor()), wide) > kAspectTol)
				wideOk = false;
		}
		ok(wideOk, "same on a 16:10 canvas, where the halves are not round numbers");

		// CONTROL: the check is tight enough to matter. A crop stretched by a
		// single percent on one axis -- the sort of thing a stray rounding of
		// only the width produces -- must FAIL the same tolerance.
		const QSize stretched(int(960 * 1.01), 540);
		ok(aspectDrift(stretched, canvas) > kAspectTol,
		   "CONTROL: a 1%% stretch is caught by that same tolerance");
	}

	std::printf("\n-- the push-in --\n");
	{
		ok(sizeForProgress(canvas, 2.0, 0.0) == canvas, "progress 0 is the whole canvas");
		ok(sizeForProgress(canvas, 2.0, 1.0) == QSize(960, 540), "progress 1 is the full zoom");
		ok(sizeForProgress(canvas, 2.0, 2.0) == QSize(960, 540),
		   "progress past 1 is clamped, not extrapolated into a 480x270 crop");

		// Monotone and aspect-clean the whole way in: the intermediate frames
		// are the ones a viewer actually watches.
		bool monotone = true, clean = true;
		QSize prev = canvas;
		for (int i = 0; i <= 100; ++i) {
			const QSize s = sizeForProgress(canvas, 2.0, i / 100.0);
			if (s.width() > prev.width() || s.height() > prev.height())
				monotone = false;
			if (aspectDrift(s, canvas) > kAspectTol)
				clean = false;
			prev = s;
		}
		ok(monotone, "the crop only ever shrinks on the way in -- no wobble");
		ok(clean, "every intermediate frame keeps the aspect too, not just the endpoints");
	}

	std::printf("\n-- toggling --\n");
	{
		ZoomMode z;
		z.setCanvas(canvas);
		ZoomParams p; // 200%, 350 ms
		ok(!z.isZoomed() && !z.isBusy(), "a fresh recording starts unzoomed and idle");
		ok(!z.region().enabled, "and with no crop at all");

		ok(z.toggle(), "the shortcut reports the new state: zoomed");
		ok(z.isZoomed(), "and the object agrees");

		// Push in. The cursor sits still in the middle of the screen.
		const QPoint mid(960, 540);
		qint64 t = 0;
		for (int i = 0; i < 40; ++i) { // 40 * 16 ms = 640 ms, well past 350
			t += 16;
			z.tick(mid, p, t);
		}
		const CaptureRegion in = z.region();
		std::printf("     after the animation: %dx%d at (%d,%d)\n", in.width, in.height, in.x,
			    in.y);
		ok(in.enabled, "the crop is on");
		ok(in.width == 960 && in.height == 540, "and has arrived at exactly 2x");
		ok(aspectDrift(QSize(in.width, in.height), canvas) <= kAspectTol,
		   "with the canvas aspect intact");

		// A settled zoom over a still cursor pushes nothing further. This is
		// the crop-filter-thrash check: the first Follow Mouse draft failed
		// exactly here.
		int churn = 0;
		for (int i = 0; i < 200; ++i) {
			t += 16;
			if (z.tick(mid, p, t))
				++churn;
		}
		std::printf("     updates over 200 idle ticks: %d\n", churn);
		ok(churn == 0, "a settled zoom over a still cursor pushes no further updates");

		// Back out.
		ok(!z.toggle(), "the second press reports unzoomed");
		ok(!z.isZoomed(), "and the object agrees");
		ok(z.isBusy(), "but it is still BUSY -- the caller must keep ticking through the ease-out");

		bool sawIntermediate = false;
		for (int i = 0; i < 40; ++i) {
			t += 16;
			z.tick(mid, p, t);
			const CaptureRegion r = z.region();
			if (r.enabled && r.width > 960 && r.width < 1920)
				sawIntermediate = true;
		}
		ok(sawIntermediate, "the way out is animated, not a cut");
		ok(!z.isBusy(), "and it eventually stops being busy");
		ok(z.region() == CaptureRegion{},
		   "ending fully zoomed out means NO crop -- not a canvas-sized one");
	}

	std::printf("\n-- the crop stays on the screen --\n");
	{
		// The cursor parked in the top-left corner. A crop centred on it would
		// start at (-480,-270) and record pixels that do not exist.
		ZoomMode z;
		z.setCanvas(canvas);
		ZoomParams p;
		z.toggle();
		qint64 t = 0;
		// Long enough for the glide to settle, not just to be under way -- the
		// interesting claim is where it COMES TO REST against the edge.
		for (int i = 0; i < 400; ++i) {
			t += 16;
			z.tick(QPoint(0, 0), p, t);
		}
		CaptureRegion r = z.region();
		std::printf("     cursor at (0,0): %dx%d at (%d,%d)\n", r.width, r.height, r.x, r.y);
		ok(r.x >= 0 && r.y >= 0, "the crop does not run off the top-left");
		ok(r.x + r.width <= canvas.width() && r.y + r.height <= canvas.height(),
		   "nor off the bottom-right");
		ok(r.x == 0 && r.y == 0, "it sits flush in the corner rather than short of it");

		// And the opposite corner, where the clamp is the other bound.
		ZoomMode z2;
		z2.setCanvas(canvas);
		z2.toggle();
		t = 0;
		for (int i = 0; i < 400; ++i) {
			t += 16;
			z2.tick(QPoint(1919, 1079), p, t);
		}
		r = z2.region();
		std::printf("     cursor at (1919,1079): %dx%d at (%d,%d)\n", r.width, r.height, r.x,
			    r.y);
		ok(r.x + r.width <= canvas.width() && r.y + r.height <= canvas.height(),
		   "the crop does not run off the bottom-right either");
		ok(r.x == 960 && r.y == 540, "it sits flush in the corner rather than short of it");
	}

	std::printf("\n-- it follows the cursor while zoomed --\n");
	{
		ZoomMode z;
		z.setCanvas(canvas);
		ZoomParams p;
		p.follow.smoothness = 0; // instant, so the test measures aim not easing
		z.toggle();
		qint64 t = 0;
		for (int i = 0; i < 60; ++i) {
			t += 16;
			z.tick(QPoint(960, 540), p, t);
		}
		const CaptureRegion centred = z.region();

		// Walk the cursor right, well past the dead zone.
		for (int i = 0; i < 60; ++i) {
			t += 16;
			z.tick(QPoint(1700, 540), p, t);
		}
		const CaptureRegion chased = z.region();
		std::printf("     centred x=%d -> chased x=%d\n", centred.x, chased.x);
		ok(chased.x > centred.x, "the crop followed the cursor to the right");
		ok(chased.y == centred.y, "and did not drift vertically while the cursor held its row");
		ok(chased.width == centred.width && chased.height == centred.height,
		   "the SIZE never changed -- the encoder is committed to one frame size");

		// CONTROL: a cursor that stays inside the dead zone moves nothing, so
		// the check above measures following rather than any-motion-at-all.
		ZoomMode z3;
		z3.setCanvas(canvas);
		z3.toggle();
		t = 0;
		for (int i = 0; i < 60; ++i) {
			t += 16;
			z3.tick(QPoint(960, 540), p, t);
		}
		const CaptureRegion before = z3.region();
		for (int i = 0; i < 60; ++i) {
			t += 16;
			z3.tick(QPoint(980, 550), p, t); // a nudge, still well inside the padding
		}
		ok(z3.region() == before, "CONTROL: a nudge inside the dead zone moves nothing");
	}

	std::printf("\n-- the animation honours the configured speed --\n");
	{
		// The point of exposing "animation speed" in ms is that the number
		// means something. A 100 ms zoom must be done well before a 1000 ms one.
		const auto ticksToArrive = [&](int animMs) {
			ZoomMode z;
			z.setCanvas(canvas);
			ZoomParams p;
			p.animMs = animMs;
			z.toggle();
			qint64 t = 0;
			for (int i = 0; i < 400; ++i) {
				t += 16;
				z.tick(QPoint(960, 540), p, t);
				if (z.region().width == 960)
					return i + 1;
			}
			return 9999;
		};
		const int fast = ticksToArrive(100);
		const int slow = ticksToArrive(1000);
		std::printf("     100 ms took %d ticks, 1000 ms took %d ticks\n", fast, slow);
		ok(fast >= 5 && fast <= 12, "a 100 ms zoom lands in roughly 100 ms of 16 ms ticks");
		ok(slow > fast * 5, "and a 1000 ms zoom takes about ten times as long");
	}

	std::printf("\n-- a new recording starts clean --\n");
	{
		ZoomMode z;
		z.setCanvas(canvas);
		ZoomParams p;
		z.toggle();
		qint64 t = 0;
		for (int i = 0; i < 40; ++i) {
			t += 16;
			z.tick(QPoint(300, 300), p, t);
		}
		ok(z.isZoomed() && z.region().enabled, "zoomed mid-recording");
		z.setCanvas(canvas); // stop, then start again
		ok(!z.isZoomed() && !z.isBusy(), "the next recording starts unzoomed");
		// Read BEFORE ticking: this is what a caller arming the pipeline sees,
		// and a stale rectangle here starts the new file already cropped.
		ok(z.region() == CaptureRegion{}, "and with no crop left over from the last one");
		ok(!z.tick(QPoint(300, 300), p, t + 16), "and its first tick pushes no crop");
	}

	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "all zoom mode checks passed",
		    failures);
	return failures ? 1 : 0;
}
