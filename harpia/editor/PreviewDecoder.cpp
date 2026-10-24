#include "editor/PreviewDecoder.hpp"

#include "editor/FrameSeeker.hpp"

#include <QThread>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>

namespace harpia {

// The decode thread. It owns its own FrameSeekers -- deliberately not shared
// with the window's, which the GUI thread still uses for playback and
// filmstrips. A FrameSeeker is single-threaded by contract.
class PreviewDecoder::Worker : public QThread {
public:
	explicit Worker(PreviewDecoder *d) : d_(d) {}

protected:
	void run() override
	{
		for (;;) {
			int sourceId = -1;
			Request req;
			QString path;
			quint64 generation = 0;
			{
				QMutexLocker lock(&d_->mutex_);
				while (!d_->quit_ && d_->queue_.isEmpty())
					d_->wake_.wait(&d_->mutex_);
				if (d_->quit_)
					break;
				sourceId = d_->queue_.takeFirst();
				// Latest-wins: whatever the newest request for this source
				// is at the moment it comes off the queue is the one decoded.
				req = d_->pending_.take(sourceId);
				path = d_->decodePathLocked(sourceId);
				generation = d_->generations_.value(sourceId);
			}
			if (req.ms < 0 || path.isEmpty())
				continue;

			FrameSeeker *fs = seekerFor(sourceId, path);
			if (!fs)
				continue;
			FrameSeeker::FrameInterval interval;
			const QImage img = fs->frameAt(req.ms, req.w, req.h, &interval);
			if (img.isNull())
				continue;

			{
				QMutexLocker lock(&d_->mutex_);
				// The file may have been dropped while this was decoding.
				if (!d_->paths_.contains(sourceId) || d_->generations_.value(sourceId) != generation)
					continue;
				d_->store(sourceId, req, img, interval);
			}
			emit d_->frameReady(sourceId, req.ms);
		}
		seekers_.clear();
	}

private:
	FrameSeeker *seekerFor(int sourceId, const QString &path)
	{
		auto it = seekers_.find(sourceId);
		if (it != seekers_.end() && openPaths_.value(sourceId) == path)
			return it->second.get();
		auto fs = std::make_unique<FrameSeeker>();
		if (!fs->open(path))
			return nullptr;
		FrameSeeker *raw = fs.get();
		seekers_[sourceId] = std::move(fs);
		openPaths_[sourceId] = path;
		return raw;
	}

	PreviewDecoder *d_;
	std::map<int, std::unique_ptr<FrameSeeker>> seekers_;
	QHash<int, QString> openPaths_;
};

PreviewDecoder::PreviewDecoder(QObject *parent, qint64 cacheBytesTotal)
	: QObject(parent), cacheBytesTotal_(std::max<qint64>(1, cacheBytesTotal))
{
	worker_ = new Worker(this);
	worker_->start(QThread::LowPriority); // the GUI must always win
}

PreviewDecoder::~PreviewDecoder()
{
	{
		QMutexLocker lock(&mutex_);
		quit_ = true;
		queue_.clear();
		pending_.clear();
		wake_.wakeAll();
	}
	worker_->wait();
	delete worker_;
}

void PreviewDecoder::setSource(int sourceId, const QString &path)
{
	QMutexLocker lock(&mutex_);
	if (paths_.value(sourceId) == path)
		return;
	paths_[sourceId] = path;
	clearSourceCacheLocked(sourceId); // a different file: nothing cached applies
	generations_[sourceId] = ++nextGeneration_;
}

void PreviewDecoder::setProxy(int sourceId, const QString &proxyPath)
{
	QMutexLocker lock(&mutex_);
	if (proxies_.value(sourceId) == proxyPath)
		return;
	if (proxyPath.isEmpty())
		proxies_.remove(sourceId);
	else
		proxies_[sourceId] = proxyPath;
	clearSourceCacheLocked(sourceId);
	generations_[sourceId] = ++nextGeneration_;
	// Anything queued was for the old file; it would decode the right time from
	// the wrong place. The next render asks again.
	pending_.remove(sourceId);
	queue_.removeAll(sourceId);
}

// Caller holds the lock.
QString PreviewDecoder::decodePathLocked(int sourceId) const
{
	const QString p = proxies_.value(sourceId);
	return p.isEmpty() ? paths_.value(sourceId) : p;
}

QString PreviewDecoder::decodePathFor(int sourceId) const
{
	QMutexLocker lock(&mutex_);
	return decodePathLocked(sourceId);
}

void PreviewDecoder::removeSource(int sourceId)
{
	QMutexLocker lock(&mutex_);
	paths_.remove(sourceId);
	proxies_.remove(sourceId);
	clearSourceCacheLocked(sourceId);
	generations_.remove(sourceId);
	pending_.remove(sourceId);
	queue_.removeAll(sourceId);
}

void PreviewDecoder::clear()
{
	QMutexLocker lock(&mutex_);
	paths_.clear();
	proxies_.clear();
	cache_.clear();
	cachedBytes_ = 0;
	cachedFrames_ = 0;
	generations_.clear();
	pending_.clear();
	queue_.clear();
}

void PreviewDecoder::cancelPending()
{
	QMutexLocker lock(&mutex_);
	pending_.clear();
	queue_.clear();
}

// Caller holds the lock.
QImage PreviewDecoder::lookupLocked(int sourceId, qint64 ms, int w, int h, bool *exact) const
{
	if (exact)
		*exact = false;
	const auto it = cache_.constFind(sourceId);
	if (it == cache_.constEnd())
		return {};
	const QVector<Cached> &v = it.value();
	const Cached *nearest = nullptr;
	for (const Cached &c : v) {
		if (c.w != w || c.h != h)
			continue; // a different preview size is a different picture
		if (c.contains(ms)) {
			if (exact)
				*exact = true;
			const_cast<Cached &>(c).useSeq = ++useSeq_;
			return c.img;
		}
		if (!nearest || std::llabs(c.ms - ms) < std::llabs(nearest->ms - ms))
			nearest = &c;
	}
	return nearest ? nearest->img : QImage();
}

bool PreviewDecoder::has(int sourceId, qint64 ms, int w, int h) const
{
	QMutexLocker lock(&mutex_);
	bool exact = false;
	lookupLocked(sourceId, ms, w, h, &exact);
	return exact;
}

QImage PreviewDecoder::frame(int sourceId, qint64 ms, int w, int h, bool *exact)
{
	if (w <= 0 || h <= 0 || ms < 0) {
		if (exact)
			*exact = false;
		return {};
	}
	QMutexLocker lock(&mutex_);
	bool hit = false;
	const QImage img = lookupLocked(sourceId, ms, w, h, &hit);
	if (exact)
		*exact = hit;
	if (hit)
		return img;
	if (!paths_.contains(sourceId))
		return img; // nothing to decode from; whatever was nearest is all there is

	// Schedule it. Replacing the pending request for this source is the
	// latest-wins rule; the source keeps its place in the queue so one clip
	// being dragged cannot starve the others in a composite.
	const bool queued = pending_.contains(sourceId);
	pending_[sourceId] = Request{ms, w, h};
	if (!queued)
		queue_.push_back(sourceId);
	wake_.wakeOne();
	return img;
}

// Caller holds the lock. Worker side.
void PreviewDecoder::store(int sourceId, const Request &req, const QImage &img,
			   const FrameSeeker::FrameInterval &interval)
{
	QVector<Cached> &v = cache_[sourceId];
	for (Cached &c : v) {
		if (c.w == req.w && c.h == req.h && c.contains(req.ms)) {
			cachedBytes_ += img.sizeInBytes() - c.img.sizeInBytes();
			c.img = img;
			c.ms = req.ms;
			c.useSeq = ++useSeq_;
			c.interval = interval;
			pruneCacheLocked();
			return;
		}
	}
	// Drop the least recently looked at, not the oldest decoded: during a
	// scrub back and forth the useful frames are the ones being asked for.
	// Bounded by count and by bytes, so a 4K preview cannot hold as many
	// frames as a small one.
	qint64 bytes = img.sizeInBytes();
	for (const Cached &c : v)
		bytes += c.img.sizeInBytes();
	while (!v.isEmpty() && (v.size() >= kCachePerSource || bytes > kCacheBytesPerSource)) {
		int worst = 0;
		for (int i = 1; i < v.size(); ++i)
			if (v[i].useSeq < v[worst].useSeq)
				worst = i;
		const qint64 removed = v[worst].img.sizeInBytes();
		bytes -= removed;
		cachedBytes_ -= removed;
		--cachedFrames_;
		v.removeAt(worst);
	}
	v.push_back(Cached{req.ms, req.w, req.h, ++useSeq_, img, interval});
	cachedBytes_ += img.sizeInBytes();
	++cachedFrames_;
	pruneCacheLocked();
}

void PreviewDecoder::clearSourceCacheLocked(int sourceId)
{
	auto it = cache_.find(sourceId);
	if (it == cache_.end())
		return;
	for (const Cached &frame : it.value())
		cachedBytes_ -= frame.img.sizeInBytes();
	cachedFrames_ -= it.value().size();
	cache_.erase(it);
}

void PreviewDecoder::pruneCacheLocked()
{
	// Keep the newest frame even when it alone exceeds the configured budget;
	// refusing to cache it would make has() stay false and reschedule forever.
	while (cachedBytes_ > cacheBytesTotal_ && cachedFrames_ > 1) {
		auto source = cache_.end();
		int index = -1;
		qint64 oldest = 0;
		for (auto it = cache_.begin(); it != cache_.end(); ++it)
			for (int i = 0; i < it.value().size(); ++i)
				if (index < 0 || it.value()[i].useSeq < oldest) {
					source = it;
					index = i;
					oldest = it.value()[i].useSeq;
				}
		if (source == cache_.end())
			break;
		cachedBytes_ -= source.value()[index].img.sizeInBytes();
		--cachedFrames_;
		source.value().removeAt(index);
	}
}

} // namespace harpia
