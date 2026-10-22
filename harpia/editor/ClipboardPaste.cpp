#include "ClipboardPaste.hpp"

#include "MediaFiles.hpp"

#include <QMimeData>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace harpia {

namespace {

// A WAV is a RIFF file whose form type is WAVE. Anything else claiming to be
// one is not worth decoding: the decoder would fail later, further from the
// paste that caused it.
bool looksLikeWav(const QByteArray &b)
{
	return b.size() > 44 && b.startsWith("RIFF") && b.mid(8, 4) == "WAVE";
}

} // namespace

QString extensionForAudioMime(const QString &mimeType)
{
	const QString m = mimeType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
	if (m == QLatin1String("audio/wav") || m == QLatin1String("audio/x-wav") || m == QLatin1String("audio/wave") ||
	    m == QLatin1String("audio/vnd.wave"))
		return QStringLiteral("wav");
	if (m == QLatin1String("audio/mpeg") || m == QLatin1String("audio/mp3"))
		return QStringLiteral("mp3");
	if (m == QLatin1String("audio/ogg") || m == QLatin1String("audio/vorbis"))
		return QStringLiteral("ogg");
	if (m == QLatin1String("audio/opus"))
		return QStringLiteral("opus");
	if (m == QLatin1String("audio/flac") || m == QLatin1String("audio/x-flac"))
		return QStringLiteral("flac");
	if (m == QLatin1String("audio/aac"))
		return QStringLiteral("aac");
	if (m == QLatin1String("audio/mp4") || m == QLatin1String("audio/x-m4a") || m == QLatin1String("audio/m4a"))
		return QStringLiteral("m4a");
	if (m == QLatin1String("audio/aiff") || m == QLatin1String("audio/x-aiff"))
		return QStringLiteral("aiff");
	return QString();
}

QString extensionForContentType(const QString &contentType)
{
	const QString m = contentType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
	if (m.startsWith(QLatin1String("audio/")))
		return extensionForAudioMime(m);
	static const QList<QPair<QLatin1String, QLatin1String>> kOthers = {
		{QLatin1String("image/png"), QLatin1String("png")},
		{QLatin1String("image/jpeg"), QLatin1String("jpg")},
		{QLatin1String("image/jpg"), QLatin1String("jpg")},
		{QLatin1String("image/gif"), QLatin1String("gif")},
		{QLatin1String("image/webp"), QLatin1String("webp")},
		{QLatin1String("image/bmp"), QLatin1String("bmp")},
		{QLatin1String("image/tiff"), QLatin1String("tiff")},
		{QLatin1String("video/mp4"), QLatin1String("mp4")},
		{QLatin1String("video/quicktime"), QLatin1String("mov")},
		{QLatin1String("video/webm"), QLatin1String("webm")},
		{QLatin1String("video/x-matroska"), QLatin1String("mkv")},
		{QLatin1String("video/x-msvideo"), QLatin1String("avi")},
		{QLatin1String("video/x-m4v"), QLatin1String("m4v")},
		{QLatin1String("video/mp2t"), QLatin1String("ts")},
		{QLatin1String("video/x-flv"), QLatin1String("flv")},
		{QLatin1String("video/x-ms-wmv"), QLatin1String("wmv")},
	};
	for (const auto &pr : kOthers)
		if (m == pr.first)
			return QString(pr.second);
	return QString();
}

QUrl singleWebLink(const QString &text)
{
	const QString t = text.trimmed();
	if (t.isEmpty() || t.contains(QLatin1Char(' ')) || t.contains(QLatin1Char('\n')) ||
	    t.contains(QLatin1Char('\t')))
		return QUrl();
	const QUrl u(t, QUrl::StrictMode);
	if (!u.isValid() || u.host().isEmpty())
		return QUrl();
	const QString scheme = u.scheme().toLower();
	if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
		return QUrl();
	return u;
}

PasteContent readPaste(const QMimeData *mime, const QByteArray &nativeWave)
{
	PasteContent c;

	// 1. Files. A file copied in the file manager also puts a thumbnail on
	// some clipboards, and the file is unambiguously what was meant.
	if (mime && mime->hasUrls()) {
		for (const QUrl &u : mime->urls()) {
			const QString f = u.toLocalFile();
			if (!f.isEmpty() && isMediaFile(f))
				c.files << f;
		}
		if (!c.files.isEmpty()) {
			c.kind = PasteContent::Kind::Files;
			return c;
		}
	}

	// 2. Sound as data.
	if (looksLikeWav(nativeWave)) {
		c.kind = PasteContent::Kind::Audio;
		c.audio = nativeWave;
		c.audioExt = QStringLiteral("wav");
		return c;
	}
	if (mime) {
		for (const QString &f : mime->formats()) {
			const QString ext = extensionForAudioMime(f);
			if (ext.isEmpty())
				continue;
			const QByteArray bytes = mime->data(f);
			if (bytes.isEmpty() || (ext == QLatin1String("wav") && !looksLikeWav(bytes)))
				continue;
			c.kind = PasteContent::Kind::Audio;
			c.audio = bytes;
			c.audioExt = ext;
			return c;
		}
	}
	if (!mime)
		return c;

	const QString text = mime->hasText() ? mime->text().trimmed() : QString();
	const QUrl link = singleWebLink(text);

	// 3. A picture -- when no words came with it. A browser's "Copy image" may
	// bring the picture's address along as text, which is still a picture.
	if (mime->hasImage() && (text.isEmpty() || link.isValid())) {
		const QImage img = qvariant_cast<QImage>(mime->imageData());
		if (!img.isNull()) {
			c.kind = PasteContent::Kind::Image;
			c.image = img;
			return c;
		}
	}

	// 4. One web address. A link copied in a browser can also arrive as a
	// URL list rather than text.
	if (link.isValid()) {
		c.kind = PasteContent::Kind::Link;
		c.link = link;
		return c;
	}
	if (text.isEmpty() && mime->hasUrls()) {
		const QList<QUrl> urls = mime->urls();
		if (urls.size() == 1) {
			const QUrl u = singleWebLink(urls.first().toString());
			if (u.isValid()) {
				c.kind = PasteContent::Kind::Link;
				c.link = u;
				return c;
			}
		}
	}

	// 5. Words.
	if (!text.isEmpty()) {
		c.kind = PasteContent::Kind::Text;
		c.text = text;
	}
	return c;
}

QByteArray clipboardWave()
{
#if defined(_WIN32)
	QByteArray out;
	if (!IsClipboardFormatAvailable(CF_WAVE) || !OpenClipboard(nullptr))
		return out;
	if (HANDLE h = GetClipboardData(CF_WAVE)) {
		if (const void *p = GlobalLock(h)) {
			out = QByteArray(static_cast<const char *>(p), int(GlobalSize(h)));
			GlobalUnlock(h);
		}
		// The block is rounded up by the allocator; the RIFF header says how
		// much of it is the sound.
		if (out.size() >= 8 && out.startsWith("RIFF")) {
			const quint32 riff = quint32(quint8(out[4])) | quint32(quint8(out[5])) << 8 |
					     quint32(quint8(out[6])) << 16 | quint32(quint8(out[7])) << 24;
			if (qint64(riff) + 8 < out.size())
				out.truncate(int(riff) + 8);
		}
	}
	CloseClipboard();
	return out;
#else
	return QByteArray();
#endif
}

bool clipboardHasWave()
{
#if defined(_WIN32)
	return IsClipboardFormatAvailable(CF_WAVE) != FALSE;
#else
	return false;
#endif
}

} // namespace harpia
