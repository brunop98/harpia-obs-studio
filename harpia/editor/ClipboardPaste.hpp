#pragma once

// What the clipboard holds, as something the timeline can take.
//
// Add > Paste and Ctrl+V put "whatever you copied" on the timeline, and a
// clipboard holds one copy in several shapes at once: a picture copied in a
// browser arrives as image data and HTML, a file copied in Explorer as a URL
// list (sometimes with a thumbnail), a sentence copied from Word as text, rich
// text AND a picture of the text. readPaste() decides which one was meant, in
// one place, so the menu entry's greying and the paste itself cannot disagree.
//
// Most to least specific:
//   1. Files  media files copied in a file manager -- used where they lie;
//   2. Audio  sound copied in an audio app, as audio data rather than a file
//             (the WAV a Windows app puts on the clipboard, or an audio/*
//             type elsewhere);
//   3. Image  a screenshot or a copied picture -- unless the clipboard also
//             holds real words, which makes the picture a rendering of that
//             text (Word, Excel) rather than the thing that was copied;
//   4. Link   text that is one web address and nothing else: downloaded when
//             it is a media file, handed to yt-dlp when it is a video page;
//   5. Text   any other words become a text clip.

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QUrl>

class QMimeData;

namespace harpia {

struct PasteContent {
	enum class Kind { Nothing, Files, Audio, Image, Link, Text };
	Kind kind = Kind::Nothing;
	QStringList files;  // Files: local paths, media only
	QByteArray audio;   // Audio: a whole audio file's bytes
	QString audioExt;   // ...and its extension ("wav", "mp3", ...)
	QImage image;       // Image
	QUrl link;          // Link: http or https
	QString text;       // Text: trimmed
};

// `nativeWave` is a WAV the platform's clipboard holds that Qt does not show
// (Windows' CF_WAVE): pass clipboardWave().
PasteContent readPaste(const QMimeData *mime, const QByteArray &nativeWave = QByteArray());

// Windows: the CF_WAVE sound on the clipboard, as the bytes of a WAV file.
// Qt only shows the clipboard formats it has converters for, and this is not
// one of them. Empty when there is none, and on every other platform.
QByteArray clipboardWave();
// The same question without copying the sound (for greying the menu entry).
bool clipboardHasWave();

// `text` as one web address (http or https) and nothing else, or an invalid
// QUrl. Surrounding spaces and a trailing newline are allowed; words are not.
QUrl singleWebLink(const QString &text);

// The file extension for a media Content-Type ("image/png" -> "png",
// "audio/mpeg" -> "mp3"), or empty for anything that is not a picture, a sound
// or a video -- an HTML page included. Parameters (";charset=...") ignored.
QString extensionForContentType(const QString &contentType);

// The extension for an audio clipboard format ("audio/wav" -> "wav"), or empty.
QString extensionForAudioMime(const QString &mimeType);

} // namespace harpia
