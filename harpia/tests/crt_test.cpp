// CRT's actual OpenGL output: neutral pixels, brightness, virtual scan/mask
// geometry, parameter persistence, and the paths used by preview and export.
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentJson.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ComponentStack.hpp"
#include "editor/component/ShaderComponent.hpp"
#include "editor/shader/GpuFx.hpp"
#include "editor/shader/GpuFxGl.hpp"
#include "editor/shader/ShaderRenderer.hpp"
#include "editor/timeline/TimelineCompositor.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

using namespace harpia;
namespace {
using Params = QMap<QString, double>;
int failures = 0;
void check(bool value, const char *description)
{
	std::printf("%s %s\n", value ? "PASS" : "FAIL", description);
	failures += !value;
}
QImage field(QColor color, QSize size = QSize(192, 1080))
{
	QImage image(size, QImage::Format_RGBA8888);
	image.fill(color);
	return image;
}
QImage pattern(QSize size, bool translucent = false)
{
	QImage image(size, QImage::Format_RGBA8888);
	for (int y = 0; y < size.height(); ++y)
		for (int x = 0; x < size.width(); ++x)
			image.setPixelColor(x, y, QColor((x * 17 + y * 3) % 256, (x * 5 + y * 11) % 256,
				(x * 23 + y * 7) % 256, translucent ? 64 + (x + y) % 192 : 255));
	return image;
}
int maxDiff(const QImage &a, const QImage &b)
{
	if (a.isNull() || b.isNull() || a.size() != b.size())
		return 999;
	int result = 0;
	for (int y = 0; y < a.height(); ++y)
		for (int x = 0; x < a.width(); ++x) {
			const QColor left = a.pixelColor(x, y), right = b.pixelColor(x, y);
			for (int difference : {std::abs(left.red() - right.red()), std::abs(left.green() - right.green()),
				std::abs(left.blue() - right.blue()), std::abs(left.alpha() - right.alpha())})
				result = std::max(result, difference);
		}
	return result;
}
std::array<double, 3> mean(const QImage &image)
{
	std::array<double, 3> result{};
	if (image.isNull())
		return {-999, -999, -999};
	for (int y = 0; y < image.height(); ++y)
		for (int x = 0; x < image.width(); ++x) {
			const QColor color = image.pixelColor(x, y);
			result[0] += color.red(); result[1] += color.green(); result[2] += color.blue();
		}
	for (double &channel : result)
		channel /= double(image.width()) * image.height();
	return result;
}
double difference(const QImage &a, const QImage &b)
{
	if (a.isNull() || b.isNull() || a.size() != b.size())
		return 999;
	double sum = 0;
	for (int y = 0; y < a.height(); ++y)
		for (int x = 0; x < a.width(); ++x) {
			const QColor ca = a.pixelColor(x, y), cb = b.pixelColor(x, y);
			sum += std::abs(ca.red() - cb.red()) + std::abs(ca.green() - cb.green()) + std::abs(ca.blue() - cb.blue());
		}
	return sum / (double(a.width()) * a.height() * 3.0);
}
int darkRuns(const QImage &image, bool horizontal = false)
{
	if (image.isNull())
		return -1;
	int count = 0;
	bool previous = false;
	for (int i = 0; i < (horizontal ? image.width() : image.height()); ++i) {
		const QColor color = image.pixelColor(horizontal ? i : image.width() / 2,
						     horizontal ? image.height() / 2 : i);
		const bool dark = color.red() < 128;
		count += dark && !previous;
		previous = dark;
	}
	return count;
}
double darkFraction(const QImage &image)
{
	if (image.isNull())
		return -1;
	int count = 0;
	for (int y = 0; y < image.height(); ++y)
		count += image.pixelColor(image.width() / 2, y).red() < 128;
	return double(count) / image.height();
}
double verticalEnergy(const QImage &image)
{
	if (image.isNull())
		return -1;
	double sum = 0;
	for (int y = 1; y < image.height(); ++y)
		for (int x = 0; x < image.width(); ++x) {
			const double delta = image.pixelColor(x, y).red() - image.pixelColor(x, y - 1).red();
			sum += delta * delta;
		}
	return sum / (double(image.width()) * (image.height() - 1));
}
double farGlow(const QImage &image, const QRect &bright)
{
	if (image.isNull())
		return -1;
	const QRect near = bright.adjusted(-2, -2, 2, 2), far = bright.adjusted(-8, -8, 8, 8);
	double total = 0;
	for (int y = far.top(); y <= far.bottom(); ++y)
		for (int x = far.left(); x <= far.right(); ++x)
			if (!near.contains(x, y))
				total += image.pixelColor(x, y).red();
	return total;
}
ComponentInstance component(const Params &params)
{
	ComponentInstance instance;
	instance.typeId = QStringLiteral("harpia.shader.crt");
	instance.instanceId = QStringLiteral("crt-test");
	for (auto it = params.cbegin(); it != params.cend(); ++it)
		instance.props.insert(it.key(), it.value());
	return instance;
}
Params resolved(const ComponentType &type, const ComponentInstance &instance, qint64 time)
{
	Params params;
	const PropBag props = resolveProps(type, instance, time);
	for (auto it = props.cbegin(); it != props.cend(); ++it)
		params.insert(it.key(), it.value().toDouble());
	return params;
}
class Frames : public TimelineCompositor::FrameProvider {
public:
	QImage image;
	QImage frameFor(int, qint64) override { return image; }
	QSize sourceSize(int) override { return image.size(); }
};
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	qputenv("HARPIA_FORCE_GPU_FX", "1");
	QGuiApplication app(argc, argv);
	QFile file(QStringLiteral(HARPIA_SRC "/editor/shader/presets/crt.frag"));
	check(file.open(QIODevice::ReadOnly), "the bundled CRT shader is readable");
	const QString source = QString::fromUtf8(file.readAll());
	const QVector<ShaderParam> definitions = parseShaderParams(source);
	Params defaults;
	QMap<QString, ShaderParam> metadata;
	for (const ShaderParam &param : definitions) {
		defaults.insert(param.uniform, param.def);
		metadata.insert(param.uniform, param);
	}
	using T = ShaderParam::Type;
	struct Expected { const char *key; T type; double min, max, value; };
	const Expected expected[] = {
		{"uCurvature", T::Float, 0, .5, .08}, {"uScanline", T::Float, 0, 1, .35},
		{"uAberration", T::Float, 0, 4, 1}, {"uVignette", T::Float, 0, 1, .40},
		{"uScanDensity", T::Float, 60, 1080, 360}, {"uScanWidth", T::Float, .05, .95, .5},
		{"uScanSoftness", T::Float, 0, 1, 0}, {"uPreserveBrightness", T::Bool, 0, 1, 1},
		{"uMaskType", T::Choice, 0, 2, 1}, {"uMaskStrength", T::Float, 0, 1, .08},
		{"uMaskSize", T::Float, 1, 6, 1}, {"uGlow", T::Float, 0, 1, 0},
		{"uGlowRadius", T::Float, .5, 8, 2}, {"uBrightness", T::Float, .5, 1.5, 1},
		{"uGamma", T::Float, .5, 2.5, 1}, {"uBorderOpacity", T::Float, 0, 1, 1}};
	bool schema = definitions.size() == 16 && metadata.size() == 16;
	for (const Expected &want : expected) {
		const auto it = metadata.constFind(QLatin1String(want.key));
		schema = schema && it != metadata.cend() && it->type == want.type &&
			std::abs(it->min - want.min) < 1e-9 && std::abs(it->max - want.max) < 1e-9 &&
			std::abs(it->def - want.value) < 1e-9 && !it->label.isEmpty();
	}
	check(schema, "all sixteen old/new controls expose their documented types, ranges and defaults");
	check(metadata.value(QStringLiteral("uMaskType")).choices ==
	      QStringList({QStringLiteral("Off"), QStringLiteral("Aperture grille"), QStringLiteral("Shadow mask")}),
	      "phosphor mask choices are named for the Inspector");
	ShaderRenderer renderer;
	if (!renderer.ensureGl()) {
		std::printf("SKIP GPU rendering: OpenGL 3.3 unavailable. Run with QT_QPA_PLATFORM=xcb under xvfb-run. %s\n",
			    qPrintable(renderer.lastError()));
		return failures ? 1 : 77;
	}
	check(true, "a real OpenGL 3.3 renderer is available");
	QString error;
	const QString wrapped = wrapShaderToy(source, definitions);
	check(renderer.setChain({ShaderLayerSource{wrapped, definitions}}, &error), "the bundled CRT compiles and links on OpenGL");
	if (!error.isEmpty()) {
		std::printf("%s\n", qPrintable(error));
		return 1;
	}
	const auto render = [&](const QImage &input, const Params &params) {
		const QImage result = renderer.apply(input, .5f, 15, {params});
		check(!result.isNull(), "CRT rendering produces pixels");
		return result;
	};
	Params neutral = defaults;
	for (const char *key : {"uCurvature", "uScanline", "uAberration", "uVignette", "uMaskStrength", "uGlow"})
		neutral[QLatin1String(key)] = 0;
	neutral[QStringLiteral("uBrightness")] = neutral[QStringLiteral("uGamma")] = 1;
	const QImage alphaPattern = pattern(QSize(192, 540), true);
	check(maxDiff(alphaPattern, render(alphaPattern, neutral)) <= 1, "neutral CRT is an identity including source alpha");

	std::printf("\n-- preserved brightness is measured in each encoded RGB channel --\n");
	bool meansPreserved = true;
	for (QColor color : {QColor(24, 24, 24), QColor(128, 128, 128), QColor(240, 240, 240), QColor(20, 110, 230)})
		for (double strength : {0.0, .35, 1.0}) {
			Params params = neutral;
			params[QStringLiteral("uPreserveBrightness")] = 1;
			params[QStringLiteral("uScanline")] = strength;
			params[QStringLiteral("uScanDensity")] = 120;
			const auto average = mean(render(field(color), params));
			const std::array<int, 3> channels{color.red(), color.green(), color.blue()};
			for (int c = 0; c < 3; ++c)
				meansPreserved = meansPreserved && std::abs(average[c] - channels[c]) <= 1.25;
			std::printf("  RGB %d,%d,%d strength %.2f -> %.2f,%.2f,%.2f\n", channels[0], channels[1], channels[2],
				    strength, average[0], average[1], average[2]);
		}
	check(meansPreserved, "scanline strength preserves mean dark, midtone, highlight and colored RGB values");
	bool reducedMeans = true;
	for (int height : {135, 360, 540}) {
		Params reduced = neutral;
		reduced[QStringLiteral("uPreserveBrightness")] = 1;
		reduced[QStringLiteral("uScanline")] = 1;
		const auto average = mean(render(field(QColor(20, 110, 230), QSize(192, height)), reduced));
		reducedMeans = reducedMeans && std::abs(average[0] - 20) <= 1.25 &&
			std::abs(average[1] - 110) <= 1.25 && std::abs(average[2] - 230) <= 1.25;
	}
	check(reducedMeans, "undersampled default-density scanlines retain RGB brightness in reduced previews");
	Params params = neutral;
	params[QStringLiteral("uScanline")] = 1;
	params[QStringLiteral("uScanDensity")] = 120;
	params[QStringLiteral("uPreserveBrightness")] = 0;
	check(mean(render(field(QColor(128, 128, 128)), params))[0] < 96, "legacy scanline mode still darkens the picture");
	Params defaultField = defaults;
	defaultField[QStringLiteral("uCurvature")] = defaultField[QStringLiteral("uAberration")] = defaultField[QStringLiteral("uVignette")] = 0;
	check(std::abs(mean(render(field(QColor(128, 128, 128)), defaultField))[0] - 128) <= 1.25,
	      "default beam and mask settings do not dim a flat field when vignette is off");

	std::printf("\n-- beam geometry survives preview scaling --\n");
	params = neutral;
	params[QStringLiteral("uPreserveBrightness")] = 1;
	params[QStringLiteral("uScanline")] = .8;
	params[QStringLiteral("uScanDensity")] = 120;
	const QImage lines1080 = render(field(QColor(128, 128, 128)), params);
	const QImage lines540 = render(field(QColor(128, 128, 128), QSize(192, 540)), params);
	check(std::abs(darkRuns(lines1080) - 120) <= 2 && std::abs(darkRuns(lines540) - 120) <= 2,
	      "a 120-line setting retains its line count at 1080p and half-size preview");
	params[QStringLiteral("uScanDensity")] = 240;
	check(std::abs(darkRuns(render(field(QColor(128, 128, 128)), params)) - 240) <= 2,
	      "scanline count changes the actual number of beam lines");
	params[QStringLiteral("uScanDensity")] = 60;
	params[QStringLiteral("uScanWidth")] = .2;
	const QImage narrow = render(field(QColor(128, 128, 128)), params);
	params[QStringLiteral("uScanWidth")] = .8;
	const QImage wide = render(field(QColor(128, 128, 128)), params);
	check(darkFraction(wide) - darkFraction(narrow) > .35, "beam width changes dark-band coverage independently of count");
	params[QStringLiteral("uScanWidth")] = .5;
	params[QStringLiteral("uScanSoftness")] = 0;
	const QImage crisp = render(field(QColor(128, 128, 128)), params);
	params[QStringLiteral("uScanSoftness")] = 1;
	const QImage soft = render(field(QColor(128, 128, 128)), params);
	check(difference(crisp, soft) > 2 && verticalEnergy(soft) < verticalEnergy(crisp) * .85,
	      "softness smooths beam edges rather than merely changing their brightness");

	std::printf("\n-- phosphor masks, glow and display controls --\n");
	params = neutral;
	params[QStringLiteral("uMaskStrength")] = .8;
	params[QStringLiteral("uMaskType")] = 0;
	check(maxDiff(render(field(QColor(128, 128, 128)), params), field(QColor(128, 128, 128))) <= 1,
	      "mask Off suppresses the mask even with nonzero strength");
	params[QStringLiteral("uMaskType")] = 1;
	const QImage aperture = render(field(QColor(128, 128, 128)), params);
	params[QStringLiteral("uMaskType")] = 2;
	const QImage shadow = render(field(QColor(128, 128, 128)), params);
	check(difference(aperture, shadow) > 2 && verticalEnergy(aperture) < .1 && verticalEnergy(shadow) > 1,
	      "aperture grille is columnar while shadow mask has a distinct two-dimensional pattern");
	params[QStringLiteral("uMaskType")] = 1;
	params[QStringLiteral("uMaskStrength")] = .15;
	const QImage faintMask = render(field(QColor(128, 128, 128)), params);
	check(difference(aperture, field(QColor(128, 128, 128))) > difference(faintMask, field(QColor(128, 128, 128))) * 2,
	      "mask strength controls visible phosphor contrast");
	params[QStringLiteral("uMaskStrength")] = .8;
	params[QStringLiteral("uMaskSize")] = 3;
	const QImage largeMask = render(field(QColor(128, 128, 128)), params);
	check(darkRuns(aperture, true) > darkRuns(largeMask, true) * 2,
	      "larger mask size produces fewer and wider phosphor groups");
	params = neutral;
	const QRect bright(64, 520, 64, 32);
	QImage spot = field(Qt::black);
	{ QPainter painter(&spot); painter.fillRect(bright, Qt::white); }
	params[QStringLiteral("uGlow")] = .8;
	params[QStringLiteral("uGlowRadius")] = 1;
	const QImage smallGlow = render(spot, params);
	params[QStringLiteral("uGlowRadius")] = 6;
	const QImage largeGlow = render(spot, params);
	check(difference(smallGlow, spot) > .01 && farGlow(largeGlow, bright) > farGlow(smallGlow, bright) + 100,
	      "glow amount lights neighboring pixels and radius controls its spread");
	params = neutral;
	params[QStringLiteral("uBrightness")] = .75;
	check(mean(render(field(QColor(128, 128, 128)), params))[0] < 108, "brightness adjusts the output level");
	params = neutral;
	params[QStringLiteral("uGamma")] = 1.8;
	check(mean(render(field(QColor(128, 128, 128)), params))[0] > 148, "display gamma adjusts midtones independently of brightness");
	params = neutral;
	params[QStringLiteral("uVignette")] = .8;
	const QImage vignette = render(field(QColor(128, 128, 128)), params);
	check(vignette.pixelColor(5, 5).red() + 30 < vignette.pixelColor(96, 540).red(), "the original vignette control still darkens corners");
	const QImage opaquePattern = pattern(QSize(192, 1080));
	params = neutral;
	params[QStringLiteral("uAberration")] = 4;
	check(difference(render(opaquePattern, params), opaquePattern) > 2, "the original aberration control still separates color channels");
	params = neutral;
	params[QStringLiteral("uCurvature")] = .4;
	check(difference(render(opaquePattern, params), opaquePattern) > 2, "the original curvature control still bends the image");
	for (double opacity : {0.0, .35, 1.0}) {
		params[QStringLiteral("uBorderOpacity")] = opacity;
		const QImage curved = render(field(QColor(128, 128, 128, 77)), params);
		const QColor corner = curved.pixelColor(0, 0), center = curved.pixelColor(96, 540);
		check(std::abs(corner.alpha() - int(std::lround(opacity * 255))) <= 1 && corner.red() == 0 &&
		      std::abs(center.alpha() - 77) <= 1, "curved border opacity is controlled while the source's center alpha is retained");
	}

	std::printf("\n-- component metadata, JSON and real GPU/compositor routes --\n");
	ComponentRegistry &registry = ComponentRegistry::instance();
	registerBuiltinComponents(registry);
	QTemporaryDir folder;
	check(QFile::copy(file.fileName(), folder.filePath(QStringLiteral("crt.frag"))) &&
	      ShaderComponents::loadFolder(folder.path(), registry) == 1, "CRT registers through the real shader component loader");
	const ComponentType *type = registry.find(QStringLiteral("harpia.shader.crt"));
	check(type && type->props.size() == definitions.size(), "all CRT parameters become component properties");
	if (!type)
		return 1;
	ComponentInstance legacy = component({{"uCurvature", 0}, {"uScanline", .35}, {"uAberration", 0}, {"uVignette", 0}});
	const Params oldResolved = resolved(*type, legacy, 0);
	bool oldDefaults = true;
	for (const ShaderParam &definition : definitions)
		if (!legacy.props.contains(definition.uniform))
			oldDefaults = oldDefaults && std::abs(oldResolved.value(definition.uniform) - definition.def) < 1e-9;
	check(oldDefaults, "older four-control CRT components inherit every new declared default");
	ComponentInstance configured = component(neutral);
	configured.props[QStringLiteral("uScanDensity")] = 120.0;
	configured.props[QStringLiteral("uMaskStrength")] = .4;
	configured.props[QStringLiteral("uMaskType")] = 2.0;
	configured.props[QStringLiteral("uMaskSize")] = 2.0;
	configured.keys[QStringLiteral("uScanline")] = {{0, 0, TlEase::Linear}, {1000, 1, TlEase::Linear}};
	configured.keys[QStringLiteral("uMaskType")] = {{0, 1, TlEase::Linear}, {1000, 2, TlEase::Linear}};
	configured.keys[QStringLiteral("uPreserveBrightness")] = {{0, 1, TlEase::Linear}, {1000, 0, TlEase::Linear}};
	const ComponentInstance roundtrip = componentFromJson(componentToJson(configured, registry), registry);
	check(roundtrip == configured, "CRT settings and numeric/choice keys survive project JSON round-trip");
	const Params halfway = resolved(*type, roundtrip, 500);
	check(std::abs(halfway.value(QStringLiteral("uScanline")) - .5) < 1e-9 &&
	      halfway.value(QStringLiteral("uMaskType")) == 1 && resolved(*type, roundtrip, 1000).value(QStringLiteral("uMaskType")) == 2 &&
	      halfway.value(QStringLiteral("uPreserveBrightness")) == 1 &&
	      resolved(*type, roundtrip, 1000).value(QStringLiteral("uPreserveBrightness")) == 0,
	      "scanline keys interpolate while mask choices and brightness-preserving switches hold until their next key");
	installGpuFx();
	GpuFx *gpu = gpuFx();
	check(gpu != nullptr, "the real GPU effect engine is installed");
	if (!gpu)
		return 1;
	const QImage input = pattern(QSize(192, 540));
	const QImage direct = render(input, halfway);
	const bool gpuRan = gpu->begin(input) && gpu->shader(QStringLiteral("crt-test"), ShaderComponents::generation(),
		wrapped, definitions, halfway, .5f, 15);
	const QImage chained = gpu->end();
	check(gpuRan && maxDiff(direct, chained) <= 2, "CRT gives the same pixels in standalone and GPU effect-chain renderers");
	EvalContext context;
	context.tMs = 500; context.durMs = 2000; context.fps = 30; context.canvas = input.size();
	ComponentStack stack({roundtrip}, registry, false);
	QImage componentGpu = input;
	stack.evaluatePixels(context, componentGpu, &error);
	check(error.isEmpty() && maxDiff(direct, componentGpu) <= 2, "keyframed CRT reaches the GPU through ComponentStack");
	const GpuFxProvider installed = gpuFxProvider();
	gpuFxProvider() = nullptr;
	QImage componentStandalone = input;
	stack.evaluatePixels(context, componentStandalone, &error);
	gpuFxProvider() = installed;
	check(error.isEmpty() && maxDiff(componentGpu, componentStandalone) <= 2, "the component's standalone fallback matches its GPU chain");
	Frames provider;
	provider.image = input;
	TlClip clip;
	clip.sourceId = 1; clip.srcEndMs = 2000; clip.components = {roundtrip};
	TlTrack track; track.clips = {clip};
	TimelineModel timeline; timeline.tracks = {track};
	TimelineCompositor::RenderError renderError;
	const QImage composed = TimelineCompositor::compose(timeline, 500, input.size(), provider, nullptr, 30, QSize(), &renderError);
	check(renderError.message.isEmpty() && maxDiff(direct, composed) <= 2,
	      "preview/export's compositor preserves the same keyframed CRT pixels");
	ShaderComponents::releaseThreadResources();
	renderer.release();
	std::printf("%s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
