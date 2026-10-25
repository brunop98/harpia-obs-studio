// Shader and component failures must be visible to preview/export callers,
// rather than producing a successful frame with the requested effect missing.
#include "editor/ClipExporter.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ComponentStack.hpp"
#include "editor/component/ShaderComponent.hpp"
#include "editor/shader/ShaderRenderer.hpp"
#include "editor/shader/EditorGlSurface.hpp"
#include "editor/shader/GpuFxGl.hpp"
#include "editor/shader/GpuFx.hpp"
#include "editor/timeline/TimelineCompositor.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
int laterCalls = 0;
int compilerMessages = 0;
QtMessageHandler previousMessages = nullptr;
void observeCompilerMessages(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
	if (message.contains(QLatin1String("QOpenGLShader::compile")) || message.contains(QLatin1String("QOpenGLShader::link")))
		++compilerMessages;
	if (previousMessages)
		previousMessages(type, context, message);
	else
		std::fprintf(stderr, "%s\n", qUtf8Printable(message));
}
void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}
class DeclaredFailure final : public IComponent {
public:
	void evaluate(const EvalContext &, ClipState &state) const override
	{
		// A component can fail while still holding its original input image.
		state.renderError = QStringLiteral("The shader framebuffer could not be rendered.");
	}
};
class LaterComponent final : public IComponent {
public:
	void evaluate(const EvalContext &, ClipState &) const override { ++laterCalls; }
};
class GpuComponent final : public IComponent {
public:
	static int cpuCalls;
	void evaluate(const EvalContext &, ClipState &state) const override
	{
		++cpuCalls;
		if (state.frame)
			state.frame->fill(Qt::cyan);
	}
	bool hasGpu() const override { return true; }
	bool evaluateGpu(const EvalContext &, GpuFx &) const override { return true; }
};
int GpuComponent::cpuCalls = 0;
class FakeGpu final : public GpuFx {
public:
	bool failBegin = false;
	bool emptyReadback = false;
	bool failMix = false;
	bool on = false;
	QImage image;
	bool begin(const QImage &frame) override
	{
		if (failBegin)
			return false;
		image = frame;
		on = true;
		return true;
	}
	QImage end() override { on = false; return emptyReadback ? QImage() : image; }
	bool active() const override { return on; }
	QSize size() const override { return image.size(); }
	bool fx(int, const QMap<QString, double> &) override { return false; }
	bool blur(int) override { return false; }
	bool maskAlpha(const QImage &, int, bool) override { return false; }
	bool shader(const QString &, int, const QString &, const QVector<ShaderParam> &,
		const QMap<QString, double> &, float, int) override { return false; }
	void saveInput() override {}
	bool mixWithSaved(double) override { return !failMix; }
};
FakeGpu *testGpu = nullptr;
GpuFx *fakeGpuProvider() { return testGpu; }
ComponentInstance instance(const QString &type, const QString &id)
{
	ComponentInstance result;
	result.typeId = type;
	result.instanceId = id;
	return result;
}
QImage inputFrame()
{
	QImage image(64, 36, QImage::Format_RGBA8888);
	image.fill(QColor(35, 70, 105));
	return image;
}
class Frames final : public TimelineCompositor::FrameProvider {
public:
	QImage frameFor(int, qint64) override { return inputFrame(); }
	QSize sourceSize(int) override { return QSize(64, 36); }
};
TimelineModel effectTimeline(const QVector<ComponentInstance> &components)
{
	TlClip clip;
	clip.type = TlClip::Type::Effect;
	clip.srcEndMs = 1000;
	clip.components = components;
	TlTrack track;
	track.kind = TlTrack::Kind::Effect;
	track.name = QStringLiteral("Shader diagnostics");
	track.clips = {clip};
	TimelineModel result;
	result.tracks = {track};
	return result;
}
struct ExportResult {
	int calls = 0;
	bool ok = false;
	bool canceled = false;
	QString error;
};
ExportResult exportTimeline(const TimelineModel &timeline, const QString &path,
	ClipExporter::Format format = ClipExporter::Format::Mp4)
{
	ClipExporter exporter;
	ExportResult result;
	QObject::connect(&exporter, &ClipExporter::finished,
		[&](bool ok, bool canceled, const QString &why) {
			++result.calls;
			result.ok = ok;
			result.canceled = canceled;
			result.error = why;
		});
	ClipExporter::Options options;
	options.timeline = timeline;
	options.format = format;
	options.canvasW = 64;
	options.canvasH = 36;
	options.timelineFps = 10;
	options.keepAudio = false;
	options.effort = ClipExporter::Options::Effort::Fast;
	exporter.run(QString(), path, options);
	return result;
}
bool write(const QString &path, const QByteArray &contents)
{
	QFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}
bool containsDiagnostic(const ExportResult &result, const QString &component)
{
	return result.calls == 1 && !result.ok && !result.canceled &&
		result.error.contains(QStringLiteral("TL-EFFECT-FRAME")) && result.error.contains(component) &&
		result.error.contains(QStringLiteral("frame"), Qt::CaseInsensitive) &&
		result.error.contains(QStringLiteral("track"), Qt::CaseInsensitive);
}
} // namespace

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);
	prepareEditorGlSurfaces();
	const bool requireGl = app.arguments().contains(QStringLiteral("--require-gl"));
	QTemporaryDir scratch;
	QTemporaryDir anotherFolder;
	if (!scratch.isValid() || !anotherFolder.isValid())
		return 2;
	ComponentRegistry &registry = ComponentRegistry::instance();
	ComponentType broken;
	broken.id = QStringLiteral("test.declaredRenderFailure");
	broken.displayName = QStringLiteral("Declared shader failure");
	broken.stage = Stage::Pixel;
	broken.make = [] { return std::make_unique<DeclaredFailure>(); };
	ComponentType later;
	later.id = QStringLiteral("test.laterPixel");
	later.stage = Stage::Pixel;
	later.make = [] { return std::make_unique<LaterComponent>(); };
	ComponentType laterComposite = later;
	laterComposite.id = QStringLiteral("test.laterComposite");
	laterComposite.stage = Stage::Composite;
	ComponentType compositeFailure = broken;
	compositeFailure.id = QStringLiteral("test.declaredCompositeFailure");
	compositeFailure.stage = Stage::Composite;
	check(registry.add(broken) && registry.add(later) && registry.add(laterComposite) && registry.add(compositeFailure),
		"diagnostic components register using the production API");
	const ComponentInstance brokenInstance = instance(broken.id, QStringLiteral("broken-instance"));
	const ComponentInstance missing = instance(QStringLiteral("harpia.shader.not_installed"), QStringLiteral("missing-instance"));
	EvalContext context;
	context.canvas = QSize(64, 36);
	context.tMs = 500;
	context.durMs = 1000;
	QString error;
	QImage frame = inputFrame();
	laterCalls = 0;
	ComponentStack({brokenInstance, instance(later.id, QStringLiteral("later")),
		instance(laterComposite.id, QStringLiteral("later-composite"))}, registry)
		.evaluatePixels(context, frame, &error);
	check(frame.isNull() && error.contains(QStringLiteral("framebuffer")) && error.contains(broken.id) &&
		error.contains(broken.displayName) && error.contains(brokenInstance.instanceId),
		"a declared failure includes the reason, display name, type and instance even if the component kept its input");
	check(laterCalls == 0, "a failed component prevents all later Pixel and Composite processing");
	frame = inputFrame();
	ComponentStack({instance(compositeFailure.id, QStringLiteral("composite-failure"))}, registry)
		.evaluatePixels(context, frame, &error);
	check(frame.isNull() && error.contains(compositeFailure.id), "declared failures propagate from Composite as well as Pixel");

	ComponentType gpuType;
	gpuType.id = QStringLiteral("test.gpuOutputFailure");
	gpuType.displayName = QStringLiteral("GPU output test");
	gpuType.stage = Stage::Pixel;
	gpuType.make = [] { return std::make_unique<GpuComponent>(); };
	check(registry.add(gpuType), "a deterministic GPU component can exercise readback and fade failures");
	const auto previousProvider = gpuFxProvider();
	FakeGpu fake;
	testGpu = &fake;
	gpuFxProvider() = &fakeGpuProvider;
	ComponentInstance gpuInstance = instance(gpuType.id, QStringLiteral("gpu-instance"));
	frame = QImage(128, 128, QImage::Format_RGBA8888);
	frame.fill(Qt::red);
	fake.emptyReadback = true;
	ComponentStack({gpuInstance}, registry).evaluatePixels(context, frame, &error);
	check(frame.isNull() && !fake.active() && error.contains(gpuType.id) && error.contains(QStringLiteral("GPU")),
		"failed GPU readback ends the session and fails the frame instead of keeping its original pixels");
	fake.emptyReadback = false;
	fake.failMix = true;
	gpuInstance.inMs = 1000;
	frame = QImage(128, 128, QImage::Format_RGBA8888);
	frame.fill(Qt::red);
	ComponentStack({gpuInstance}, registry).evaluatePixels(context, frame, &error);
	check(frame.isNull() && !fake.active() && error.contains(gpuType.id) && error.contains(QStringLiteral("fade")),
		"failed GPU fade blending cannot be reported as a successfully applied effect");
	fake.failMix = false;
	fake.failBegin = true;
	gpuInstance.inMs = 0;
	frame = QImage(128, 128, QImage::Format_RGBA8888);
	frame.fill(Qt::red);
	GpuComponent::cpuCalls = 0;
	ComponentStack({gpuInstance}, registry).evaluatePixels(context, frame, &error);
	check(!frame.isNull() && error.isEmpty() && frame.pixelColor(64, 64) == QColor(Qt::cyan) && GpuComponent::cpuCalls == 1,
		"an unavailable GPU session still permits a successful CPU fallback");
	gpuFxProvider() = previousProvider;
	testGpu = nullptr;

	frame = inputFrame();
	ComponentStack({missing}, registry, false).evaluatePixels(context, frame, &error);
	check(frame.isNull() && error.contains(missing.typeId) && error.contains(missing.instanceId),
		"an enabled missing shader fails even when optional conflict scanning is disabled");
	ComponentInstance ignored = missing;
	ignored.enabled = false;
	frame = inputFrame();
	ComponentStack({ignored}, registry, false).evaluatePixels(context, frame, &error);
	check(!frame.isNull() && error.isEmpty(), "disabled missing shaders do not block a frame and clear stale diagnostics");
	ignored.enabled = true;
	ignored.inMs = 1000;
	EvalContext before = context;
	before.tMs = 0;
	frame = inputFrame();
	ComponentStack({ignored}, registry, false).evaluatePixels(before, frame, &error);
	check(!frame.isNull() && error.isEmpty(), "a zero-weight missing shader is not evaluated before its envelope begins");
	frame = inputFrame();
	ComponentStack({ignored}, registry, false).evaluatePixels(context, frame, &error);
	check(frame.isNull() && !error.isEmpty(), "the same missing shader is diagnosed when its envelope starts contributing");

	Frames provider;
	TimelineCompositor::RenderError renderError;
	const TimelineModel faulty = effectTimeline({brokenInstance});
	frame = TimelineCompositor::compose(faulty, 500, context.canvas, provider, nullptr, 10, {}, &renderError);
	check(frame.isNull() && renderError.code == QStringLiteral("TL-EFFECT-FRAME") &&
		renderError.track == 0 && renderError.clip == 0 && renderError.message.contains(brokenInstance.instanceId),
		"whole-canvas effect composition retains the declared component failure and exact clip context");
	TimelineModel onClip = faulty;
	onClip.tracks[0].kind = TlTrack::Kind::Video;
	onClip.tracks[0].clips[0].type = TlClip::Type::Video;
	onClip.tracks[0].clips[0].sourceId = 1;
	frame = TimelineCompositor::compose(onClip, 500, context.canvas, provider, nullptr, 10, {}, &renderError);
	check(frame.isNull() && renderError.code == QStringLiteral("TL-EFFECT-FRAME") && renderError.message.contains(broken.id),
		"an effect on an individual source clip uses the same diagnostic path");
	TlClip outgoing = onClip.tracks[0].clips[0];
	outgoing.components.clear();
	TlClip incoming = onClip.tracks[0].clips[0];
	incoming.outStartMs = 500;
	incoming.transition.enabled = true;
	onClip.tracks[0].clips = {outgoing, incoming};
	frame = TimelineCompositor::compose(onClip, 750, context.canvas, provider, nullptr, 10, {}, &renderError);
	check(frame.isNull() && renderError.clip == 1 && renderError.message.contains(broken.id),
		"an incoming transition clip cannot silently lose its failing effect");
	TimelineModel healthy = onClip;
	healthy.tracks[0].clips = {outgoing};
	frame = TimelineCompositor::compose(healthy, 500, context.canvas, provider, nullptr, 10, {}, &renderError);
	check(!frame.isNull() && renderError.code.isEmpty() && renderError.message.isEmpty(),
		"a later healthy render clears the earlier failure instead of retaining stale warnings");

	check(containsDiagnostic(exportTimeline(faulty, scratch.filePath(QStringLiteral("declared.mp4"))), broken.id),
		"MP4 export fails once with frame, track and actionable component details");
	check(containsDiagnostic(exportTimeline(faulty, scratch.filePath(QStringLiteral("declared.gif")), ClipExporter::Format::Gif), broken.id),
		"timeline GIF export preserves the failure from its intermediate timeline render");
	check(containsDiagnostic(exportTimeline(effectTimeline({missing}), scratch.filePath(QStringLiteral("missing.mp4"))), missing.typeId),
		"an uninstalled shader cannot export successfully as a silently unchanged video");

	const QByteArray valid = "void mainImage(out vec4 color, in vec2 position) { color = vec4(0.2, 0.4, 0.6, 1.0); }\n";
	const QString removedPath = scratch.filePath(QStringLiteral("deleted.frag"));
	const QString keptPath = anotherFolder.filePath(QStringLiteral("retained.frag"));
	check(write(removedPath, valid) && write(keptPath, valid), "independent shader folders can be prepared");
	ShaderComponents::loadFolder(scratch.path(), registry);
	ShaderComponents::loadFolder(anotherFolder.path(), registry);
	const QString removedId = shaderComponentId(QStringLiteral("deleted"));
	const QString keptId = shaderComponentId(QStringLiteral("retained"));
	check(registry.find(removedId) && registry.find(keptId), "shaders from separate folders coexist");
	check(QFile::remove(removedPath), "a loaded shader file can be removed for the reload regression");
	ShaderComponents::loadFolder(scratch.path(), registry);
	check(registry.find(removedId) && registry.find(keptId),
		"reloading preserves component metadata and another folder's registered shader types");
	frame = inputFrame();
	ComponentStack({instance(removedId, QStringLiteral("deleted-instance"))}, registry, false)
		.evaluatePixels(context, frame, &error);
	check(frame.isNull() && error.contains(removedId), "a deleted shader does not keep rendering an obsolete cached source");

	TimelineModel caption = effectTimeline({missing});
	caption.tracks[0].kind = TlTrack::Kind::Video;
	caption.tracks[0].clips[0].type = TlClip::Type::Text;
	caption.tracks[0].clips[0].text.text = QStringLiteral("Caption");
	frame = TimelineCompositor::compose(caption, 500, context.canvas, provider, nullptr, 10, {}, &renderError);
	check(frame.isNull() && renderError.code == QStringLiteral("TL-EFFECT-TARGET") &&
		renderError.message.contains(missing.typeId) && renderError.message.contains(QStringLiteral("effect clip")),
		"a shader attached to a text clip reports its unsupported target and a useful repair");
	caption.tracks[0].clips[0].components[0].enabled = false;
	frame = TimelineCompositor::compose(caption, 500, context.canvas, provider, nullptr, 10, {}, &renderError);
	check(!frame.isNull() && renderError.code.isEmpty(), "a disabled text shader leaves its caption usable");
	caption.tracks[0].clips[0].components[0].enabled = true;
	caption.tracks[0].clips[0].components[0].inMs = 1000;
	frame = TimelineCompositor::compose(caption, 0, context.canvas, provider, nullptr, 10, {}, &renderError);
	check(!frame.isNull() && renderError.code.isEmpty(), "a text shader with zero envelope weight is not spuriously diagnosed");

	ShaderRenderer empty;
	error = QStringLiteral("stale error");
	check(empty.setChain({}, &error) && error.isEmpty() && empty.apply(inputFrame(), 0, 0, {}) == inputFrame(),
		"an intentionally empty shader chain remains a valid pass-through without needing GL");

	const bool glAvailable = ShaderComponents::available();
	const QString badPath = scratch.filePath(QStringLiteral("broken.frag"));
	const QString badId = shaderComponentId(QStringLiteral("broken"));
	const QByteArray invalid = "void mainImage(out vec4 color, in vec2 position) { color = undeclared_broken_shader; }\n";
	check(write(badPath, invalid), "a syntactically invalid shader fixture can be written");
	ShaderComponents::loadFolder(scratch.path(), registry);
	const ComponentInstance activeShader = instance(glAvailable ? badId : keptId, QStringLiteral("active-shader"));
	frame = inputFrame();
	ComponentStack({activeShader}, registry).evaluatePixels(context, frame, &error);
	check(frame.isNull() && !error.isEmpty() && error.contains(activeShader.typeId), glAvailable
		? "a real GLSL compile error stops the effect instead of passing its input through"
		: "an unavailable GL context produces an actionable shader failure instead of a pass-through");
	if (glAvailable) {
		const QString firstCompileFailure = error;
		compilerMessages = 0;
		previousMessages = qInstallMessageHandler(&observeCompilerMessages);
		bool retained = true;
		for (int i = 0; i < 5; ++i) {
			QImage goodFrame = inputFrame();
			QString goodError;
			ComponentStack({instance(keptId, QStringLiteral("interleaved-valid"))}, registry)
				.evaluatePixels(context, goodFrame, &goodError);
			QImage badFrame = inputFrame();
			QString badError;
			ComponentStack({activeShader}, registry).evaluatePixels(context, badFrame, &badError);
			retained = retained && !goodFrame.isNull() && goodError.isEmpty() && badFrame.isNull() &&
				badError == firstCompileFailure;
		}
		qInstallMessageHandler(previousMessages);
		previousMessages = nullptr;
		check(retained, "a broken shader retains its own compile diagnostic when valid shaders render between failures");
		check(compilerMessages == 0, "an unchanged broken shader does not recompile and flood driver logs on every frame");
	}
	check(containsDiagnostic(exportTimeline(effectTimeline({activeShader}), scratch.filePath(QStringLiteral("shader-failure.mp4"))), activeShader.typeId),
		"actual shader compilation/context failures reach the exporter too");
	if (glAvailable) {
		check(write(badPath, valid), "a broken shader can be repaired in place");
		ShaderComponents::loadFolder(scratch.path(), registry);
		frame = inputFrame();
		ComponentStack({instance(badId, QStringLiteral("repaired"))}, registry).evaluatePixels(context, frame, &error);
		const QColor color = frame.isNull() ? QColor() : frame.pixelColor(32, 18);
		check(!frame.isNull() && error.isEmpty() && std::abs(color.red() - 51) <= 1 &&
			std::abs(color.green() - 102) <= 1 && std::abs(color.blue() - 153) <= 1,
			"reloading a repaired shader invalidates the old compile failure and renders its actual pixels");
		ShaderRenderer renderer;
		QString why;
		check(renderer.setChain({{QStringLiteral("#version 330 core\nout vec4 color; void main(){ color=vec4(1.0); }"), {}}}, &why),
			"a real standalone shader chain can be compiled");
		check(renderer.apply(QImage(), 0, 0, {}).isNull() && !renderer.lastError().isEmpty(),
			"a failed render with an active shader chain has a reason rather than silently returning its input");
		renderer.release();
		qputenv("HARPIA_FORCE_GPU_FX", "1");
		installGpuFx();
		GpuFx *engine = gpuFx();
		const bool began = engine && engine->begin(inputFrame());
		check(began, "the real GPU effect engine can begin a diagnostic session");
		if (began) {
			// Inject a deterministic OpenGL error without allocating huge images
			// or damaging the context. end() must reject stale/partial pixels.
			QOpenGLContext::currentContext()->functions()->glEnable(0xffffffffu);
			check(engine->end().isNull() && !engine->active(),
				"a real GPU driver error fails readback instead of returning an unchanged successful image");
			check(engine->begin(inputFrame()) && !engine->end().isNull(),
				"the GPU engine can render a healthy frame after a diagnosed failure");
		}
	} else {
		std::printf("  SKIP real GL compile/repair checks: no usable OpenGL context on this platform\n");
	}
	ShaderComponents::releaseThreadResources();
	releaseEditorGlSurfaces();
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : requireGl && !glAvailable ? 77 : 0;
}
