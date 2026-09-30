#include "BatchExport.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMutexLocker>

#include <algorithm>

namespace harpia {


BatchExporter::BatchExporter(QObject *parent) : QObject(parent) {}

BatchExporter::~BatchExporter()
{
	cancel();
	join();
}

void BatchExporter::join()
{
	if (thread_.joinable())
		thread_.join();
}

QVector<BatchExporter::Result> BatchExporter::results() const
{
	QMutexLocker lock(&resultsMutex_);
	return results_;
}

bool BatchExporter::start(const QString &primaryInput, const ClipExporter::Options &base,
			  const QVector<Item> &items, const QString &folder)
{
	if (running_.load() || items.isEmpty())
		return false;
	join(); // a previous, finished batch's thread
	if (!QDir().mkpath(folder))
		return false;
	folder_ = folder;
	cancel_.store(false);
	{
		QMutexLocker lock(&resultsMutex_);
		results_.clear();
	}
	running_.store(true);
	thread_ = std::thread([this, primaryInput, base, items]() { run(primaryInput, base, items); });
	return true;
}

void BatchExporter::cancel()
{
	cancel_.store(true);
	if (ClipExporter *ex = current_.load())
		ex->cancel();
}

void BatchExporter::run(QString primaryInput, ClipExporter::Options base, QVector<Item> items)
{
	const int count = items.size();
	const QString ext = ClipExporter::extensionFor(base.format);
	int okCount = 0, failCount = 0;
	bool canceled = false;
	QElapsedTimer clock;
	clock.start();

	for (int i = 0; i < count && !canceled; ++i) {
		if (cancel_.load()) {
			canceled = true;
			break;
		}
		const Item &item = items[i];
		const QString path = folder_ + QLatin1Char('/') + item.name + QLatin1Char('.') + ext;

		// The same exporter, the same options, one cut: this IS a Multi-Cut
		// export of a single cut, so crop, size, quality, audio and the
		// cut's speed all land exactly as they do in the joined video.
		ClipExporter::Options o = base;
		if (item.useTimeline) {
			// A version of the whole project: only the timeline differs from
			// the single export `base` describes, voiceover and all.
			o.timeline = item.timeline;
		} else {
			o.cuts.clear();
			o.cuts.push_back(item.cut);
			o.startMs = 0;
			o.endMs = 0;
			o.speed = 1.0;
			o.timeline = TimelineModel();
			o.voiceovers.clear();
		}

		bool ok = false, itemCanceled = false;
		QString err;
		ClipExporter ex;
		// Same thread: the lambdas run inside ex.run(), before it returns.
		connect(&ex, &ClipExporter::finished, &ex,
			[&](bool g, bool c, const QString &e) {
				ok = g && !c;
				itemCanceled = c;
				err = e;
			},
			Qt::DirectConnection);
		connect(&ex, &ClipExporter::progress, &ex,
			[&](int pct, qint64, qint64) {
				const double done = (i + std::clamp(pct, 0, 100) / 100.0) / count;
				const qint64 elapsed = clock.elapsed();
				const qint64 eta = done > 0.02 ? qint64(elapsed / done - elapsed) : 0;
				emit progress(i, count, int(done * 100.0), eta);
			},
			Qt::DirectConnection);
		current_.store(&ex);
		emit progress(i, count, int(100.0 * i / count), 0);
		if (!cancel_.load())
			ex.run(primaryInput, path, o);
		else
			itemCanceled = true;
		current_.store(nullptr);

		if (!ok) // never leave a partial or empty file behind
			QFile::remove(path);
		if (itemCanceled || cancel_.load()) {
			canceled = true;
			// Not counted either way: it was neither written nor refused.
			break;
		}
		Result r;
		r.name = item.name;
		r.path = path;
		r.ok = ok;
		r.error = ok ? QString() : (err.isEmpty() ? QStringLiteral("Could not export the clip.") : err);
		{
			QMutexLocker lock(&resultsMutex_);
			results_.push_back(r);
		}
		if (ok)
			++okCount;
		else
			++failCount;
		emit itemFinished(i, ok, path, r.error);
	}
	// The per-clip exporter stops its own count at 95 before the flush, so
	// the batch says 100 itself once everything is written.
	if (!canceled)
		emit progress(count - 1, count, 100, 0);
	running_.store(false);
	emit finished(okCount, failCount, canceled);
}

} // namespace harpia
