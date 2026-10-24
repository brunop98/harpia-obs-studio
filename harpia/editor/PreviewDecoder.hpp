#pragma once

#include "FrameSeeker.hpp"

// Preview frames decoded off the GUI thread.
//
// FrameSeeker::frameAt is fast for the ordinary case and slow for the one that
// matters most: a seek lands on the previous keyframe and has to roll forward
// to the target. On a 4K VP9 file with a four-second GOP that measured
// 300-400 ms per position, and showFrame() called it straight from the GUI
// thread -- so every scrub froze the window for a third of a second, and
// dragging leftward re-decoded the whole GOP for each step.
//
// Nothing here makes that decode faster. It makes it not block: the GUI thread
// asks for a frame and is answered immediately, either with the frame or with
// the nearest one already decoded, and a worker thread catches up behind it.
// When it does, frameReady() tells the window to draw again -- so a scrub
// tracks the cursor with slightly stale pictures and settles on the exact frame
// a moment after you stop, which is how a preview is supposed to behave.
//
// Requests are latest-wins PER SOURCE: a fast drag never queues up positions
// nobody will look at, but a composite of several clips still gets a request in
// for each of them.

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVector>
#include <QWaitCondition>

namespace harpia {

class PreviewDecoder : public QObject {
	Q_OBJECT
public:
	static constexpr qint64 kCacheBytesTotal = qint64(512) << 20;
	explicit PreviewDecoder(QObject *parent = nullptr, qint64 cacheBytesTotal = kCacheBytesTotal);
	~PreviewDecoder() override;

	// Register the file behind a source id. Re-registering the same id with a
	// different path reopens it; the same path is a no-op.
	void setSource(int sourceId, const QString &path);
	// Decode this source from `proxyPath` instead of its original file. The
	// proxy has the same timestamps and duration, so nothing above here changes;
	// it is just a file that seeks quickly. Empty path = back to the original.
	// Cached frames from the previous file are dropped: they came out of a
	// different decode and the two do not have to agree pixel for pixel.
	void setProxy(int sourceId, const QString &proxyPath);
	// What the decoder is actually reading for this source -- the proxy if one
	// is set, otherwise the original.
	QString decodePathFor(int sourceId) const;
	void removeSource(int sourceId);
	void clear();

	// Never blocks. Returns the frame at `ms` if it has been decoded at this
	// size, otherwise the nearest one held for the source (null if there is
	// none yet) and schedules the decode. `*exact` reports which you got, so a
	// caller that needs the real frame knows to come back after frameReady().
	QImage frame(int sourceId, qint64 ms, int w, int h, bool *exact = nullptr);

	// True when `ms` is already decoded at this size -- i.e. frame() would
	// answer exactly, with no decode scheduled.
	bool has(int sourceId, qint64 ms, int w, int h) const;

	// Forget scheduled work (not the cache). For when the preview moves
	// somewhere unrelated and the queued positions are no longer interesting.
	void cancelPending();

	// Per-source limits and a shared global ceiling keep scrubbing useful
	// without multiplying memory use by the number of imported recordings.
	// The least recently used frames are discarded first. A single frame
	// larger than the budget is retained so its request can still converge.
	static constexpr int kCachePerSource = 400;
	static constexpr qint64 kCacheBytesPerSource = qint64(512) << 20;

signals:
	// A requested frame is now available. Queued to the GUI thread.
	void frameReady(int sourceId, qint64 ms);

private:
	struct Request {
		qint64 ms = -1;
		int w = 0;
		int h = 0;
	};
	struct Cached {
		qint64 ms = -1;
		int w = 0;
		int h = 0;
		qint64 useSeq = 0; // for the LRU eviction
		QImage img;
		FrameSeeker::FrameInterval interval;
		bool contains(qint64 requestedMs) const
		{
			return ms == requestedMs || interval.contains(requestedMs);
		}
	};

	class Worker;
	friend class Worker;

	QImage lookupLocked(int sourceId, qint64 ms, int w, int h, bool *exact) const;
	QString decodePathLocked(int sourceId) const;
	// Worker side; caller holds the lock.
	void store(int sourceId, const Request &req, const QImage &img,
		   const FrameSeeker::FrameInterval &interval);
	void clearSourceCacheLocked(int sourceId);
	void pruneCacheLocked();

	mutable QMutex mutex_;
	QWaitCondition wake_;
	QHash<int, QString> paths_;
	QHash<int, QString> proxies_; // sourceId -> proxy file, when one exists
	QHash<int, Request> pending_; // latest request per source
	QVector<int> queue_;          // source ids with work, in arrival order
	QHash<int, QVector<Cached>> cache_;
	QHash<int, quint64> generations_; // reject results from an invalidated source/proxy
	quint64 nextGeneration_ = 0;
	const qint64 cacheBytesTotal_;
	qint64 cachedBytes_ = 0;
	qsizetype cachedFrames_ = 0;
	mutable qint64 useSeq_ = 0;
	bool quit_ = false;

	Worker *worker_ = nullptr;
};

} // namespace harpia
