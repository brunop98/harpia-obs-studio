#pragma once

#include <QString>
#include <QVector>

#include <atomic>
#include <functional>

namespace harpia {

// Detects visual scene changes in a video using FFmpeg's scene score
// (libavfilter select='gt(scene,threshold)') and returns the timestamps (ms) of
// each detected cut. Higher threshold → fewer cuts. Blocking — run on a worker
// thread. This is how the editor's "Auto-cut on scene changes" populates the
// output timeline.
class SceneDetector {
public:
	static QVector<qint64> detect(const QString &path, double threshold,
				      std::atomic<bool> *cancel,
				      const std::function<void(int)> &progress, QString *err);
};

} // namespace harpia
