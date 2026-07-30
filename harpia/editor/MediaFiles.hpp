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

// Audio-only files: no picture to composite, so these land on an audio lane
// rather than a video one. A container that can hold video too (mp4, mkv, webm)
// is deliberately NOT here -- isVideoFile claims those, and dropping a video
// should give you the video. Only formats that are audio and nothing else.
inline bool isAudioFile(const QString &path)
{
	static const QStringList kExts = {QStringLiteral("mp3"),  QStringLiteral("wav"),
					  QStringLiteral("m4a"),  QStringLiteral("aac"),
					  QStringLiteral("flac"), QStringLiteral("ogg"),
					  QStringLiteral("oga"),  QStringLiteral("opus"),
					  QStringLiteral("wma"),  QStringLiteral("aiff"),
					  QStringLiteral("aif"),  QStringLiteral("mka")};
	return kExts.contains(QFileInfo(path).suffix().toLower());
}

// The one list the file dialogs offer, built from the one list the drop path
// accepts. Two hand-written lists had already drifted apart -- one offered *.wma
// and the other did not -- which is how a format becomes openable one way and
// not the other for no reason anybody can find.
inline QString audioOpenFilter()
{
	static const QStringList kExts = {QStringLiteral("mp3"),  QStringLiteral("wav"),
					  QStringLiteral("m4a"),  QStringLiteral("aac"),
					  QStringLiteral("flac"), QStringLiteral("ogg"),
					  QStringLiteral("oga"),  QStringLiteral("opus"),
					  QStringLiteral("wma"),  QStringLiteral("aiff"),
					  QStringLiteral("aif"),  QStringLiteral("mka")};
	QStringList globs;
	for (const QString &e : kExts)
		globs << QStringLiteral("*.") + e;
	return QStringLiteral("Audio files (%1);;All files (*)").arg(globs.join(QChar(' ')));
}

// Anything that can become a clip.
inline bool isMediaFile(const QString &path)
{
	return isVideoFile(path) || isImageFile(path) || isAudioFile(path);
}

} // namespace harpia
