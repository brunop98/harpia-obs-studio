#pragma once

// Colour emoji in text clips: Google's Noto emoji (the Android / Gmail / Google
// Chat artwork), drawn as pictures where the emoji are in the text.
//
// A text clip is drawn from glyph OUTLINES (QPainterPath::addText), so it can
// be outlined, filled per colour and cached -- and an outline has one colour.
// An emoji came out as a flat silhouette in the text colour (a white skull for
// 💀), and on Windows, whose system emoji font Qt draws in one colour anyway,
// the same in the boxes around it. So the text is split into plain text and
// emoji; the text is shaped as before, and each emoji is given a square the
// size of the font and drawn there as a picture.
//
// The pictures are a pack next to the program (resources/emoji/noto-emoji.pack,
// built by scripts/make-emoji-pack.py): 4012 emoji (flags included) at 128 px,
// as WebP, decoded
// with FFmpeg the first time each is drawn. Without the pack nothing changes:
// every piece is text.

#include <QFont>
#include <QImage>
#include <QRectF>
#include <QString>
#include <QVector>

namespace harpia::emoji {

struct Piece {
	QString text; // the characters as typed
	QString key;  // the emoji's picture ("1f480"), empty for plain text
	bool isEmoji() const { return !key.isEmpty(); }
};

// Where the pack is (default: next to the program, or $HARPIA_EMOJI_PACK).
// For tests, and for a pack somewhere else.
void setPackPath(const QString &path);
bool available();
int count();

// The picture's key for one emoji (a grapheme cluster): its code points in
// lower-case hex, at least 4 digits, joined by "_", without U+FE0F.
QString keyFor(const QString &cluster);

// The text as plain-text runs and single emoji, in order. An emoji is a
// cluster the pack has a picture for that reads as an emoji: an emoji by
// Unicode's rules (😀, ⭐), a sequence (👍🏽, 👨‍👩‍👧, 🇧🇷), or anything asked to
// look like one (U+FE0F, a keycap). "1", "©", "™" and "↔" on their own stay
// text, as everywhere else.
QVector<Piece> split(const QString &text);

// The picture (premultiplied ARGB, 128 px), or null. Thread-safe; cached.
QImage image(const QString &key);
// The picture at `side` device pixels, scaled down smoothly (drawing the 128 px
// picture straight into a small square would only sample it, and look rough).
// Cached; past 128 px, the 128 px picture.
QImage image(const QString &key, int side);

// An emoji's room in a line of `font`: how far the pen moves, and the square
// it is drawn in from pen position x on `baseline`.
double advance(const QFont &font);
QRectF square(const QFont &font, double x, double baseline);
// A run of text, emoji counted as above.
double width(const QFont &font, const QString &text);

} // namespace harpia::emoji
