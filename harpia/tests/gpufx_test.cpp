// Effects on the GPU give the CPU's answer.
//
// Every effect is run both ways on the same frame -- the CPU pass as the
// reference, the GPU engine (GpuFxGl) as the subject -- and the largest
// difference in any channel of any pixel is reported and bounded. Needs a real
// OpenGL 3.3 context (run under Xvfb in the suite); with none, it says so and
// passes, since the app then uses the CPU path this compares against.
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ComponentStack.hpp"
#include "editor/component/ShaderComponent.hpp"
#include "editor/shader/GpuFx.hpp"
#include "editor/shader/GpuFxGl.hpp"
#include "editor/timeline/EffectClip.hpp"
#include "editor/timeline/Spotlight.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QTemporaryDir>
#include <QFile>

#include <cstdio>
#include <cstdlib>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static int maxDiff(const QImage &a0, const QImage &b0, bool alpha = true)
{
	const QImage a = a0.convertToFormat(QImage::Format_RGBA8888), b = b0.convertToFormat(QImage::Format_RGBA8888);
	if (a.size() != b.size())
		return 999;
	int mx = 0;
	for (int y = 0; y < a.height(); ++y) {
		const uchar *pa = a.constScanLine(y), *pb = b.constScanLine(y);
		for (int x = 0; x < a.width() * 4; ++x) {
			if (!alpha && (x & 3) == 3)
				continue;
			mx = std::max(mx, std::abs(pa[x] - pb[x]));
		}
	}
	return mx;
}

static QImage testFrame(QSize sz)
{
	QImage img(sz, QImage::Format_RGBA8888);
	for (int y = 0; y < sz.height(); ++y) {
		uchar *row = img.scanLine(y);
		for (int x = 0; x < sz.width(); ++x) {
			row[x * 4 + 0] = uchar((x * 255) / sz.width());
			row[x * 4 + 1] = uchar((y * 255) / sz.height());
			row[x * 4 + 2] = uchar(((x ^ y) * 7) & 255);
			row[x * 4 + 3] = 255;
		}
	}
	QPainter p(&img);
	p.fillRect(QRect(sz.width() / 4, sz.height() / 4, sz.width() / 3, sz.height() / 3), QColor(250, 245, 230));
	p.fillRect(QRect(sz.width() / 2, sz.height() / 2, 40, 30), QColor(10, 200, 60));
	return img;
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	qputenv("HARPIA_FORCE_GPU_FX", "1"); // the suite runs on a software GL; the app would not
	installGpuFx();
	GpuFx *g = gpuFx();
	if (!g) {
		std::printf("\n  (no OpenGL 3.3 here: the app uses the CPU path; nothing to compare)\n");
		std::printf("\nALL PASSED (0 failures)\n");
		return 0;
	}
	const QSize sz(640, 360);
	const QImage base = testFrame(sz);

	std::printf("\n-- every effect, GPU against CPU --\n");
	for (int t = 0; t < kFxTypeCount; ++t) {
		const FxType ft = FxType(t);
		if (ft == FxType::InverseSelection)
			continue;
		FxSpec fx;
		fx.type = ft;
		fx.params = fxDefaults(ft);
		for (auto it = fx.params.begin(); it != fx.params.end(); ++it)
			if (it.value() == 0.0)
				it.value() = 0.35;
		QImage cpu = base;
		Effects::applyCpu(cpu, fx, 0);
		QImage gpu;
		if (g->begin(base) && g->fx(t, fx.params))
			gpu = g->end();
		else
			g->end();
		const int d = maxDiff(cpu, gpu);
		std::printf("     %-22s largest difference %d/255\n", fxTypeName(ft), d);
		// Integer-exact passes: 1 of rounding. Hue and vignette do float maths
		// on either side: a couple of levels.
		const int allowed = (ft == FxType::HueShift || ft == FxType::Vignette || ft == FxType::Saturation) ? 3 : 1;
		ok(!gpu.isNull() && d <= allowed, fxTypeName(ft));
	}

	std::printf("\n-- the blur, a mask, and a chain in one trip --\n");
	{
		QImage cpu = base;
		Spotlight::blurInPlaceCpu(cpu, 9);
		QImage gpu;
		if (g->begin(base) && g->blur(9))
			gpu = g->end();
		ok(maxDiff(cpu, gpu) <= 1, "blur radius 9 matches blurInPlace");
		// A radius wide enough to take the running-sum path.
		for (int r : {40, 150}) {
			QImage c2 = base, g2;
			Spotlight::blurInPlaceCpu(c2, r);
			if (g->begin(base) && g->blur(r))
				g2 = g->end();
			std::printf("     blur radius %d (running sums): largest difference %d/255\n", r, maxDiff(c2, g2));
			ok(maxDiff(c2, g2) <= 1, r == 40 ? "a wide blur matches too" : "and one wider than the frame is tall");
		}

		QImage cover(sz, QImage::Format_RGBA8888);
		cover.fill(Qt::black);
		{
			QPainter p(&cover);
			p.setBrush(Qt::white);
			p.setPen(Qt::NoPen);
			p.drawEllipse(QRect(100, 60, 300, 200));
		}
		QImage soft = cover;
		Spotlight::blurInPlaceCpu(soft, 6);
		QImage cpuMask = base;
		for (int y = 0; y < sz.height(); ++y) {
			uchar *row = cpuMask.scanLine(y);
			const uchar *c = soft.constScanLine(y);
			for (int x = 0; x < sz.width(); ++x)
				row[x * 4 + 3] = uchar((int(row[x * 4 + 3]) * c[x * 4] + 127) / 255);
		}
		QImage gpuMask;
		if (g->begin(base) && g->maskAlpha(cover, 6, false))
			gpuMask = g->end();
		ok(maxDiff(cpuMask, gpuMask) <= 2, "a feathered mask cuts the same alpha");

		FxSpec a, b;
		a.type = FxType::Brightness;
		a.params = {{"amount", 0.1}};
		b.type = FxType::GaussianBlur;
		b.params = {{"radius", 0.2}};
		QImage cpuChain = base;
		Effects::applyCpu(cpuChain, a, 0);
		Effects::applyCpu(cpuChain, b, 0);
		QImage gpuChain;
		if (g->begin(base) && g->fx(int(a.type), a.params) && g->fx(int(b.type), b.params))
			gpuChain = g->end();
		ok(maxDiff(cpuChain, gpuChain) <= 2, "brightness then blur, uploaded once, read back once");
		ok(!g->active(), "and the session is over");
		ok(g->begin(base) && !g->begin(base), "a second begin inside a session is refused (callers fall back)");
		g->end();
	}

	std::printf("\n-- how much faster --\n");
	{
		const QImage big = testFrame(QSize(1920, 1080));
		FxSpec fx;
		fx.type = FxType::Glow;
		fx.params = fxDefaults(fx.type);
		QElapsedTimer tm;
		tm.start();
		for (int i = 0; i < 5; ++i) {
			QImage c = big;
			Effects::applyCpu(c, fx, 0);
		}
		const double cpuMs = tm.nsecsElapsed() / 5e6;
		tm.restart();
		for (int i = 0; i < 5; ++i) {
			g->begin(big);
			g->fx(int(fx.type), fx.params);
			g->end();
		}
		const double gpuMs = tm.nsecsElapsed() / 5e6;
		std::printf("     Glow at 1080p: CPU %.1f ms, GPU %.1f ms (a software GL here; a real GPU is far faster)\n",
			    cpuMs, gpuMs);
		ok(true, "timed");
	}

	std::printf("\n-- a clip's whole effect stack, as the app runs it --\n");
	{
		ComponentRegistry &reg = ComponentRegistry::instance();
		registerBuiltinComponents(reg);
		QTemporaryDir dir;
		QFile::copy(QStringLiteral(HARPIA_SRC "/editor/shader/presets/huecycle.frag"), dir.filePath(QStringLiteral("huecycle.frag")));
		ShaderComponents::loadFolder(dir.path(), reg);
		const auto comp = [](const char *id, QVariantMap props = {}) {
			ComponentInstance ci;
			ci.typeId = QString::fromLatin1(id);
			ci.instanceId = QString::fromLatin1(id);
			for (auto it = props.begin(); it != props.end(); ++it)
				ci.props.insert(it.key(), it.value());
			return ci;
		};
		QVector<ComponentInstance> list = {
			comp("harpia.fx.saturation", {{"amount", 0.4}}),
			comp("harpia.blur", {{"radius", 0.2}}),
			comp("harpia.shader.huecycle", {{"uSpeed", 0.25}, {"uSaturation", 1.0}}),
			comp("harpia.fx.vignette", {{"amount", 0.5}}),
			comp("harpia.mask", {{"feather", 0.05}}),
		};
		EvalContext ctx;
		ctx.tMs = 1000;
		ctx.durMs = 5000;
		ctx.fps = 30;
		const auto render = [&](const QVector<ComponentInstance> &l) {
			ComponentStack stack(l, reg, false);
			QImage f = base;
			stack.evaluatePixels(ctx, f);
			return f;
		};
		const GpuFxProvider installed = gpuFxProvider();
		gpuFxProvider() = nullptr; // the CPU path (the shader then uses its own renderer)
		const QImage cpu = render(list);
		gpuFxProvider() = installed;
		const QImage gpu = render(list);
		const int d = maxDiff(cpu, gpu);
		std::printf("     saturation, blur, a user shader, vignette, mask: largest difference %d/255\n", d);
		ok(d <= 3, "five effects in one GPU trip give the CPU path's picture");

		list[1].inMs = 2000; // the blur half-way through fading in at t = 1 s
		gpuFxProvider() = nullptr;
		const QImage cpuRamp = render(list);
		gpuFxProvider() = installed;
		const QImage gpuRamp = render(list);
		const int dr = maxDiff(cpuRamp, gpuRamp);
		std::printf("     with the blur half faded in: largest difference %d/255\n", dr);
		// QPainter's opacity is quantised to 8 bits before it blends; the GPU
		// blends in float. A frame that is mid-fade differs by a few levels.
		ok(dr <= 5, "and a component part-way through its fade-in cross-fades the same (to rounding)");
	}

	ShaderComponents::releaseThreadResources(); // the shader renderer and this engine
	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
