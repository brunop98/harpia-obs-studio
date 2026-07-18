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
	//
	// Stub for now: always returns a null image so callers fall back to a
	// generic file icon. The signature is fixed so the real implementation
	// (decode one frame via libobs media source or a bundled ffmpeg, scale,
	// cache to disk) drops in without touching callers.
	QImage thumbnailFor(const QString &videoPath, const QSize &target)
	{
		Q_UNUSED(videoPath);
		Q_UNUSED(target);
		return QImage();
	}

	// TODO(follow-up): async generation with a signal on completion, disk cache,
	// and eviction. Kept minimal here so the interface is fixed.
};

} // namespace harpia
