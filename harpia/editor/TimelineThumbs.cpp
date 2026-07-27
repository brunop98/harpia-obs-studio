#include "TimelineThumbs.hpp"

#include "FrameSeeker.hpp"

namespace harpia {

namespace {
// Fast enough to read as "filling in", slow enough that the repaints are free.
constexpr int kEmitIntervalMs = 120;
} // namespace

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
		if (!seeker.open(path, true) || seeker.durationMs() <= 0) // fast: filmstrip only
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
			const bool last = (i == count - 1);
			QMetaObject::invokeMethod(
				this,
				[this, i, img, last]() {
					if (i >= thumbs_.size())
						return;
					thumbs_[i] = img;
					// Every listener copies the whole strip and repaints, so
					// signalling all 60 arrivals separately cost far more on the
					// GUI thread than the decode did. Coalesce to a readable
					// refresh rate; the final one always goes through.
					if (!last && lastEmit_.isValid() &&
					    lastEmit_.elapsed() < kEmitIntervalMs)
						return;
					lastEmit_.restart();
					emit updated();
				},
				Qt::QueuedConnection);
		}
	});
}

} // namespace harpia
