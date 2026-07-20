#include "TimelineThumbs.hpp"

#include "FrameSeeker.hpp"

namespace harpia {

TimelineThumbs::TimelineThumbs(QObject *parent) : QObject(parent) {}

TimelineThumbs::~TimelineThumbs()
{
	abort_.store(true);
	if (worker_.joinable())
		worker_.join();
}

void TimelineThumbs::start(const QString &path, int count, int maxW, int maxH)
{
	if (worker_.joinable() || count <= 0)
		return;
	thumbs_.resize(count);
	worker_ = std::thread([this, path, count, maxW, maxH]() {
		FrameSeeker seeker;
		if (!seeker.open(path) || seeker.durationMs() <= 0)
			return;
		const qint64 dur = seeker.durationMs();
		for (int i = 0; i < count; ++i) {
			if (abort_.load())
				return;
			const qint64 ms = qint64((i + 0.5) * double(dur) / count);
			const QImage img = seeker.frameAt(ms, maxW, maxH);
			if (abort_.load())
				return;
			// Hand the result to the GUI thread (QImage is cheap to copy).
			QMetaObject::invokeMethod(
				this,
				[this, i, img]() {
					if (i < thumbs_.size()) {
						thumbs_[i] = img;
						emit updated();
					}
				},
				Qt::QueuedConnection);
		}
	});
}

} // namespace harpia
