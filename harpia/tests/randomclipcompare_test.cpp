// Real files, asynchronous decoding and the actual compositor: a comparison
// must show different source moments at one shared output time, without
// changing the project or blocking the GUI on decoding/teardown.
#include "editor/RandomClipCompareDialog.hpp"
#include "editor/FrameSeeker.hpp"
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/TransformScriptComponent.hpp"
#include "editor/script/TransformScript.hpp"
#include "editor/timeline/ClipVariations.hpp"
#include "editor/timeline/TimelineCompositor.hpp"

#include <QtWidgets>
#include <QSignalSpy>
#include <QTest>

#include <cstdio>
#include <functional>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *description)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
	failures += !condition;
}
bool until(const std::function<bool()> &condition, int timeout = 6000)
{
	QElapsedTimer clock;
	clock.start();
	while (!condition() && clock.elapsed() < timeout) {
		QTest::qWait(5);
	}
	return condition();
}
bool makeVideo(const QString &path)
{
	QProcess ffmpeg;
	ffmpeg.start(QStringLiteral("ffmpeg"),
		     {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
		      QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
		      QStringLiteral("color=c=red:s=64x48:r=10:d=1"), QStringLiteral("-f"), QStringLiteral("lavfi"),
		      QStringLiteral("-i"), QStringLiteral("color=c=blue:s=64x48:r=10:d=1"),
		      QStringLiteral("-filter_complex"), QStringLiteral("[0:v][1:v]concat=n=2:v=1:a=0"),
		      QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"),
		      QStringLiteral("yuv420p"), QStringLiteral("-y"), path});
	return ffmpeg.waitForFinished(15000) && ffmpeg.exitCode() == 0;
}
QRect colouredBounds(const QImage &image)
{
	QRect result;
	for (int y = 0; y < image.height(); ++y) {
		for (int x = 0; x < image.width(); ++x) {
			const QColor c = image.pixelColor(x, y);
			if (c.red() > 80 || c.blue() > 80) {
				result |= QRect(x, y, 1, 1);
			}
		}
	}
	return result;
}
} // namespace

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication application(argc, argv);
	QTemporaryDir temporary;
	if (!temporary.isValid()) {
		return 2;
	}
	registerBuiltinComponents(ComponentRegistry::instance());
	const QString path = temporary.filePath(QStringLiteral("two-moments.mp4"));
	check(makeVideo(path), "a real video changes from red to blue at one second");
	if (failures) {
		return 1;
	}
	RandomClipCompareDialog::Input input;
	input.canvas = QSize(640, 360);
	input.fps = 30.0;
	input.clip.outStartMs = 1000;
	input.clip.srcEndMs = 1000;
	input.clip.speed = 2.0;
	input.clip.scale = 0.3;
	input.clip.clipOptions = {{0, 0, -0.25, 0}, {0, 1000, 0.25, -0.25}, {1, 0, 0, 0}};
	TlKeyframe start, end;
	start.tf.posX = 0.4;
	end.tf.posX = 0.6;
	start.tf.scale = end.tf.scale = 0.3;
	end.tMs = 500;
	input.clip.keys = {start, end};
	input.sources = {{0, QStringLiteral("Same video, different moments"), path, QSize(64, 48), false},
			 {1, QStringLiteral("Missing recording"), temporary.filePath("missing.mp4"), QSize(64, 48),
			  true}};
	const TlClip original = input.clip;
	int heartbeats = 0;
	QTimer heartbeat;
	heartbeat.setInterval(2);
	QObject::connect(&heartbeat, &QTimer::timeout, [&]() { ++heartbeats; });
	heartbeat.start();
	RandomClipCompareDialog dialog(input);
	dialog.show();
	dialog.setPlaying(false);
	dialog.seekTo(250);
	check(until([&]() { return dialog.playheadMs() == 250 && !dialog.previewImage(0).isNull(); }),
	      "all previews load asynchronously at the requested shared time");
	check(dialog.optionCount() == 3 && dialog.durationMs() == 500 && dialog.renderSize() == QSize(256, 144),
	      "the grid includes every option at low resolution and the speed-adjusted duration");
	const QRect red = colouredBounds(dialog.previewImage(0)), blue = colouredBounds(dialog.previewImage(1));
	check(!red.isEmpty() && !blue.isEmpty() && blue.x() - red.x() == 128 && blue.y() - red.y() == -36,
	      "each option keeps its independent framing over shared keyframe animation");
	if (!red.isEmpty() && !blue.isEmpty()) {
		const auto r = dialog.previewImage(0).pixelColor(red.center());
		const auto b = dialog.previewImage(1).pixelColor(blue.center());
		check(r.red() > 180 && r.blue() < 50 && b.blue() > 180 && b.red() < 50,
		      "duplicate source options decode their own source moments without sharing a seek position");
	}
	check(!dialog.optionError(2).isEmpty() && dialog.optionError(0).isEmpty() && dialog.optionError(1).isEmpty(),
	      "a missing file reports an error only in its own tile");
	check(heartbeats > 0, "GUI events continue while the comparison decodes");
	const auto cards = dialog.findChildren<QFrame *>(QStringLiteral("compareVariationCard"));
	check(cards.size() == 3 && cards[0]->y() == cards[1]->y() && cards[1]->y() == cards[2]->y(),
	      "a wide comparison window lays out the variations side by side");
	dialog.resize(500, 660);
	check(until([&]() { return cards[1]->y() > cards[0]->y(); }),
	      "the grid reflows into rows when the window is narrow");
	dialog.resize(960, 660);
	check(until([&]() { return cards[1]->y() == cards[0]->y(); }),
	      "widening the window restores the side-by-side grid");
	for (int ms = 0; ms < 450; ms += 7) {
		dialog.seekTo(ms);
	}
	dialog.seekTo(123);
	check(until([&]() { return dialog.playheadMs() == 123; }),
	      "rapid scrubbing coalesces to the latest time rather than queueing old frames");
	auto *slider = dialog.findChild<QSlider *>(QStringLiteral("compareVariationsScrub"));
	slider->setValue(10000);
	check(until([&]() { return dialog.playheadMs() == 499; }),
	      "the shared scrubber stops at the final valid frame");
	dialog.seekTo(9999999);
	check(until([&]() { return dialog.playheadMs() == 499; }), "out-of-range seeks clamp to the clip duration");
	dialog.seekTo(-200);
	check(until([&]() { return dialog.playheadMs() == 0; }), "negative seeks clamp to the start");
	dialog.setPlaying(true);
	check(until([&]() { return dialog.playheadMs() > 100; }), "one Play command advances every variation together");
	slider->setSliderDown(true);
	slider->setValue(5000);
	check(until([&]() { return dialog.playheadMs() == 249; }),
	      "dragging the shared scrubber seeks every tile together");
	QTest::qWait(150);
	check(dialog.isPlaying() && dialog.playheadMs() == 249,
	      "playback holds at the chosen time while the scrubber is held");
	slider->setSliderDown(false);
	check(until([&]() { return dialog.playheadMs() != 249; }),
	      "playback resumes from the scrubbed time on release");
	QTest::keyClick(&dialog, Qt::Key_Space);
	const qint64 paused = dialog.playheadMs();
	QTest::qWait(150);
	check(!dialog.isPlaying() && dialog.playheadMs() == paused,
	      "Space pauses every tile without a late frame advancing it");
	dialog.setPlaying(true);
	QTest::qWait(650);
	check(dialog.isPlaying() && dialog.playheadMs() >= 0 && dialog.playheadMs() < 500,
	      "all variations loop within the shared slot, even at a nonzero project time");
	dialog.setPlaying(false);
	dialog.seekTo(250);
	check(until([&]() { return dialog.playheadMs() == 250; }), "paused seeking remains usable after looping");
	const auto args = QCoreApplication::arguments();
	const int snapshot = args.indexOf(QStringLiteral("--snapshot"));
	if (snapshot >= 0 && snapshot + 1 < args.size()) {
		check(dialog.grab().save(args[snapshot + 1]), "comparison grid screenshot saves successfully");
	}
	check(input.clip == original, "comparison leaves the project clip, option selection and animation untouched");
	dialog.reject();
	check(!dialog.isPlaying(), "closing stops comparison playback");

	// Check the same look as the editor/exporter, including logical canvas
	// dimensions passed to scripts instead of the tiny render dimensions.
	RandomClipCompareDialog::Input look = input;
	look.clip.clipOptions.removeLast();
	look.clip.crop = QRect(8, 8, 40, 32);
	ComponentInstance transform;
	transform.typeId = QStringLiteral("harpia.transform");
	transform.instanceId = QStringLiteral("pose");
	transform.props = {{QStringLiteral("opacity"), 0.5}};
	look.clip.components = {transform};
	const QString scriptFolder = temporary.filePath(QStringLiteral("scripts"));
	QDir().mkpath(scriptFolder);
	const auto writeComponentScript = [&](double x) {
		QFile file(scriptFolder + QStringLiteral("/comparison-component.js"));
		return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
		       file.write(QStringLiteral("function position(){return {x:%1,y:0.5};}").arg(x).toUtf8()) > 0;
	};
	check(writeComponentScript(0.4) &&
		      TransformScriptComponents::loadFolder(scriptFolder, ComponentRegistry::instance()) == 1,
	      "a real transform-script component registers before comparison starts");
	ComponentInstance componentScript;
	componentScript.typeId = transformScriptComponentId(QStringLiteral("comparison-component"));
	componentScript.instanceId = QStringLiteral("script-pose");
	look.clip.components.append(componentScript);
	const ComponentRegistry registrySnapshot = ComponentRegistry::instance();
	TlScript script;
	script.name = QStringLiteral("comparison-geometry");
	look.clip.scripts = {script};
	look.scripts[script.name] = QStringLiteral("function scale(t,u,d,ctx){return ctx.canvasW===640 ? 0.3 : 0.8;}");
	RandomClipCompareDialog styled(look);
	check(writeComponentScript(0.8) &&
		      TransformScriptComponents::loadFolder(scriptFolder, ComponentRegistry::instance()) == 1,
	      "the editor hot reloads a different component generation while comparison renders");
	ComponentRegistry::instance().remove(QStringLiteral("harpia.transform"));
	styled.setPlaying(false);
	styled.seekTo(250);
	check(until([&]() { return styled.playheadMs() == 250 && !styled.previewImage(1).isNull(); }),
	      "effects, crop and scripts render on the comparison worker");
	registerBuiltinComponents(ComponentRegistry::instance());
	class ReferenceFrames final : public TimelineCompositor::FrameProvider {
	public:
		FrameSeeker seeker;
		QSize sourceSize(int) override { return QSize(64, 48); }
		QImage frameFor(int, qint64 ms) override { return seeker.frameAt(ms, 256, 144); }
	} frames;
	frames.seeker.open(path, false, 1);
	TransformEvaluator evaluator;
	evaluator.compile(script.name, look.scripts[script.name]);
	TlClip clip = look.clip;
	clip_variations::show(clip, 1);
	TlTrack track;
	track.clips = {clip};
	TimelineModel model;
	model.tracks = {track};
	const auto expected = TimelineCompositor::compose(model, 1250, QSize(256, 144), frames, &evaluator, 30.0,
							  look.canvas, nullptr, &registrySnapshot);
	check(styled.previewImage(1) == expected && styled.optionError(1).isEmpty(),
	      "comparison pixels match the real compositor with crop, component opacity and logical script geometry");
	const auto reloaded =
		TimelineCompositor::compose(model, 1250, QSize(256, 144), frames, &evaluator, 30.0, look.canvas);
	check(reloaded != expected && styled.previewImage(1) == expected,
	      "hot reload updates the editor while the open comparison keeps its immutable script/effect snapshot");
	styled.reject();
	RandomClipCompareDialog::Input brokenScript = look;
	brokenScript.scripts[script.name] =
		QStringLiteral("function scale(){throw new Error('Comparison script failed');}");
	RandomClipCompareDialog broken(brokenScript);
	broken.setPlaying(false);
	check(until([&]() { return broken.optionError(0).contains(QStringLiteral("Comparison script failed")); }),
	      "runtime effect/script failures are visible instead of silently comparing an incorrect look");
	broken.reject();

	RandomClipCompareDialog::Input many = input;
	while (many.clip.clipOptions.size() < 12) {
		many.clip.clipOptions.append({1, 0});
	}
	RandomClipCompareDialog large(many);
	large.setPlaying(false);
	check(large.optionCount() == 12 && large.renderSize() == QSize(160, 90),
	      "larger grids include all choices while reducing render size further");
	large.seekTo(100);
	QElapsedTimer closeClock;
	closeClock.start();
	large.reject();
	check(closeClock.elapsed() < 150, "closing during decode never waits for the rendering thread on the GUI");
	const int beatsBeforeClose = heartbeats;
	for (int i = 0; i < 4; ++i) {
		auto quick = std::make_unique<RandomClipCompareDialog>(input);
		quick->seekTo(400);
		quick.reset();
	}
	QTest::qWait(150);
	check(heartbeats > beatsBeforeClose, "rapid open/close discards late results and keeps the GUI responsive");
	RandomClipCompareRenderer worker(input, QSize(256, 144));
	QSignalSpy finished(&worker, &QThread::finished);
	worker.start();
	worker.request(300, 1);
	worker.cancel();
	check(until([&]() { return !finished.isEmpty(); }) && worker.wait(1000),
	      "cancellation wakes an idle or busy worker and finishes its decoder cleanup");
	std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
	return failures ? 1 : 0;
}
