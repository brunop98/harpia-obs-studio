#pragma once

#include "ClipLibrary.hpp"

#include <QObject>

#include <atomic>
#include <memory>

namespace harpia {

// Metadata-only discovery for a video picker. Traversal runs on the global
// worker pool; bounded batches and progress are delivered on the GUI thread.
// Calls, including destruction, belong to the object's GUI thread. Replacing
// or canceling a scan discards its queued results without waiting for disk IO.
class VideoFolderScanner : public QObject {
	Q_OBJECT
public:
	explicit VideoFolderScanner(QObject *parent = nullptr);
	~VideoFolderScanner() override;

	void start(QStringList roots, bool hideWebcamCompanions = false);
	void cancel();
	bool isRunning() const { return running_; }

signals:
	void batchReady(const QVector<ClipInfo> &clips);
	void progress(qint64 visitedEntries);
	void finished(int unavailableRoots);

private:
	std::shared_ptr<std::atomic<bool>> canceled_;
	bool running_ = false;
};

} // namespace harpia

Q_DECLARE_METATYPE(harpia::ClipInfo)
Q_DECLARE_METATYPE(QVector<harpia::ClipInfo>)
