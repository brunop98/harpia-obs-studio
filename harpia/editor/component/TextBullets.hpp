#pragma once

// Bullets that arrive on cue, with the cue written next to the words:
//
//     [0]   Framing the shot
//     [3.5] Setting the exposure
//     [7]   Rolling
//
// The marker is a time in SECONDS from the start of the clip. It is stripped
// before the caption is drawn, so what ends up on screen is exactly what you
// typed after it.
//
// Why in the text rather than as properties or keyframes: a bullet's time
// belongs to that bullet. Keyframing a count means the words are in one place
// and their timing in another, and re-ordering two bullets then means
// re-timing them too. Here you cut a line and its cue comes with it.
//
// Everything is a pure function of (caption, time), so the whole feature is
// checkable without a canvas, a clip or a window -- the component itself is a
// handful of lines that call in here.

#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace harpia {

// One line of the caption, with the moment it is due.
struct BulletLine {
	qint64 atMs = 0;  // when this line joins the picture
	QString text;     // the line as it should be DRAWN: marker removed
	bool timed = false; // it carried a marker of its own
};

// Parse a caption into its lines.
//
// A line with no marker inherits the time of the line above it, so a bullet
// that wraps onto a second line arrives with its first -- and a caption with no
// markers at all is simply on screen from the start, which is what an ordinary
// caption does today.
inline QVector<BulletLine> parseBulletLines(const QString &caption)
{
	QVector<BulletLine> out;
	qint64 running = 0;
	const QStringList lines = caption.split(QLatin1Char('\n'));
	out.reserve(lines.size());
	for (const QString &raw : lines) {
		BulletLine bl;
		bl.text = raw;

		// "[" <spaces> <number> <spaces> "]" and nothing else before it. A
		// leading space is allowed because people indent bullets.
		int i = 0;
		while (i < raw.size() && raw.at(i).isSpace())
			++i;
		if (i < raw.size() && raw.at(i) == QLatin1Char('[')) {
			const int close = raw.indexOf(QLatin1Char(']'), i + 1);
			if (close > i) {
				const QString inner = raw.mid(i + 1, close - i - 1).trimmed();
				bool okNum = false;
				const double secs = inner.toDouble(&okNum);
				// Negative is not a time, and neither is "[note]". Both are
				// left exactly as typed rather than half-eaten: a caption
				// that quietly loses a bracketed word is worse than one that
				// shows a marker you did not mean.
				if (okNum && !inner.isEmpty() && secs >= 0.0) {
					bl.atMs = qint64(std::llround(secs * 1000.0));
					bl.timed = true;
					// The words after the marker, with the spaces that
					// separated them dropped. All of them, not one: a
					// list is written with its words lined up under
					// each other, and that padding is there to line up
					// the MARKERS -- drawing it would indent every line
					// by however wide its own cue happened to be.
					int j = close + 1;
					while (j < raw.size() && raw.at(j) == QLatin1Char(' '))
						++j;
					bl.text = raw.mid(j);
					running = bl.atMs;
				}
			}
		}
		if (!bl.timed)
			bl.atMs = running;
		out.append(bl);
	}
	return out;
}

// The caption as it should be drawn at `tMs` into the clip.
//
// `accumulate` is the difference between a list that builds up and a caption
// that changes: with it, every line due by now is on screen; without it, only
// the most recent GROUP -- the run of lines sharing one moment, which is how a
// bullet plus its wrapped continuation stay together.
inline QString bulletsAt(const QString &caption, qint64 tMs, bool accumulate)
{
	const QVector<BulletLine> lines = parseBulletLines(caption);
	if (lines.isEmpty())
		return QString();

	// The latest moment that has arrived. Not simply the last due line: times
	// do not have to increase down the caption, and "the newest thing on
	// screen" is what one-at-a-time means.
	qint64 newest = -1;
	for (const BulletLine &b : lines)
		if (b.atMs <= tMs)
			newest = std::max(newest, b.atMs);
	if (newest < 0)
		return QString(); // nothing is due yet

	QStringList shown;
	for (const BulletLine &b : lines) {
		if (b.atMs > tMs)
			continue;
		if (!accumulate && b.atMs != newest)
			continue;
		shown << b.text;
	}
	return shown.join(QLatin1Char('\n'));
}

// Does this caption use the marker at all? For the component, so a clip whose
// text has no cues is left completely alone rather than being blanked until
// time zero arrives.
inline bool captionHasBulletCues(const QString &caption)
{
	for (const BulletLine &b : parseBulletLines(caption))
		if (b.timed)
			return true;
	return false;
}

} // namespace harpia
