// Render failures must identify the failed stage instead of returning one
// generic composition error. Synthetic components exercise the real renderer
// and exporter without exhausting memory or requiring damaged media files.
#include "editor/ClipExporter.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/timeline/TimelineCompositor.hpp"

#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>

#include <climits>
#include <cstdio>

using namespace harpia;

static int failures = 0;
static int afterFailureCalls = 0;

static void ok(bool condition, const char *what)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		++failures;
}

class EmptyFrame final : public IComponent {
public:
	void evaluate(const EvalContext &, ClipState &io) const override
	{
		if (io.frame)
			*io.frame = QImage();
	}
};

class AfterFailure final : public IComponent {
public:
	void evaluate(const EvalContext &, ClipState &) const override { ++afterFailureCalls; }
};

class SolidFrame final : public IComponent {
public:
	void evaluate(const EvalContext &, ClipState &io) const override
	{
		if (io.frame)
			io.frame->fill(QColor(20, 80, 140));
	}
};

static ComponentInstance component(const QString &type, const QString &instance)
{
	ComponentInstance c;
	c.typeId = type;
	c.instanceId = instance;
	return c;
}

static TimelineModel effectTimeline(const QVector<ComponentInstance> &components)
{
	TlClip clip;
	clip.type = TlClip::Type::Effect;
	clip.srcEndMs = 1000;
	clip.components = components;
	TlTrack track;
	track.kind = TlTrack::Kind::Effect;
	track.name = QStringLiteral("Diagnostic effects");
	track.clips.append(clip);
	TimelineModel model;
	model.tracks.append(track);
	return model;
}

class TestFrames final : public TimelineCompositor::FrameProvider {
public:
	QImage frameFor(int, qint64) override
	{
		QImage frame(64, 36, QImage::Format_RGBA8888);
		frame.fill(QColor(60, 70, 80));
		return frame;
	}
};

static QImage render(const TimelineModel &model, QSize canvas, TestFrames &provider,
		     TimelineCompositor::RenderError *error, qint64 atMs = 500)
{
	return TimelineCompositor::compose(model, atMs, canvas, provider, nullptr, 10.0, QSize(), error);
}

struct ExportResult {
	int finishedCount = 0;
	bool ok = false;
	bool canceled = false;
	QString error;
};

static ExportResult exportTimeline(const TimelineModel &model, const QString &path)
{
	ClipExporter exporter;
	ExportResult result;
	QObject::connect(&exporter, &ClipExporter::finished,
			 [&](bool good, bool canceled, const QString &error) {
				++result.finishedCount;
				result.ok = good;
				result.canceled = canceled;
				result.error = error;
			 });
	ClipExporter::Options options;
	options.timeline = model;
	options.canvasW = 64;
	options.canvasH = 36;
	options.timelineFps = 10.0;
	options.keepAudio = false;
	options.effort = ClipExporter::Options::Effort::Fast;
	exporter.run(QString(), path, options);
	return result;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);
	QTemporaryDir scratch;
	if (!scratch.isValid())
		return 2;

	ComponentType empty;
	empty.id = QStringLiteral("test.emptyFrame");
	empty.displayName = QStringLiteral("Empty test frame");
	empty.stage = Stage::Pixel;
	empty.pointOp = true;
	empty.make = []() { return std::make_unique<EmptyFrame>(); };
	ComponentType after;
	after.id = QStringLiteral("test.afterFailure");
	after.stage = Stage::Pixel;
	after.pointOp = true;
	after.make = []() { return std::make_unique<AfterFailure>(); };
	ComponentType afterComposite = after;
	afterComposite.id = QStringLiteral("test.afterFailureComposite");
	afterComposite.stage = Stage::Composite;
	ComponentType emptyComposite = empty;
	emptyComposite.id = QStringLiteral("test.emptyCompositeFrame");
	emptyComposite.stage = Stage::Composite;
	ComponentType solid;
	solid.id = QStringLiteral("test.solidFrame");
	solid.stage = Stage::Pixel;
	solid.pointOp = true;
	solid.make = []() { return std::make_unique<SolidFrame>(); };
	ComponentRegistry &registry = ComponentRegistry::instance();
	ok(registry.add(empty) && registry.add(after) && registry.add(afterComposite) &&
		   registry.add(emptyComposite) && registry.add(solid),
	   "deterministic test effects register through the normal component API");
	const ComponentInstance broken = component(empty.id, QStringLiteral("broken-effect"));
	const ComponentInstance following = component(after.id, QStringLiteral("after-effect"));
	const TimelineModel faulty = effectTimeline(
		{broken, following, component(afterComposite.id, QStringLiteral("after-composite"))});
	const TimelineModel healthy = effectTimeline({component(solid.id, QStringLiteral("solid"))});
	const QSize canvas(64, 36);
	TestFrames provider;
	TimelineCompositor::RenderError error;

	std::printf("\n-- canvas failures carry a reason --\n");
	ok(render(healthy, QSize(0, 36), provider, &error).isNull(), "an invalid canvas fails");
	ok(error.code == QStringLiteral("TL-CANVAS-SIZE") && !error.message.isEmpty(),
	   "invalid dimensions have their own diagnostic code and reason");
	ok(error.track == -1 && error.clip == -1, "a canvas failure is not attributed to an unrelated clip");
	ok(render(healthy, QSize(INT_MAX, INT_MAX), provider, &error).isNull(),
	   "an image larger than Qt can represent fails without exhausting RAM");
	ok(error.code == QStringLiteral("TL-CANVAS-ALLOC") && !error.message.isEmpty(),
	   "an unallocatable canvas reports an allocation failure");

	std::printf("\n-- effect failures identify the exact component --\n");
	afterFailureCalls = 0;
	ok(render(faulty, canvas, provider, &error).isNull(), "an effect returning no frame stops composition");
	ok(error.code == QStringLiteral("TL-EFFECT-FRAME"), "the failure identifies effect processing");
	ok(error.track == 0 && error.clip == 0, "the failing track and clip are preserved");
	ok(error.message.contains(empty.id) && error.message.contains(broken.instanceId),
	   "the reason includes the component type and instance");
	ok(afterFailureCalls == 0, "later Pixel and Composite effects are not evaluated on the empty frame");
	const TimelineModel compositeFailure = effectTimeline(
		{component(emptyComposite.id, QStringLiteral("broken-composite"))});
	ok(render(compositeFailure, canvas, provider, &error).isNull() &&
		   error.message.contains(emptyComposite.id),
	   "an empty frame returned in the Composite stage is diagnosed too");

	TimelineModel bounded = faulty;
	bounded.tracks[0].clips[0].scale = 0.5;
	ok(render(bounded, canvas, provider, &error).isNull() &&
		   error.code == QStringLiteral("TL-EFFECT-FRAME"),
	   "a bounded effect reports the same component failure");
	TimelineModel ramp = faulty;
	ramp.tracks[0].clips[0].components[0].inMs = 1000;
	ok(render(ramp, canvas, provider, &error).isNull() &&
		   error.message.contains(broken.instanceId),
	   "a failing effect during its fade reports the cause before paint-back");

	TimelineModel onClip = faulty;
	onClip.tracks[0].kind = TlTrack::Kind::Video;
	onClip.tracks[0].clips[0].type = TlClip::Type::Video;
	onClip.tracks[0].clips[0].sourceId = 1;
	ok(render(onClip, canvas, provider, &error).isNull() &&
		   error.code == QStringLiteral("TL-EFFECT-FRAME") && error.track == 0 && error.clip == 0,
	   "a component failing on an individual video clip is also diagnosed");

	std::printf("\n-- diagnostics do not poison subsequent frames --\n");
	const QImage good = render(healthy, canvas, provider, &error);
	ok(!good.isNull() && good.pixelColor(32, 18) == QColor(20, 80, 140),
	   "a healthy effect still renders its expected pixels");
	ok(error.code.isEmpty() && error.message.isEmpty() && error.track == -1 && error.clip == -1,
	   "a successful frame clears stale failure details");
	TimelineModel disabled = faulty;
	disabled.tracks[0].clips[0].components[0].enabled = false;
	ok(!render(disabled, canvas, provider, &error).isNull() && error.code.isEmpty(),
	   "a disabled failing effect does not fail the frame");

	std::printf("\n-- the exporter exposes the diagnostic to its caller --\n");
	const ExportResult failed = exportTimeline(faulty, scratch.filePath(QStringLiteral("failure.mp4")));
	if (!failed.error.isEmpty())
		std::printf("     %s\n", qPrintable(failed.error));
	ok(failed.finishedCount == 1 && !failed.ok && !failed.canceled,
	   "a render failure emits one failed export result");
	ok(failed.error.contains(QStringLiteral("TL-EFFECT-FRAME")) && failed.error.contains(empty.id),
	   "the export error retains the stable code and component reason");
	ok(failed.error.contains(QStringLiteral("frame"), Qt::CaseInsensitive) &&
		   failed.error.contains(QStringLiteral("64")) && failed.error.contains(QStringLiteral("36")) &&
		   failed.error.contains(QStringLiteral("track"), Qt::CaseInsensitive) &&
		   failed.error.contains(QStringLiteral("clip"), Qt::CaseInsensitive),
	   "the export error includes frame, resolution, track and clip context");
	const QString goodPath = scratch.filePath(QStringLiteral("healthy.mp4"));
	const ExportResult succeeded = exportTimeline(healthy, goodPath);
	ok(succeeded.finishedCount == 1 && succeeded.ok && !succeeded.canceled && succeeded.error.isEmpty() &&
		   QFileInfo(goodPath).size() > 0,
	   "a healthy source-free timeline still exports after a failed render");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
