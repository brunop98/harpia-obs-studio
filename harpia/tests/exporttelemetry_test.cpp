// Real export progress: the stage and frame counters belong to the current
// output, multi-pass GIF progress moves forward, and cancellation keeps the
// batch pending until its incomplete file has been removed.
#include "editor/BatchExport.hpp"
#include "editor/ClipExporter.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdio>

using namespace harpia;

static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

struct ExportRun {
	QStringList stages;
	QVector<int> percentages;
	QVector<QPair<qint64, qint64>> frames;
	int finished = 0;
	bool success = false;
	bool canceled = false;
	qint64 bytes = 0;
	QString error;
};

static ExportRun runExport(ClipExporter &exporter, const QString &input, const QString &output,
			   const ClipExporter::Options &options)
{
	ExportRun result;
	QObject context;
	QObject::connect(&exporter, &ClipExporter::stageChanged, &context,
			 [&](const QString &stage) { result.stages.push_back(stage); }, Qt::DirectConnection);
	QObject::connect(&exporter, &ClipExporter::progress, &context,
			 [&](int percent, qint64, qint64 bytes) {
				 result.percentages.push_back(percent);
				 result.bytes = bytes;
			 }, Qt::DirectConnection);
	QObject::connect(&exporter, &ClipExporter::frameProgress, &context,
			 [&](qint64 completed, qint64 total) { result.frames.push_back({completed, total}); },
			 Qt::DirectConnection);
	QObject::connect(&exporter, &ClipExporter::finished, &context,
			 [&](bool success, bool canceled, const QString &error) {
				 ++result.finished;
				 result.success = success;
				 result.canceled = canceled;
				 result.error = error;
			 }, Qt::DirectConnection);
	exporter.run(input, output, options);
	if (!result.error.isEmpty())
		std::printf("    %s\n", qPrintable(result.error));
	return result;
}

static bool monotonic(const QVector<int> &percentages)
{
	int previous = 0;
	for (int percentage : percentages) {
		if (percentage < previous || percentage >= 100)
			return false;
		previous = percentage;
	}
	return !percentages.isEmpty();
}

static TimelineModel textTimeline(qint64 duration)
{
	TlClip title;
	title.type = TlClip::Type::Text;
	title.srcEndMs = duration;
	title.text.text = QStringLiteral("Export progress");
	title.text.fontPx = 30;
	TlTrack track;
	track.clips.push_back(title);
	TimelineModel timeline;
	timeline.tracks.push_back(track);
	return timeline;
}

enum class CancelAt { Never, ItemStart, DuringFrames };
struct BatchRun {
	int finished = 0;
	int successes = -1;
	int failures = -1;
	bool canceled = false;
	bool cleanupBeforeFinish = false;
	QVector<QString> paths;
	QStringList stages;
	bool frames = false;
	bool bytes = false;
	bool startedBeforeDetails = true;
};

static BatchRun runBatch(BatchExporter &batch, const QString &input, const ClipExporter::Options &base,
			 const QVector<BatchExporter::Item> &items, const QString &folder, CancelAt cancelAt)
{
	BatchRun result;
	QObject context;
	std::atomic<bool> canceled{false};
	QObject::connect(&batch, &BatchExporter::itemStarted, &context,
			 [&](int, int, const QString &path) { result.paths.push_back(path); });
	QObject::connect(&batch, &BatchExporter::stageChanged, &context, [&](const QString &stage) {
		result.startedBeforeDetails = result.startedBeforeDetails && !result.paths.isEmpty();
		result.stages.push_back(stage);
	});
	QObject::connect(&batch, &BatchExporter::frameProgress, &context, [&](qint64 completed, qint64) {
		result.startedBeforeDetails = result.startedBeforeDetails && !result.paths.isEmpty();
		result.frames = result.frames || completed > 0;
	});
	QObject::connect(&batch, &BatchExporter::outputBytes, &context,
			 [&](qint64 bytes) { result.bytes = result.bytes || bytes > 0; });
	QObject::connect(&batch, &BatchExporter::finished, &context, [&](int successes, int failures, bool canceled) {
		++result.finished;
		result.successes = successes;
		result.failures = failures;
		result.canceled = canceled;
		result.cleanupBeforeFinish = canceled && !result.paths.isEmpty() && !QFile::exists(result.paths.last());
	});
	// Direct cancellation deliberately races neither event dispatch nor the
	// next item. These are the exact two boundaries the UI can interrupt.
	if (cancelAt == CancelAt::ItemStart)
		QObject::connect(&batch, &BatchExporter::itemStarted, &context,
				 [&](int, int, const QString &) { batch.cancel(); }, Qt::DirectConnection);
	if (cancelAt == CancelAt::DuringFrames)
		QObject::connect(&batch, &BatchExporter::frameProgress, &context,
				 [&](qint64 completed, qint64) {
					 if (completed > 0 && !canceled.exchange(true))
						 batch.cancel();
				 }, Qt::DirectConnection);
	check(batch.start(input, base, items, folder), "batch starts");
	QElapsedTimer timer;
	timer.start();
	while (!result.finished && timer.elapsed() < 30000) {
		QCoreApplication::processEvents();
		QThread::msleep(2);
	}
	if (!result.finished)
		batch.cancel();
	check(result.finished == 1, "batch finishes exactly once");
	return result;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);
	const QString media = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
	const QString input = media + QStringLiteral("/av_red.mp4");
	if (!QFile::exists(input)) {
		std::fprintf(stderr, "Missing fixture: %s\n", qPrintable(input));
		return 2;
	}
	QTemporaryDir output;
	ClipExporter::Options options;
	options.endMs = 700;
	options.effort = ClipExporter::Options::Effort::Fast;
	options.videoCrf = 23;
	options.keepAudio = false;

	{
		ClipExporter exporter;
		const QString path = output.filePath("trim.mp4");
		const ExportRun result = runExport(exporter, input, path, options);
		check(result.finished == 1 && result.success && !result.canceled, "trim completes successfully once");
		check(result.stages.first() == QStringLiteral("Preparing export") &&
		      result.stages.contains(QStringLiteral("Rendering video")) &&
		      result.stages.last() == QStringLiteral("Finalizing file"), "trim reports preparation, render and finalization");
		check(monotonic(result.percentages), "trim progress moves forward and reserves 100 for completion");
		bool positiveFrames = false;
		for (const auto &frame : result.frames)
			positiveFrames = positiveFrames || frame.first > 0;
		check(positiveFrames, "trim reports encoded frames");
		check(result.bytes == QFileInfo(path).size() && result.bytes > 0, "last byte count describes the finished output");
	}
	{
		ClipExporter exporter;
		exporter.cancel();
		const QString path = output.filePath("pre-canceled.mp4");
		const ExportRun stopped = runExport(exporter, input, path, options);
		check(stopped.finished == 1 && stopped.canceled && !stopped.success && !QFile::exists(path),
		      "cancel before worker starts is preserved and creates no output");
		exporter.resetCancellation();
		const ExportRun resumed = runExport(exporter, input, path, options);
		check(resumed.success && !resumed.canceled, "an explicitly reset exporter can be reused");
	}
#ifdef Q_OS_UNIX
	{
		// Faststart reopens the path while writing the trailer. Moving the
		// temporary output at that boundary forces a real finalization error
		// without disk exhaustion or changes outside this test folder.
		ClipExporter exporter;
		const QString path = output.filePath("finalization-error.mp4");
		const QString movedPath = output.filePath("moved.mp4");
		bool moved = false;
		QObject::connect(&exporter, &ClipExporter::stageChanged, &exporter,
			[&](const QString &stage) {
				if (!moved && stage == QStringLiteral("Finalizing file"))
					moved = QFile::rename(path, movedPath);
			}, Qt::DirectConnection);
		const ExportRun result = runExport(exporter, input, path, options);
		check(moved, "the finalization failure fixture interrupts the trailer");
		check(result.finished == 1 && !result.success && !result.canceled &&
		      result.error.contains(QStringLiteral("finalize")),
		      "an unwritable trailer reports failure instead of a completed export");
	}
#endif
	{
		ClipExporter::Options timeline = options;
		timeline.timeline = textTimeline(800);
		timeline.canvasW = 320;
		timeline.canvasH = 180;
		timeline.timelineFps = 30;
		ClipExporter exporter;
		const ExportRun result = runExport(exporter, input, output.filePath("timeline.mp4"), timeline);
		check(result.success && monotonic(result.percentages), "timeline progress stays monotonic through its audio pass");
		check(result.frames.contains({24, 24}), "timeline reports completed and total frame counts");
		check(result.stages.contains(QStringLiteral("Mixing audio")), "timeline identifies its audio pass");
		timeline.format = ClipExporter::Format::Gif;
		timeline.gifFps = 10;
		timeline.outWidth = 160;
		timeline.outHeight = 90;
		ClipExporter gifExporter;
		const QString gifPath = output.filePath("timeline.gif");
		const ExportRun gif = runExport(gifExporter, input, gifPath, timeline);
		check(gif.success && monotonic(gif.percentages), "two-pass timeline GIF progress never resets or reaches 100 early");
		check(gif.stages.contains(QStringLiteral("Rendering video")) &&
		      gif.stages.contains(QStringLiteral("Encoding GIF")), "GIF identifies both render and encoding stages");
		check(gif.bytes == QFileInfo(gifPath).size() && gif.bytes > 0, "GIF final size describes GIF rather than temporary video");
	}

	BatchExporter::Item first;
	first.name = QStringLiteral("first");
	first.cut.endMs = 700;
	BatchExporter::Item second = first;
	second.name = QStringLiteral("second");
	{
		BatchExporter batch;
		const BatchRun result = runBatch(batch, input, options, {first, second}, output.filePath("complete"), CancelAt::Never);
		check(result.successes == 2 && result.failures == 0 && !result.canceled, "batch writes both outputs");
		check(result.paths.size() == 2 && result.startedBeforeDetails, "each output is identified before its progress details");
		check(!result.stages.isEmpty() && result.frames && result.bytes, "batch forwards stages, frame counts and bytes");
	}
	{
		BatchExporter batch;
		const BatchRun result = runBatch(batch, input, options, {first, second}, output.filePath("stop-before"), CancelAt::ItemStart);
		check(result.canceled && result.successes == 0 && result.failures == 0, "cancel at item start starts no export");
		check(result.paths.size() == 1 && result.cleanupBeforeFinish, "pre-start cancellation finishes after cleanup and skips following items");
	}
	{
		BatchExporter batch;
		const BatchRun result = runBatch(batch, input, options, {first, second}, output.filePath("stop-during"), CancelAt::DuringFrames);
		check(result.canceled && result.successes == 0 && result.failures == 0, "cancel during rendering reports cancellation");
		check(result.paths.size() == 1 && result.cleanupBeforeFinish, "partial output is removed before canceled completion");
	}
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
