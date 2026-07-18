#pragma once

#include <QImage>
#include <QString>

namespace harpia {

// Produces and caches preview thumbnails for recordings, keyed by file path +
// mtime. Deferred feature: the Clip Library window will call thumbnailFor() to
// populate its grid.
//
// Planned implementation: extract a representative frame using libobs' media
// source (create a "ffmpeg_source" pointed at the file, render one frame) or a
// bundled ffmpeg helper, scale it, and cache the PNG under the app cache dir.
// Declared now so the Clip Library UI can be built against a stable interface.
class ThumbnailCache {
public:
	// Returns a cached/generated thumbnail, or a null QImage if not yet
	// available (the UI can show a placeholder until generation completes).
	QImage thumbnailFor(const QString &videoPath, const QSize &target);

	// TODO(follow-up): async generation with a signal on completion, disk cache,
	// and eviction. Kept minimal here so the interface is fixed.
};

} // namespace harpia
