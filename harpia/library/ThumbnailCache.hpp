#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QString>

namespace harpia {

// Produces and caches preview thumbnails for recordings. Generation runs on a
// background thread pool (decoding one frame with FFmpeg) so the UI never
// blocks; results are cached both in memory and on disk (keyed by path + mtime
// + size) so repeat loads — and later sessions — are instant.
//
// Usage from a view:
//   QImage img = cache.cached(path, size);          // instant; null if not ready
//   if (img.isNull()) cache.ensure(path, size);     // kick off async generation
//   connect(&cache, &ThumbnailCache::ready, ...);   // update the item when done
class ThumbnailCache : public QObject {
	Q_OBJECT
public:
	explicit ThumbnailCache(QObject *parent = nullptr);

	// Return a ready thumbnail (from memory or disk), or a null image if it has
	// not been generated yet. Never does decoding work itself.
	QImage cached(const QString &videoPath, const QSize &target);

	// Ensure a thumbnail is generated for this clip. No-op if it already exists
	// or a job is already in flight. Emits ready(videoPath) when it completes.
	void ensure(const QString &videoPath, const QSize &target);

	// Probe a media file's duration in milliseconds (0 if unknown). Cheap — reads
	// the container header only. Safe to call on a worker thread.
	static qint64 probeDurationMs(const QString &videoPath);

signals:
	void ready(const QString &videoPath);

private:
	// Called on the GUI thread when a background job completes.
	void jobFinished(const QString &key, const QImage &img, const QString &videoPath);

	static QString keyFor(const QString &videoPath, qint64 mtimeSecs, const QSize &target);
	QString diskPath(const QString &key) const;

	// Decode a representative frame and scale it to fit `target`. Runs on a
	// worker thread; touches no GUI state. Returns a null image on failure.
	static QImage extractFrame(const QString &videoPath, const QSize &target);

	QString cacheDir_;
	QHash<QString, QImage> memory_; // key -> image
	QSet<QString> inFlight_;        // keys currently being generated
};

} // namespace harpia
