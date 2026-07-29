#pragma once

// What counts as a droppable media file, in one place.
//
// The timeline decides whether to accept a drag, and the window decides what to
// build from it. Both have to agree on the answer: a widget that accepts a file
// the window then refuses is a drop that visibly does nothing, which is worse
// than not accepting it at all. One list, read by both.
//
// GIF is in BOTH lists on purpose. It is a video the decoder can read frame by
// frame and also an image QImage will load the first frame of; the video path is
// the one that does what people mean by dropping a GIF, so isVideoFile is
// checked first everywhere it matters.

#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace harpia {

inline bool isVideoFile(const QString &path)
{
	static const QStringList kExts = {QStringLiteral("mp4"), QStringLiteral("mov"),
					  QStringLiteral("mkv"), QStringLiteral("webm"),
					  QStringLiteral("avi"), QStringLiteral("m4v"),
					  QStringLiteral("gif"), QStringLiteral("wmv"),
					  QStringLiteral("flv"), QStringLiteral("ts")};
	return kExts.contains(QFileInfo(path).suffix().toLower());
}

inline bool isImageFile(const QString &path)
{
	static const QStringList kExts = {QStringLiteral("png"),  QStringLiteral("jpg"),
					  QStringLiteral("jpeg"), QStringLiteral("bmp"),
					  QStringLiteral("gif"),  QStringLiteral("webp"),
					  QStringLiteral("tif"),  QStringLiteral("tiff")};
	return kExts.contains(QFileInfo(path).suffix().toLower());
}

// Anything that can become a clip.
inline bool isMediaFile(const QString &path)
{
	return isVideoFile(path) || isImageFile(path);
}

} // namespace harpia
