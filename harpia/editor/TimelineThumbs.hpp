#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <thread>

namespace harpia {

// Decodes a sparse strip of small thumbnails across a clip (one per equal time
// slice) on a worker thread with its own FrameSeeker, delivering results to
// the GUI thread incrementally — timelines repaint as frames arrive and the
// editor never blocks. One strip per editor session; the destructor aborts and
// joins the worker.
class TimelineThumbs : public QObject {
	Q_OBJECT
public:
	explicit TimelineThumbs(QObject *parent = nullptr);
	~TimelineThumbs() override;

	void start(const QString &path, int count, int maxW, int maxH);

	// GUI-thread only. Entries are null until decoded; index i covers the time
	// slice [i, i+1) * duration/count.
	const QVector<QImage> &thumbs() const { return thumbs_; }

signals:
	void updated(); // one more thumbnail became available

private:
	QVector<QImage> thumbs_;
	QElapsedTimer lastEmit_; // GUI thread only: coalesces the updated() signal
	std::thread worker_;
	std::atomic<bool> abort_{false};
};

} // namespace harpia
