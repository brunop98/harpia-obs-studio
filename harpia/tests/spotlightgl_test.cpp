// The GPU spotlight must agree with the CPU one.
//
// Preview and export both go through Spotlight::apply, and whichever path it
// picks has to produce the same picture, or "what you see is what you get"
// quietly stops being true — on one machine and not another, which is the worst
// kind of bug to be told about. So this renders the same spec both ways and
// holds the results against each other.
//
// Needs a GL 3.3 context. Under xvfb-run with llvmpipe that is software
// rendering, which is fine: this is checking agreement, not speed.
#include "editor/shader/SpotlightGl.hpp"
#include "editor/timeline/Spotlight.hpp"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

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

// Content with gradients AND a hard edge: a flat field would let almost any
// blur bug through.
static QImage scene(int w, int h)
{
	QImage im(w, h, QImage::Format_RGBA8888);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			int v = (x * 255) / w;
			if (x > w / 2 && y > h / 3 && y < h * 2 / 3)
				v = 255 - v;
			im.setPixelColor(x, y, QColor(v, (y * 255) / h, 200 - v / 2));
		}
	return im;
}

struct Diff {
	double mean = 0;
	int worst = 0;
	double fracOver8 = 0;
};

// Edge pixels are where a rasterised path and an analytic shape can legitimately
// disagree by one pixel, so report the bulk statistics AND the worst case rather
// than asserting on a single number. A real mistake — a flipped axis, the wrong
// rotation sign, a mismatched sigma — moves the mean and the bulk fraction, not
// just a handful of boundary pixels.
static Diff compare(const QImage &a, const QImage &b)
{
	Diff d;
	double sum = 0;
	int n = 0, over = 0;
	for (int y = 0; y < a.height(); ++y)
		for (int x = 0; x < a.width(); ++x) {
			const QColor ca = a.pixelColor(x, y), cb = b.pixelColor(x, y);
			const int e = std::max({std::abs(ca.red() - cb.red()),
						std::abs(ca.green() - cb.green()),
						std::abs(ca.blue() - cb.blue())});
			sum += e;
			d.worst = std::max(d.worst, e);
			if (e > 8)
				++over;
			++n;
		}
	d.mean = n ? sum / n : 0;
	d.fracOver8 = n ? double(over) / n : 0;
	return d;
}

static void check(const char *what, const SpotlightSpec &spec, qint64 t, double meanTol,
		  double fracTol)
{
	QImage gpu = scene(640, 360);
	QImage cpu = gpu.copy();

	const bool ranOnGpu = SpotlightGl::tryApply(gpu, spec, t);
	if (!ranOnGpu) {
		std::printf("  SKIP %s (GPU declined)\n", what);
		return;
	}
	// applyCpu, not apply: apply() would dispatch straight back to the GPU and
	// this would be comparing a picture with itself -- which passes perfectly
	// and proves nothing.
	Spotlight::applyCpu(cpu, spec, t);

	const Diff d = compare(gpu, cpu);
	std::printf("  %-34s mean %5.2f  worst %3d  >8: %5.2f%%\n", what, d.mean, d.worst,
		    100.0 * d.fracOver8);
	ok(d.mean <= meanTol && d.fracOver8 <= fracTol, what);
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	// This box has llvmpipe and nothing else, and SpotlightGl refuses software
	// rasterisers on purpose (they are slower than the CPU path it replaces).
	// The override exists for exactly this: agreement is testable on llvmpipe
	// even though speed is not.
	qputenv("HARPIA_SPOTLIGHT_GL_FORCE", "1");
	QGuiApplication app(argc, argv);

	std::printf("\n-- is there a GPU to test against? --\n");
	const bool haveGl = SpotlightGl::available();
	std::printf("     GL available: %d\n", int(haveGl));
	if (!haveGl) {
		std::printf("\nSKIPPED: no OpenGL 3.3 here. Run under xvfb-run to exercise this.\n");
		return 0;
	}
	ok(haveGl, "an offscreen GL 3.3 context came up");

	std::printf("\n-- the shapes, with no blur --\n");
	// No blur means the only thing that can differ is the mask geometry, so any
	// disagreement here is a coordinate or shape bug and nothing else.
	for (int s = 0; s < kSpotShapeCount; ++s) {
		SpotlightSpec spec;
		spec.enabled = true;
		SpotMask m;
		m.shape = SpotShape(s);
		m.pose.cx = 0.42;
		m.pose.cy = 0.55; // off-centre, so a flipped axis cannot hide
		m.pose.w = 0.4;
		m.pose.h = 0.3;
		m.pose.radius = 0.25;
		spec.masks.append(m);
		check(spotShapeName(SpotShape(s)), spec, 0, 0.6, 0.004);
	}

	std::printf("\n-- rotation, where a sign error lives --\n");
	for (double deg : {30.0, -55.0}) {
		SpotlightSpec spec;
		spec.enabled = true;
		SpotMask m;
		m.shape = SpotShape::Rect;
		m.pose.cx = 0.4;
		m.pose.cy = 0.45;
		m.pose.w = 0.45;
		m.pose.h = 0.18; // long and thin: rotating it the wrong way is obvious
		m.pose.rotation = deg;
		spec.masks.append(m);
		char buf[64];
		std::snprintf(buf, sizeof buf, "rotated %+.0f degrees", deg);
		check(buf, spec, 0, 1.5, 0.02);
	}

	std::printf("\n-- inverted, several areas, a coloured dim --\n");
	{
		SpotlightSpec spec;
		spec.enabled = true;
		spec.invert = true;
		spec.dimOpacity = 0.8;
		spec.dimColor = QColor(20, 90, 160);
		for (int i = 0; i < 3; ++i) {
			SpotMask m;
			m.shape = SpotShape::Circle;
			m.pose.cx = 0.25 + 0.25 * i;
			m.pose.cy = 0.4 + 0.1 * i;
			m.pose.w = 0.18;
			spec.masks.append(m);
		}
		check("inverted, 3 circles, blue dim", spec, 0, 0.6, 0.004);
	}

	std::printf("\n-- with the blur, which is the whole reason for this --\n");
	// The looser tolerance is honest: the CPU runs three box passes and the GPU
	// a Gaussian of the matching sigma. They are approximations of each other by
	// construction, not the same filter, so they differ slightly in the falloff.
	for (double b : {0.35, 1.0}) {
		SpotlightSpec spec;
		spec.enabled = true;
		spec.blur = b;
		spec.dimOpacity = 0.5;
		SpotMask m;
		m.shape = SpotShape::RoundRect;
		m.pose.cx = 0.45;
		m.pose.cy = 0.5;
		m.pose.w = 0.35;
		m.pose.h = 0.35;
		m.pose.radius = 0.3;
		spec.masks.append(m);
		char buf[64];
		std::snprintf(buf, sizeof buf, "blur %.2f", b);
		check(buf, spec, 0, 4.0, 0.10);
	}

	std::printf("\n-- and it refuses rather than truncating --\n");
	{
		SpotlightSpec spec;
		spec.enabled = true;
		for (int i = 0; i < kMaxGlMasks + 1; ++i) {
			SpotMask m;
			m.pose.cx = 0.05 + 0.05 * i;
			m.pose.w = 0.05;
			spec.masks.append(m);
		}
		QImage f = scene(640, 360);
		ok(!SpotlightGl::tryApply(f, spec, 0),
		   "more areas than the shader holds falls back instead of dropping some");
	}
	{
		// Small frames are not worth the round trip, and the CPU is already
		// quick there.
		SpotlightSpec spec;
		spec.enabled = true;
		spec.masks.append(Spotlight::preset(QStringLiteral("Spotlight")));
		QImage tiny = scene(64, 48);
		ok(!SpotlightGl::tryApply(tiny, spec, 0), "a tiny frame stays on the CPU");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
