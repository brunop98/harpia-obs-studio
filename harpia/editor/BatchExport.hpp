#pragma once

// Batch export: every Multi-Cut cut to its own file.
//
// The workflow is cutting the highlights out of one long recording and
// keeping each as a clip to reuse -- so the unit of work is one cut, written
// through the very same exporter a single Multi-Cut export uses, with the
// same format, quality, size, crop and audio choice for the whole batch and
// each cut's own speed. The clips are written one after another (the encoder
// already uses every core for one file), into a folder of their own.
//
// BatchExporter drives it: one worker thread, one ClipExporter per clip,
// overall progress out, cancel stops after the clip being written. The naming
// and folder rules are free functions so they can be checked without media.

#include "ClipExporter.hpp"

#include <QDir>
#include <QMutex>
#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <thread>

namespace harpia {

namespace batch_export {

// A prefix that can be a file name on every platform: the characters Windows
// refuses are dropped, as are leading/trailing spaces and dots; empty comes
// back as "clip".
inline QString safePrefix(const QString &prefix)
{
	QString s = prefix;
	s.remove(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")));
	s = s.trimmed();
	while (s.endsWith(QLatin1Char('.')) || s.endsWith(QLatin1Char(' ')))
		s.chop(1);
	while (s.startsWith(QLatin1Char('.')) || s.startsWith(QLatin1Char(' ')))
		s.remove(0, 1);
	return s.isEmpty() ? QStringLiteral("clip") : s;
}

// "Prefix_01": the number is zero-padded to the batch's width (at least two
// digits), so a folder of twelve clips sorts 01..12 rather than 1, 10, 11, 2.
inline QString clipName(const QString &prefix, int index1, int count)
{
	int digits = 1;
	for (int n = std::max(1, count); n >= 10; n /= 10)
		++digits;
	digits = std::max(2, digits);
	return QStringLiteral("%1_%2").arg(safePrefix(prefix)).arg(index1, digits, 10, QLatin1Char('0'));
}

// A folder for the batch under `base`: `name`, or `name (2)`, `name (3)`... --
// whichever does not exist yet, so a second batch never overwrites the first.
inline QString uniqueFolder(const QString &base, const QString &name)
{
	const QDir dir(base);
	const QString clean = safePrefix(name);
	QString candidate = clean;
	for (int n = 2; dir.exists(candidate); ++n)
		candidate = QStringLiteral("%1 (%2)").arg(clean).arg(n);
	return dir.filePath(candidate);
}

// "12 clips exported", "11 of 12 clips exported (1 failed)", "Stopped after 4
// of 12 clips".
inline QString summary(int okCount, int failCount, int total, bool canceled)
{
	if (canceled)
		return QStringLiteral("Stopped after %1 of %2 clips").arg(okCount + failCount).arg(total);
	if (failCount == 0)
		return okCount == 1 ? QStringLiteral("1 clip exported")
				    : QStringLiteral("%1 clips exported").arg(okCount);
	return QStringLiteral("%1 of %2 clips exported (%3 failed)").arg(okCount).arg(total).arg(failCount);
}

} // namespace batch_export

class BatchExporter : public QObject {
	Q_OBJECT
public:
	// One file. Either a Multi-Cut cut (`cut`), or -- when `useTimeline` --
	// a whole Full-editing render of `timeline` (a text-variation version:
	// the same project with other words), with everything else in `base`
	// (canvas, frame rate, sources, voiceover) applied as for a single export.
	struct Item {
		QString name;          // file name without the extension
		ClipExporter::Cut cut; // source index, range, speed
		bool useTimeline = false;
		TimelineModel timeline;
	};
	struct Result {
		QString name;
		QString path;
		bool ok = false;
		QString error;
	};

	explicit BatchExporter(QObject *parent = nullptr);
	~BatchExporter() override;

	// Write every item into `folder` (created if needed). `base` carries the
	// shared choices -- its cut list and range are ignored, its input list is
	// the same one the joined Multi-Cut export would use. Returns false when
	// a batch is already running or there is nothing to do.
	bool start(const QString &primaryInput, const ClipExporter::Options &base, const QVector<Item> &items,
		   const QString &folder);
	// Stop after the clip being written; that clip's partial file is removed.
	void cancel();
	bool running() const { return running_.load(); }
	// Filled in as clips finish; complete once finished() has fired.
	QVector<Result> results() const;
	QString folder() const { return folder_; }

signals:
	// `index` is the clip being written (0-based), `overallPct` the whole
	// batch, `etaMs` for the rest of the batch (0 while unknown).
	void progress(int index, int count, int overallPct, qint64 etaMs);
	void itemFinished(int index, bool ok, const QString &path, const QString &error);
	void finished(int okCount, int failCount, bool canceled);

private:
	void run(QString primaryInput, ClipExporter::Options base, QVector<Item> items);
	void join();

	std::thread thread_;
	std::atomic<bool> running_{false};
	std::atomic<bool> cancel_{false};
	std::atomic<ClipExporter *> current_{nullptr}; // the clip being written, for cancel()
	QString folder_;
	mutable QMutex resultsMutex_;
	QVector<Result> results_;
};

} // namespace harpia
