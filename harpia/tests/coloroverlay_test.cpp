// The Color Overlay shader, and the dropdown shaders can now declare.
//
//   * `//@param ... choice` parses into named options (underscores read as
//     spaces), and a shader component turns it into a dropdown, not a slider.
//   * Rendered through a clip's real component stack, on a picture with opaque,
//     half-transparent and fully transparent pixels:
//       - the overlay stays inside the clip's shape: every pixel keeps its
//         alpha, so the transparent part stays transparent;
//       - Normal paints the colour; Opacity 0, or a see-through swatch, leaves
//         the picture as it was;
//       - Multiply by white, Screen with black, Overlay and Soft light with
//         mid-grey change nothing -- the identities that pin each formula;
//       - Color takes the swatch's hue and keeps the clip's brightness;
//       - the GPU effect chain and the CPU path give the same picture.
//
// Rendering needs OpenGL 3.3; the suite runs it under Xvfb with a software GL.
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ComponentStack.hpp"
#include "editor/component/ShaderComponent.hpp"
#include "editor/shader/GpuFx.hpp"
#include "editor/shader/GpuFxGl.hpp"
#include "editor/shader/ShaderEffect.hpp"

#include <QApplication>
#include <QFile>
#include <QTemporaryDir>

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

static const char *kSrc = HARPIA_SRC "/editor/shader/presets/coloroverlay.frag";

// Left half opaque mid-grey, right half fully transparent, one column of
// half-transparent orange between them.
static QImage picture()
{
	QImage img(64, 32, QImage::Format_RGBA8888);
	for (int y = 0; y < img.height(); ++y) {
		uchar *row = img.scanLine(y);
		for (int x = 0; x < img.width(); ++x) {
			uchar *p = row + x * 4;
			if (x < 30) {
				p[0] = p[1] = p[2] = 128;
				p[3] = 255;
			} else if (x < 34) {
				p[0] = 200;
				p[1] = 100;
				p[2] = 50;
				p[3] = 128;
			} else {
				p[0] = p[1] = p[2] = p[3] = 0;
			}
		}
	}
	return img;
}

static QColor at(const QImage &img, int x, int y)
{
	const uchar *p = img.convertToFormat(QImage::Format_RGBA8888).constScanLine(y) + x * 4;
	return QColor(p[0], p[1], p[2], p[3]);
}

static int maxDiff(const QImage &a0, const QImage &b0)
{
	const QImage a = a0.convertToFormat(QImage::Format_RGBA8888), b = b0.convertToFormat(QImage::Format_RGBA8888);
	if (a.size() != b.size())
		return 999;
	int mx = 0;
	for (int y = 0; y < a.height(); ++y) {
		const uchar *pa = a.constScanLine(y), *pb = b.constScanLine(y);
		for (int x = 0; x < a.width() * 4; ++x) {
			// A fully transparent pixel's colour is invisible and undefined.
			if (pa[(x & ~3) + 3] == 0 && pb[(x & ~3) + 3] == 0)
				continue;
			mx = std::max(mx, std::abs(pa[x] - pb[x]));
		}
	}
	return mx;
}

static double colorProp(QRgb argb)
{
	return double(quint32(argb));
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	qputenv("HARPIA_FORCE_GPU_FX", "1"); // the suite runs on a software GL; the app would not

	QFile f(QString::fromLatin1(kSrc));
	f.open(QIODevice::ReadOnly);
	const QString glsl = QString::fromUtf8(f.readAll());

	std::printf("\n-- a dropdown in a shader --\n");
	{
		const QVector<ShaderParam> ps = parseShaderParams(glsl);
		const ShaderParam *mode = nullptr;
		for (const ShaderParam &p : ps)
			if (p.uniform == QLatin1String("uMode"))
				mode = &p;
		ok(ps.size() == 3 && mode, "three controls, one of them uMode");
		if (mode) {
			std::printf("     %s: %s\n", qPrintable(mode->label), qPrintable(mode->choices.join(QStringLiteral(" / "))));
			ok(mode->type == ShaderParam::Type::Choice, "uMode is a choice");
			ok(mode->choices == QStringList({QStringLiteral("Normal"), QStringLiteral("Multiply"),
							 QStringLiteral("Screen"), QStringLiteral("Overlay"),
							 QStringLiteral("Soft light"), QStringLiteral("Color")}),
			   "with its six options, \"Soft_light\" reading \"Soft light\"");
			ok(mode->def == 0.0 && mode->min == 0.0 && mode->max == 5.0 &&
				   mode->label == QLatin1String("Blend mode"),
			   "Normal by default, 0..5, labelled Blend mode");
		}
		const QVector<ShaderParam> bad =
			parseShaderParams(QStringLiteral("//@param uX choice 9 A|B Pick\n//@param uY choice 0\n"));
		ok(bad.size() == 1 && bad[0].def == 1.0, "a default past the last option clamps; a choice with no options is dropped");
		ok(wrapShaderToy(QString(), ps).contains(QLatin1String("uniform float uMode;")),
		   "the shader gets the option's index as a float");
	}

	ComponentRegistry &reg = ComponentRegistry::instance();
	registerBuiltinComponents(reg);
	QTemporaryDir dir;
	QFile::copy(QString::fromLatin1(kSrc), dir.filePath(QStringLiteral("coloroverlay.frag")));
	ShaderComponents::loadFolder(dir.path(), reg);
	const QString id = shaderComponentId(QStringLiteral("coloroverlay"));

	std::printf("\n-- as a component --\n");
	{
		const ComponentType *t = reg.find(id);
		ok(t != nullptr, "coloroverlay.frag becomes a component");
		if (t) {
			std::printf("     \"%s\"\n", qPrintable(t->displayName));
			ok(t->displayName == QLatin1String("Color Overlay"), "named Color Overlay in the Add Component menu");
			const PropDef *mode = nullptr;
			for (const PropDef &d : t->props)
				if (d.key == QLatin1String("uMode"))
					mode = &d;
			ok(mode && mode->type == PropType::Choice && mode->choices.size() == 6,
			   "Blend mode is a dropdown of six, not a slider");
		}
	}

	installGpuFx();
	if (!ShaderComponents::available()) {
		ShaderComponents::releaseThreadResources();
		std::printf("\n  (no OpenGL 3.3 here: shaders render as a pass-through; nothing to draw)\n");
		std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
		return failures ? 1 : 0;
	}

	const QImage base = picture();
	const auto render = [&](QRgb color, int mode, double opacity) {
		ComponentInstance ci;
		ci.typeId = id;
		ci.instanceId = QStringLiteral("overlay");
		ci.props.insert(QStringLiteral("uColor"), colorProp(color));
		ci.props.insert(QStringLiteral("uMode"), double(mode));
		ci.props.insert(QStringLiteral("uOpacity"), opacity);
		EvalContext ctx;
		ctx.tMs = 0;
		ctx.durMs = 1000;
		ctx.fps = 30;
		ComponentStack stack({ci}, reg, false);
		QImage out = base;
		stack.evaluatePixels(ctx, out);
		return out;
	};
	const QRgb blue = qRgba(0x3F, 0x73, 0xB8, 255);

	std::printf("\n-- Normal --\n");
	{
		const QImage out = render(blue, 0, 1.0);
		const QColor solid = at(out, 5, 5), half = at(out, 31, 5), clear = at(out, 50, 5);
		std::printf("     opaque grey -> rgba(%d,%d,%d,%d); half-transparent -> rgba(%d,%d,%d,%d); clear alpha %d\n",
			    solid.red(), solid.green(), solid.blue(), solid.alpha(), half.red(), half.green(),
			    half.blue(), half.alpha(), clear.alpha());
		ok(std::abs(solid.red() - 0x3F) <= 2 && std::abs(solid.green() - 0x73) <= 2 &&
			   std::abs(solid.blue() - 0xB8) <= 2 && solid.alpha() == 255,
		   "the clip takes the colour");
		ok(std::abs(half.blue() - 0xB8) <= 2 && std::abs(half.alpha() - 128) <= 1,
		   "a half-transparent edge takes it too, and stays half-transparent");
		ok(clear.alpha() == 0, "and where the clip is transparent, it stays transparent");
		const QImage halfway = render(blue, 0, 0.5);
		const QColor h = at(halfway, 5, 5);
		ok(std::abs(h.blue() - (128 + 0xB8) / 2) <= 2, "at Opacity 0.5, half way between");
	}

	std::printf("\n-- what must change nothing --\n");
	{
		const int none = maxDiff(base, render(blue, 0, 0.0));
		const int seeThrough = maxDiff(base, render(qRgba(0x3F, 0x73, 0xB8, 0), 0, 1.0));
		const int mulWhite = maxDiff(base, render(qRgba(255, 255, 255, 255), 1, 1.0));
		const int scrBlack = maxDiff(base, render(qRgba(0, 0, 0, 255), 2, 1.0));
		const int ovlGrey = maxDiff(base, render(qRgba(128, 128, 128, 255), 3, 1.0));
		const int softGrey = maxDiff(base, render(qRgba(128, 128, 128, 255), 4, 1.0));
		std::printf("     opacity 0: %d; clear swatch: %d; multiply white: %d; screen black: %d; overlay grey: %d; "
			    "soft light grey: %d\n",
			    none, seeThrough, mulWhite, scrBlack, ovlGrey, softGrey);
		ok(none <= 1, "Opacity 0");
		ok(seeThrough <= 1, "a fully see-through swatch");
		ok(mulWhite <= 1, "Multiply by white");
		ok(scrBlack <= 1, "Screen with black");
		ok(ovlGrey <= 2, "Overlay with mid-grey");
		ok(softGrey <= 2, "Soft light with mid-grey");
	}

	std::printf("\n-- the blends do their job --\n");
	{
		const QColor mul = at(render(blue, 1, 1.0), 5, 5);
		ok(std::abs(mul.blue() - 128 * 0xB8 / 255) <= 2 && std::abs(mul.red() - 128 * 0x3F / 255) <= 2,
		   "Multiply: grey times the colour");
		const QColor scr = at(render(blue, 2, 1.0), 5, 5);
		ok(scr.blue() > 128 && scr.red() > 128, "Screen: lighter than the grey in every channel");
		const QColor col = at(render(blue, 5, 1.0), 5, 5);
		const double lum = 0.3 * col.redF() + 0.59 * col.greenF() + 0.11 * col.blueF();
		std::printf("     Color on mid-grey -> rgb(%d,%d,%d), luminance %.3f\n", col.red(), col.green(), col.blue(),
			    lum);
		ok(std::abs(lum - 128.0 / 255.0) < 0.02, "Color: the grey's brightness kept");
		ok(col.blue() > col.green() && col.green() > col.red(), "and the swatch's hue taken");
	}

	std::printf("\n-- the GPU chain and the CPU path agree --\n");
	if (!gpuFx()) {
		std::printf("     (no GPU effect chain here: only the CPU path to check)\n");
	} else {
		const GpuFxProvider installed = gpuFxProvider();
		for (const int mode : {0, 4, 5}) {
			gpuFxProvider() = nullptr;
			const QImage cpu = render(blue, mode, 0.8);
			gpuFxProvider() = installed;
			const QImage gpu = render(blue, mode, 0.8);
			const int d = maxDiff(cpu, gpu);
			std::printf("     mode %d: largest difference %d/255 (soft edges included)\n", mode, d);
			ok(d <= 2, mode == 0 ? "Normal at 0.8: the same picture both ways"
				   : mode == 4 ? "Soft light at 0.8: the same"
					       : "Color at 0.8: the same");
		}
	}

	ShaderComponents::releaseThreadResources(); // GL goes before the app does
	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
