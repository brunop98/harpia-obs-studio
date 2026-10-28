// Metadata-only discovery must stay off the GUI thread, stream bounded batches,
// and never deliver results after cancellation, replacement, or destruction.
#include "library/VideoFolderScanner.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QRunnable>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *description)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
	failures += !condition;
}
template<typename Predicate>
bool waitUntil(Predicate ready, int timeoutMs = 5000)
{
	QElapsedTimer elapsed;
	elapsed.start();
	while (!ready() && elapsed.elapsed() < timeoutMs) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
		QThread::msleep(1);
	}
	return ready();
}
QString addFile(const QString &path)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly) || file.write("fake video data") != 15) {
		check(false, "the test fixture can create its media files");
		return {};
	}
	file.close();
	return QFileInfo(path).canonicalFilePath();
}
struct Result {
	QVector<ClipInfo> clips;
	qint64 visited = 0;
	int unavailable = -1;
	int batches = 0;
	bool bounded = true;
	bool guiThread = true;
	bool monotonic = true;
	bool stoppedWhenFinished = false;
};
Result scan(VideoFolderScanner &scanner, const QStringList &roots, bool hideWebcam = false)
{
	Result result;
	const auto batch = QObject::connect(&scanner, &VideoFolderScanner::batchReady, &scanner,
		[&](const QVector<ClipInfo> &clips) {
			result.guiThread &= QThread::currentThread() == qApp->thread();
			result.bounded &= !clips.isEmpty() && clips.size() <= 64;
			++result.batches;
			result.clips += clips;
		});
	const auto progress = QObject::connect(&scanner, &VideoFolderScanner::progress, &scanner,
		[&](qint64 visited) {
			result.guiThread &= QThread::currentThread() == qApp->thread();
			result.monotonic &= visited >= result.visited;
			result.visited = visited;
		});
	const auto finished = QObject::connect(&scanner, &VideoFolderScanner::finished, &scanner,
		[&](int unavailable) {
			result.guiThread &= QThread::currentThread() == qApp->thread();
			result.stoppedWhenFinished = !scanner.isRunning();
			result.unavailable = unavailable;
		});
	scanner.start(roots, hideWebcam);
	check(scanner.isRunning(), "starting a scan immediately reports that it is running");
	check(waitUntil([&]() { return result.unavailable >= 0; }), "the asynchronous scan completes");
	QObject::disconnect(batch);
	QObject::disconnect(progress);
	QObject::disconnect(finished);
	return result;
}
QSet<QString> paths(const QVector<ClipInfo> &clips)
{
	QSet<QString> result;
	for (const ClipInfo &clip : clips)
		result.insert(clip.filePath);
	return result;
}
// With one worker, this marker runs after all prior scanner IO. Deliberately
// do not process GUI events here: result callbacks must remain queued so the
// cancellation tests exercise their delivery guards, not just early exits.
bool waitForQueuedResults()
{
	auto ready = std::make_shared<std::atomic<bool>>(false);
	QThreadPool::globalInstance()->start(QRunnable::create([ready]() { ready->store(true); }), -1);
	QElapsedTimer elapsed;
	elapsed.start();
	while (!ready->load() && elapsed.elapsed() < 5000)
		QThread::msleep(1);
	return ready->load();
}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	QThreadPool::globalInstance()->setMaxThreadCount(1);
	QTemporaryDir fixture;
	check(fixture.isValid(), "a temporary fixture directory is available");
	const QString root = fixture.filePath(QStringLiteral("recordings"));
	const QString nested = root + QStringLiteral("/nested/deeper");
	const QString other = fixture.filePath(QStringLiteral("other"));
	QSet<QString> expected;
	const QStringList extensions = {QStringLiteral("MP4"), QStringLiteral("MOV"), QStringLiteral("MKV"),
		QStringLiteral("WebM"), QStringLiteral("AVI"), QStringLiteral("M4V"), QStringLiteral("GIF"),
		QStringLiteral("WMV"), QStringLiteral("FLV"), QStringLiteral("TS")};
	for (const QString &extension : extensions)
		expected.insert(addFile(nested + QStringLiteral("/video.") + extension));
	for (int i = 0; i < 140; ++i)
		expected.insert(addFile(root + QStringLiteral("/take%1.mp4").arg(i)));
	const QString companion = addFile(root + QStringLiteral("/take_webcam.MP4"));
	expected.insert(companion);
	const QString otherClip = addFile(other + QStringLiteral("/elsewhere.m4v"));
	expected.insert(otherClip);
	addFile(root + QStringLiteral("/music.wav"));
	addFile(root + QStringLiteral("/music.mp3"));
	addFile(root + QStringLiteral("/still.png"));
#ifndef Q_OS_WIN
	// A dot prefix is hidden on Unix. Windows hidden attributes and native
	// symlinks need a different fixture; QFile::link creates .lnk shortcuts.
	addFile(root + QStringLiteral("/.hidden.mp4"));
	addFile(root + QStringLiteral("/.private/secret.mp4"));
	const QString alias = fixture.filePath(QStringLiteral("recordings-alias"));
	const bool aliasMade = QFile::link(root, alias);
	const bool loopMade = QFile::link(root, nested + QStringLiteral("/loop"));
	const bool linkedFileMade = QFile::link(otherClip, root + QStringLiteral("/linked.mp4"));
	check(aliasMade && loopMade && linkedFileMade, "the fixture includes an explicit alias and nested symlinks");
#else
	const QString alias;
	const bool aliasMade = false;
	std::printf("SKIP Unix hidden-file and symlink fixtures on Windows.\n");
#endif
	QStringList roots = {nested, root, root + QStringLiteral("/."), other, QString(),
		fixture.filePath(QStringLiteral("missing")), otherClip};
	if (aliasMade)
		roots << alias;
	VideoFolderScanner scanner;
	int heartbeats = 0;
	QTimer heartbeat;
	heartbeat.setInterval(0);
	QObject::connect(&heartbeat, &QTimer::timeout, &app, [&]() { ++heartbeats; });
	heartbeat.start();
	const Result all = scan(scanner, roots);
	heartbeat.stop();
	check(paths(all.clips) == expected && all.clips.size() == expected.size(),
	      "nested and overlapping roots, duplicate roots and aliases return each supported video exactly once");
	check(all.unavailable == 2, "missing roots and file paths are counted without aborting valid roots");
	check(all.bounded && all.batches >= 3, "large scans stream nonempty batches of at most 64 clips");
	check(all.guiThread && all.monotonic && all.visited >= all.clips.size() && all.stoppedWhenFinished,
	      "ordered progress and completion arrive on the GUI thread with the correct running state");
	check(heartbeats > 0, "the GUI event loop continues running while folders are scanned");
	bool metadata = true;
	for (const ClipInfo &clip : all.clips) {
		const QFileInfo info(clip.filePath);
		metadata &= clip.filePath == info.canonicalFilePath() && clip.fileName == info.fileName() &&
			clip.sizeBytes == 15 && clip.modified.isValid() && !clip.isAudio && clip.durationMs == 0 &&
			clip.isGif == (info.suffix().compare(QStringLiteral("gif"), Qt::CaseInsensitive) == 0);
	}
	check(metadata, "invalid video bytes still yield only canonical filesystem metadata, without media probing");
	QSet<QString> withoutCompanions = expected;
	withoutCompanions.remove(companion);
	check(paths(scan(scanner, {root, other}, true).clips) == withoutCompanions,
	      "webcam companions are hidden only when the caller requests it");
	const QString hiddenRoot = fixture.filePath(QStringLiteral(".configured"));
	const QString hiddenRootClip = addFile(hiddenRoot + QStringLiteral("/visible.mp4"));
	check(paths(scan(scanner, {hiddenRoot}).clips) == QSet<QString>{hiddenRootClip},
	      "an explicitly configured hidden root remains usable while nested hidden directories are skipped");
	const Result empty = scan(scanner, {QString(), fixture.filePath(QStringLiteral("absent"))});
	check(empty.clips.isEmpty() && empty.unavailable == 1 && empty.visited == 0,
	      "empty and unavailable roots finish safely without scanning the current working directory");

	int canceledSignals = 0;
	const auto canceledBatch = QObject::connect(&scanner, &VideoFolderScanner::batchReady, &app,
		[&](const QVector<ClipInfo> &) { ++canceledSignals; });
	const auto canceledProgress = QObject::connect(&scanner, &VideoFolderScanner::progress, &app,
		[&](qint64) { ++canceledSignals; });
	const auto canceledFinished = QObject::connect(&scanner, &VideoFolderScanner::finished, &app,
		[&](int) { ++canceledSignals; });
	scanner.start({root});
	check(waitForQueuedResults(), "the canceled scan has queued results ready to deliver");
	scanner.cancel();
	QCoreApplication::processEvents();
	check(!scanner.isRunning() && canceledSignals == 0, "cancel discards already queued batches, progress and completion");
	const QString largeRoot = fixture.filePath(QStringLiteral("large"));
	for (int i = 0; i < 600; ++i)
		addFile(largeRoot + QStringLiteral("/clip%1.mp4").arg(i));
	scanner.start({largeRoot});
	const auto afterLargeScan = std::make_shared<std::atomic<bool>>(false);
	QThreadPool::globalInstance()->start(QRunnable::create([afterLargeScan]() {
		afterLargeScan->store(true);
	}), -1);
	// Keep the GUI stalled long enough for the worker to fill four batches.
	// The lower-priority marker cannot run until traversal exits: an unbounded
	// producer would finish this small local tree even with no GUI dispatch.
	QThread::msleep(150);
	check(!afterLargeScan->load(), "a stalled GUI applies backpressure instead of queuing the whole library");
	QElapsedTimer cancelElapsed;
	cancelElapsed.start();
	scanner.cancel();
	while (!afterLargeScan->load() && cancelElapsed.elapsed() < 2000)
		QThread::msleep(1);
	check(afterLargeScan->load(),
	      "cancel releases a worker waiting for GUI batch capacity without needing GUI events");
	QCoreApplication::processEvents();
	check(canceledSignals == 0, "canceling a backpressured scan also drops all pending GUI callbacks");
	QObject::disconnect(canceledBatch);
	QObject::disconnect(canceledProgress);
	QObject::disconnect(canceledFinished);
	scanner.start({root});
	check(waitForQueuedResults(), "the replaced scan has queued results ready to deliver");
	const Result replacement = scan(scanner, {other});
	check(paths(replacement.clips) == QSet<QString>{otherClip} && replacement.batches == 1 && replacement.unavailable == 0,
	      "starting another scan discards every callback from the previous generation");
	const Result drained = scan(scanner, {largeRoot});
	check(drained.clips.size() == 600 && drained.bounded && drained.batches >= 10,
	      "GUI delivery releases batch capacity so a large scan can finish normally");

	int destroyedSignals = 0;
	auto *doomed = new VideoFolderScanner;
	QObject::connect(doomed, &VideoFolderScanner::batchReady, &app,
		[&](const QVector<ClipInfo> &) { ++destroyedSignals; });
	QObject::connect(doomed, &VideoFolderScanner::finished, &app, [&](int) { ++destroyedSignals; });
	doomed->start({root});
	check(waitForQueuedResults(), "the destroyed scanner has pending queued callbacks");
	QPointer<VideoFolderScanner> guard(doomed);
	delete doomed;
	QCoreApplication::processEvents();
	check(!guard && destroyedSignals == 0, "destroying a scanner discards pending callbacks without accessing its receiver");

	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
