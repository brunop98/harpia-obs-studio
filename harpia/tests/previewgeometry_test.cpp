// A lower preview quality changes raster size, not the geometry scripts see.
// Exercise the real compositor and QuickJS against small sources (never enlarged
// by the decoder), cropped sources, and text measured at the project's size.
#include "editor/script/TransformScript.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ComponentStack.hpp"
#include "editor/component/TransformScriptComponent.hpp"
#include "editor/timeline/TimelineCompositor.hpp"

#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool condition, const char *what)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		++failures;
}

class Frames final : public TimelineCompositor::FrameProvider {
public:
	QSize original;
	QSize decoded;
	QSize sourceSize(int) override { return original; }
	QImage frameFor(int, qint64) override
	{
		QImage image(decoded, QImage::Format_RGBA8888);
		image.fill(Qt::white);
		return image;
	}
};

static int brightest(const QImage &image)
{
	int value = 0;
	for (int y = 0; y < image.height(); ++y)
		for (int x = 0; x < image.width(); ++x)
			value = std::max(value, image.pixelColor(x, y).red());
	return value;
}

static QString geometryScript(QSize expected)
{
	return QStringLiteral("function opacity(t,u,d,ctx) { return ctx.clipW === %1 && ctx.clipH === %2 ? 1 : 0; }")
		.arg(expected.width()).arg(expected.height());
}

static TimelineModel modelFor(TlClip clip, bool component = false)
{
	if (component) {
		ComponentInstance instance;
		instance.typeId = transformScriptComponentId(QStringLiteral("geometry"));
		instance.instanceId = QStringLiteral("geometry");
		clip.components = {instance};
	} else {
		TlScript script;
		script.name = QStringLiteral("geometry");
		clip.scripts = {script};
	}
	clip.srcEndMs = 1000;
	clip.sourceId = 1;
	TlTrack track;
	track.clips = {clip};
	TimelineModel model;
	model.tracks = {track};
	return model;
}

static bool loadGeometryComponent(const QString &folder, QSize expected)
{
	QFile file(folder + QStringLiteral("/geometry.js"));
	const QByteArray source = geometryScript(expected).toUtf8();
	if (!file.open(QIODevice::WriteOnly) || file.write(source) != source.size())
		return false;
	file.close();
	return TransformScriptComponents::loadFolder(folder, ComponentRegistry::instance()) == 1;
}

int main(int argc, char **argv)
{
	QGuiApplication app(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if (!TransformEvaluator::available()) {
		std::printf("SKIP: scripting unavailable\n");
		return 0;
	}
	const QSize logical(400, 200);
	QTemporaryDir scripts;
	if (!scripts.isValid())
		return 2;
	struct Geometry {
		QSize source;
		QRect crop;
		QSize expected;
		const char *name;
	};
	const Geometry cases[] = {
		{QSize(64, 64), QRect(), QSize(64, 64), "small source retains its native script dimensions"},
		{QSize(800, 400), QRect(200, 100, 400, 200), QSize(200, 100), "large cropped source uses logical project geometry"},
		{QSize(64, 64), QRect(-10, -10, 50, 50), QSize(40, 40), "a crop crossing the source edge reports the visible part"},
		{QSize(64, 64), QRect(70, 70, 10, 10), QSize(64, 64), "a rejected crop reports the full image that is actually drawn"},
	};
	for (TlClip::Type type : {TlClip::Type::Image, TlClip::Type::Video}) {
		std::printf("\n-- %s clip script geometry --\n", type == TlClip::Type::Image ? "image" : "video");
		for (const Geometry &geometry : cases) {
			TransformEvaluator evaluator;
			QString why;
			if (!evaluator.compile(QStringLiteral("geometry"), geometryScript(geometry.expected), &why)) {
				ok(false, qPrintable(why));
				continue;
			}
			TlClip clip;
			clip.type = type;
			clip.crop = geometry.crop;
			ok(loadGeometryComponent(scripts.path(), geometry.expected), "the same script registers as a component");
			bool matches = true;
			for (bool component : {false, true}) {
				const TimelineModel model = modelFor(clip, component);
				for (int divisor : {1, 2, 4}) {
					const QSize render = logical / divisor;
					Frames frames;
					frames.original = geometry.source;
					frames.decoded = geometry.source;
					if (frames.decoded.width() > render.width() || frames.decoded.height() > render.height())
						frames.decoded = frames.decoded.scaled(render, Qt::KeepAspectRatio);
					const QImage result = TimelineCompositor::compose(model, 500, render, frames,
						&evaluator, 30.0, logical);
					const bool correct = !result.isNull() && brightest(result) > 200;
					if (!correct)
						std::printf("     mismatch at preview 1/%d (%s)\n", divisor,
							component ? "component" : "per-clip script");
					matches = matches && correct;
				}
			}
			ok(matches, geometry.name);
		}
	}

	std::printf("\n-- caption script geometry --\n");
	for (const QString &text : {QStringLiteral("A caption"), QStringLiteral("A longer caption that wraps across the picture and has several words")}) {
		TlClip clip;
		clip.type = TlClip::Type::Text;
		clip.text.text = text;
		clip.text.fontPx = 54;
		clip.text.color = Qt::white;
		const QSize expected = TimelineCompositor::textNaturalSize(clip.text, logical);
		TransformEvaluator evaluator;
		QString why;
		ok(evaluator.compile(QStringLiteral("geometry"), geometryScript(expected), &why), "caption geometry script compiles");
		ok(loadGeometryComponent(scripts.path(), expected), "the caption script registers as a component");
		Frames unused;
		bool matches = true;
		for (bool component : {false, true}) {
			const TimelineModel model = modelFor(clip, component);
			for (int divisor : {1, 2, 4}) {
				const QImage result = TimelineCompositor::compose(model, 500, logical / divisor,
					unused, &evaluator, 30.0, logical);
				matches = matches && !result.isNull() && brightest(result) > 0;
			}
		}
		ok(matches, "caption bounds and wrapping reported to scripts are independent of preview quality");
	}

	std::printf("\n-- bundled tutorial zoom retains the picture's aspect --\n");
	{
		const QString folder = QFileInfo(QStringLiteral(__FILE__)).absolutePath() +
			QStringLiteral("/../editor/script/presets");
		QFile file(folder + QStringLiteral("/tutorial-zoom.js"));
		ok(file.open(QIODevice::ReadOnly), "the bundled tutorial script can be read");
		const QString source = QString::fromUtf8(file.readAll());
		ComponentRegistry registry;
		ok(TransformScriptComponents::loadFolder(folder, registry) > 0, "bundled scripts register as components");
		ComponentInstance instance;
		instance.typeId = transformScriptComponentId(QStringLiteral("tutorial-zoom"));
		instance.instanceId = QStringLiteral("tutorial");
		instance.props = {{QStringLiteral("zoom"), 2.0}, {QStringLiteral("focusX"), 0.5},
			{QStringLiteral("focusY"), 0.0}, {QStringLiteral("inSec"), 0.0},
			{QStringLiteral("outSec"), 0.0}, {QStringLiteral("delaySec"), 0.0}};
		ComponentStack stack({instance}, registry);
		EvalContext context;
		context.tMs = context.outMs = 500;
		context.durMs = 2000;
		context.canvas = logical;
		context.srcSize = context.scriptSize = QSize(400, 100); // a letterboxed picture
		const TlTransform actual = stack.evaluatePose(context, TlTransform()).xf;
		TransformEvaluator evaluator;
		ok(evaluator.compile(QStringLiteral("tutorial"), source), "the same bundled script compiles directly");
		ClipScript script;
		script.name = QStringLiteral("tutorial");
		for (auto it = instance.props.cbegin(); it != instance.props.cend(); ++it)
			script.params.insert(it.key(), it.value().toDouble());
		ScriptContext facts;
		facts.canvasW = logical.width();
		facts.canvasH = logical.height();
		facts.clipW = 400;
		facts.clipH = 100;
		const TlTransform expected = evaluator.applyAt(script, TlTransform(), 500, 2000, facts);
		ok(std::abs(actual.posX - expected.posX) < 1e-9 && std::abs(actual.posY - expected.posY) < 1e-9 &&
			std::abs(actual.scale - expected.scale) < 1e-9,
		   "a tutorial zoom component frames a letterboxed source like the original script");
		ok(std::abs(actual.posY - 0.5) < 1e-9, "the focused picture stays centered instead of being panned off-screen");
	}
	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
