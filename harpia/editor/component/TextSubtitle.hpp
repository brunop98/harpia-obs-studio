#pragma once

// Word-timed captions: what a subtitle clip SHOWS at an instant.
//
// A subtitle clip carries its caption (what the user can edit in the
// Inspector) and, alongside, the words with the times the speaker said them
// (from the transcription). The Subtitle component uses both to decide what
// is drawn right now -- the whole phrase, one word at a time, or the phrase
// building up word by word -- without ever rewriting the stored text.
//
// The two can disagree: the user fixes a typo, adds a word, drops one. The
// rule is then simple and predictable: when the caption's words no longer
// match the timed words one for one, the timings are re-spread evenly across
// the span the original words covered. A one-word fix keeps every timing; a
// rewrite gets even pacing, which is the honest thing to do with no better
// information.

#include "Component.hpp" // ClipWordTime

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace harpia {

enum class SubtitleMode { Whole = 0, OneWord = 1, BuildUp = 2 };
constexpr int kSubtitleModeCount = 3;

// The caption split into words, whitespace-separated, blanks dropped.
inline QStringList subtitleWordsOf(const QString &caption)
{
	return caption.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

// The words with their times, reconciled with the caption as it stands now.
// Same count: the caption's spelling with the stored timing, word for word.
// Different count: the caption's words spread evenly over [first start, last
// end]. No timing at all: spread over `fallbackSpanMs` (the clip's length),
// so a hand-typed caption given the component still animates across itself.
// Empty when there is nothing to time.
inline QVector<ClipWordTime> reconciledWords(const QString &caption, const QVector<ClipWordTime> &timed,
					     qint64 fallbackSpanMs = 0)
{
	const QStringList words = subtitleWordsOf(caption);
	QVector<ClipWordTime> out;
	if (words.isEmpty())
		return out;
	if (timed.size() == words.size()) {
		out = timed;
		for (int i = 0; i < out.size(); ++i)
			out[i].text = words[i];
		return out;
	}
	qint64 from = 0, to = 0;
	if (!timed.isEmpty()) {
		from = timed.first().startMs;
		to = timed.last().endMs;
		for (const ClipWordTime &w : timed) {
			from = std::min(from, w.startMs);
			to = std::max(to, w.endMs);
		}
	}
	if (to <= from)
		to = from + (fallbackSpanMs > 0 ? fallbackSpanMs : 1000 * words.size()); // no timing: the clip, else a second a word
	const double step = double(to - from) / words.size();
	for (int i = 0; i < words.size(); ++i) {
		ClipWordTime w;
		w.text = words[i];
		w.startMs = from + qint64(std::llround(i * step));
		w.endMs = from + qint64(std::llround((i + 1) * step));
		out.append(w);
	}
	return out;
}

// What to draw at `tMs` (clip-relative). Before the first word: nothing for
// OneWord and BuildUp (the speaker has not started), the phrase for Whole.
// Between words: OneWord keeps the last word up until the next starts, so the
// screen never flickers empty mid-sentence.
inline QString subtitleTextAt(const QString &caption, const QVector<ClipWordTime> &timed, qint64 tMs,
			      SubtitleMode mode, qint64 fallbackSpanMs = 0)
{
	if (mode == SubtitleMode::Whole)
		return caption;
	const QVector<ClipWordTime> words = reconciledWords(caption, timed, fallbackSpanMs);
	if (words.isEmpty())
		return QString();
	int current = -1; // the last word whose start has passed
	for (int i = 0; i < words.size(); ++i)
		if (words[i].startMs <= tMs)
			current = i;
	if (current < 0)
		return QString();
	if (mode == SubtitleMode::OneWord)
		return words[current].text;
	QStringList shown;
	for (int i = 0; i <= current; ++i)
		shown << words[i].text;
	return shown.join(QLatin1Char(' '));
}

} // namespace harpia
