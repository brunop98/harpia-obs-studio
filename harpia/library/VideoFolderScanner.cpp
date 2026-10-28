#include "VideoFolderScanner.hpp"

#include "../editor/MediaFiles.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointer>
#include <QRunnable>
#include <QSemaphore>
#include <QSet>
#include <QThreadPool>

#include <utility>

namespace harpia {
namespace {

using CancelToken = std::shared_ptr<std::atomic<bool>>;

struct BatchPermit {
	explicit BatchPermit(std::shared_ptr<QSemaphore> semaphore) : semaphore_(std::move(semaphore)) {}
	~BatchPermit() { semaphore_->release(); }
	std::shared_ptr<QSemaphore> semaphore_;
};

QString pathKey(const QString &canonicalPath)
{
	QString key = QDir::fromNativeSeparators(canonicalPath);
#ifdef Q_OS_WIN
	key = key.toCaseFolded();
#endif
	return key;
}

// The worker only transports the weak pointer. Inspecting it and invoking the
// receiver happen inside the application-thread callback, never during IO.
template<typename Callback>
void postResult(QPointer<VideoFolderScanner> guard, const CancelToken &canceled, Callback callback)
{
	if (canceled->load(std::memory_order_relaxed))
		return;
	QMetaObject::invokeMethod(qApp, [guard, canceled, callback = std::move(callback)]() {
		if (canceled->load(std::memory_order_relaxed) || !guard)
			return;
		callback(guard.data());
	}, Qt::QueuedConnection);
}

} // namespace

VideoFolderScanner::VideoFolderScanner(QObject *parent) : QObject(parent)
{
	qRegisterMetaType<QVector<ClipInfo>>("QVector<ClipInfo>");
	qRegisterMetaType<QVector<ClipInfo>>("QVector<harpia::ClipInfo>");
}

VideoFolderScanner::~VideoFolderScanner()
{
	cancel();
}

void VideoFolderScanner::cancel()
{
	if (canceled_)
		canceled_->store(true, std::memory_order_relaxed);
	running_ = false;
}

void VideoFolderScanner::start(QStringList roots, bool hideWebcamCompanions)
{
	cancel();
	canceled_ = std::make_shared<std::atomic<bool>>(false);
	running_ = true;
	const auto canceled = canceled_;
	const QPointer<VideoFolderScanner> guard(this);
	QThreadPool::globalInstance()->start(QRunnable::create(
		[guard, canceled, roots = std::move(roots), hideWebcamCompanions]() {
			QStringList canonicalRoots;
			QSet<QString> rootKeys;
			int unavailableRoots = 0;
			for (const QString &root : roots) {
				if (canceled->load(std::memory_order_relaxed))
					return;
				// An empty setting must never turn into a scan of the process's
				// current working directory.
				if (root.trimmed().isEmpty())
					continue;
				const QFileInfo info(root);
				const QString canonical = info.canonicalFilePath();
				if (!info.isDir() || !info.isReadable() || canonical.isEmpty()) {
					++unavailableRoots;
					continue;
				}
				const QString key = pathKey(canonical);
				if (rootKeys.contains(key))
					continue;
				rootKeys.insert(key);
				canonicalRoots.push_back(canonical);
			}

			// A selected ancestor already visits its children. Drop redundant
			// nested roots before traversal, regardless of their input order.
			QStringList scanRoots;
			for (const QString &root : canonicalRoots) {
				if (canceled->load(std::memory_order_relaxed))
					return;
				const QString key = pathKey(root);
				bool nested = false;
				for (const QString &other : canonicalRoots) {
					QString parent = pathKey(other);
					if (parent == key)
						continue;
					if (!parent.endsWith(QLatin1Char('/')))
						parent += QLatin1Char('/');
					if (key.startsWith(parent)) {
						nested = true;
						break;
					}
				}
				if (!nested)
					scanRoots.push_back(root);
			}

			QSet<QString> files;
			QVector<ClipInfo> batch;
			batch.reserve(64);
			const auto pendingBatches = std::make_shared<QSemaphore>(4);
			qint64 visited = 0;
			QElapsedTimer sinceProgress;
			sinceProgress.start();
			auto flushBatch = [&]() {
				if (batch.isEmpty())
					return true;
				// Backpressure keeps a fast disk from filling the GUI queue with
				// the entire library. Only the worker waits, and cancellation is
				// checked at least every 25 ms while the GUI is busy.
				for (;;) {
					if (canceled->load(std::memory_order_relaxed))
						return false;
					if (pendingBatches->tryAcquire(1, 25))
						break;
				}
				const auto permit = std::make_shared<BatchPermit>(pendingBatches);
				postResult(guard, canceled, [clips = std::move(batch), permit](VideoFolderScanner *scanner) {
					// The queued callback owns the permit even if postResult's
					// guard skips delivery or Qt discards the event on shutdown.
					(void)permit;
					emit scanner->batchReady(clips);
				});
				batch = {};
				batch.reserve(64);
				return true;
			};
			for (const QString &root : scanRoots) {
				if (canceled->load(std::memory_order_relaxed))
					return;
				QDirIterator entries(root, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot |
							  QDir::NoSymLinks, QDirIterator::Subdirectories);
				while (entries.hasNext()) {
					if (canceled->load(std::memory_order_relaxed))
						return;
					entries.next();
					++visited;
					const QFileInfo info = entries.fileInfo();
					if (info.isFile() && !info.isSymLink() && !info.isHidden() &&
					    isVideoFile(info.fileName()) &&
					    (!hideWebcamCompanions || !info.completeBaseName().endsWith(
						QStringLiteral("_webcam"), Qt::CaseInsensitive))) {
						const QString canonical = info.canonicalFilePath();
						const QString key = pathKey(canonical);
						if (!canonical.isEmpty() && !files.contains(key)) {
							files.insert(key);
							ClipInfo clip;
							clip.filePath = canonical;
							clip.fileName = info.fileName();
							clip.sizeBytes = info.size();
							clip.modified = info.lastModified();
							clip.isGif = info.suffix().compare(QStringLiteral("gif"),
											 Qt::CaseInsensitive) == 0;
							batch.push_back(std::move(clip));
							if (batch.size() == 64 && !flushBatch())
								return;
						}
					}
					if (sinceProgress.elapsed() >= 100) {
						if (!flushBatch())
							return;
						postResult(guard, canceled, [visited](VideoFolderScanner *scanner) {
							emit scanner->progress(visited);
						});
						sinceProgress.restart();
					}
				}
			}
			if (canceled->load(std::memory_order_relaxed))
				return;
			if (!flushBatch())
				return;
			postResult(guard, canceled, [visited](VideoFolderScanner *scanner) {
				emit scanner->progress(visited);
			});
			// The separate callback checks cancellation again if a progress
			// handler replaces this generation before it finishes.
			postResult(guard, canceled, [unavailableRoots](VideoFolderScanner *scanner) {
				scanner->running_ = false;
				emit scanner->finished(unavailableRoots);
			});
		}), 1);
}

} // namespace harpia
