#include "StillImage.hpp"

#include "FrameSeeker.hpp"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QStringList>

namespace harpia {

// The file-dialog filter, built from what can ACTUALLY be opened rather than
// written out by hand. The hand-written one listed *.webp on every build, so on
// a Qt without the WebP plugin the dialog invited a file the editor then
// refused -- and offering something you cannot accept is worse than not
// offering it.
QString imageOpenFilter()
{
	QStringList globs;
	for (const QByteArray &f : QImageReader::supportedImageFormats())
		globs << QStringLiteral("*.") + QString::fromLatin1(f);
	// libav reads these whatever Qt's plugins do, via readStillImage's fallback.
	for (const QString &extra : {QStringLiteral("*.webp"), QStringLiteral("*.tif"),
				     QStringLiteral("*.tiff")})
		if (!globs.contains(extra))
			globs << extra;
	globs.sort();
	return QStringLiteral("Images (%1);;All files (*)").arg(globs.join(QChar(' ')));
}

// Read a still, by whichever decoder can. Returns a null image and fills *why
// with a reason a person can act on.
//
// Two decoders, because neither alone covers what the dialog offers. Qt handles
// PNG, JPEG, BMP and GIF natively and quickly. WebP needs Qt's qtimageformats
// plugin, which is NOT in every build -- it is absent from the trimmed obs-deps
// Qt used for releases -- and the dialog offered *.webp regardless, so picking
// one produced "Could not read that image." with no clue why. libav is already
// linked for the video path and decodes WebP, so it is the fallback for
// anything Qt declines.
QImage readStillImage(const QString &path, QString *why)
{
	const QFileInfo fi(path);
	if (!fi.exists()) {
		if (why)
			*why = QStringLiteral("There is no file at %1.").arg(QDir::toNativeSeparators(path));
		return {};
	}
	if (!fi.isReadable()) {
		if (why)
			*why = QStringLiteral("%1 cannot be read — check the file's permissions.")
				       .arg(fi.fileName());
		return {};
	}

	// A still used as a clip is legitimately large; Qt's 128 MB default refuses
	// a 6000x6000 photo outright. Raised rather than removed -- an unbounded
	// limit is how a malformed header turns into an out-of-memory kill.
	if (QImageReader::allocationLimit() < 512)
		QImageReader::setAllocationLimit(512);

	QImageReader reader(path);
	reader.setAutoTransform(true); // honour the EXIF orientation of a phone photo
	QImage img = reader.read();
	if (!img.isNull())
		return img;
	const QString qtErr = reader.errorString();

	// Fall back to libav, which reads formats this Qt build has no plugin for.
	FrameSeeker fs;
	if (fs.open(path)) {
		// A generous bound rather than the preview's: this is the source, and
		// downscaling it here would throw away detail the canvas may want.
		const QImage frame = fs.frameAt(0, 16384, 16384);
		if (!frame.isNull())
			return frame;
	}

	if (why) {
		QStringList qtFormats;
		for (const QByteArray &f : QImageReader::supportedImageFormats())
			qtFormats << QString::fromLatin1(f);
		*why = QStringLiteral("%1 could not be decoded.\n\nQt said: %2\nThe video decoder "
				      "could not read it either.\n\nThis build reads: %3 (plus "
				      "anything the video decoder handles).")
			       .arg(fi.fileName(), qtErr, qtFormats.join(QStringLiteral(", ")));
	}
	return {};
}

} // namespace harpia
