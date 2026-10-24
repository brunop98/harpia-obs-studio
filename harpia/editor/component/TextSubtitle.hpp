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
#include "../timeline/RichText.hpp"

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace harpia {

enum class SubtitleMode { Whole = 0, OneWord = 1, BuildUp = 2, HighlightCurrent = 3, HighlightSpoken = 4 };
constexpr int kSubtitleModeCount = 5;

// The caption split into words, whitespace-separated, blanks dropped.
inline QStringList subtitleWordsOf(const QString &caption)
{
	static thread_local const QRegularExpression whitespace(QStringLiteral("\\s+"));
	return caption.split(whitespace, Qt::SkipEmptyParts);
}

// The words with their times, reconciled with the caption as it stands now.
// Same count: the caption's spelling with the stored timing, word for word.
// Different count: the caption's words spread evenly over [first start, last
// end]. No timing at all: spread over `fallbackSpanMs` (the clip's length),
// so a hand-typed caption given the component still animates across itself.
// Empty when there is nothing to time.
inline QVector<ClipWordTime> paceCaptionWords(const QStringList &words, const QVector<ClipWordTime> &timed,
					    qint64 fallbackSpanMs)
{
	QVector<ClipWordTime> out;
	if (words.isEmpty())
		return out;
	if (timed.size() == words.size()) {
		bool sameText = true;
		for (int i = 0; i < timed.size(); ++i)
			if (timed.at(i).text != words.at(i)) {
				sameText = false;
				break;
			}
		if (sameText)
			return timed; // keep the shared word clock when nothing needs rewriting
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

inline QVector<ClipWordTime> reconciledWords(const QString &caption, const QVector<ClipWordTime> &timed,
					     qint64 fallbackSpanMs = 0)
{
	return paceCaptionWords(subtitleWordsOf(caption), timed, fallbackSpanMs);
}

// Animation follows the words a viewer can see. Tags never become words of
// their own, and a <br> is the same boundary as a typed line break.
inline QVector<ClipWordTime> timedCaptionWords(const QString &caption, const QVector<ClipWordTime> &timed,
					      qint64 fallbackSpanMs = 0)
{
	const QString plain = rich_text::plainText(caption);
	static thread_local const QRegularExpression whitespace(
		QStringLiteral("\\s+"), QRegularExpression::UseUnicodePropertiesOption);
	const QStringList words = plain.split(whitespace, Qt::SkipEmptyParts);
	return paceCaptionWords(words, timed, fallbackSpanMs);
}

namespace caption_animation {

// A visible run keeps its original characters and markup. Highlighting is
// inserted inside existing tags, so a keyword's own colour returns as soon
// as the spoken-word highlight moves on.
struct TextPart {
	QString text;
	int word = -1;
	bool noParse = false;
	bool markup = false;
};

inline QVector<TextPart> partsOf(const QString &caption)
{
	QVector<TextPart> out;
	int word = -1;
	bool inWord = false, noParse = false;
	for (int i = 0; i < caption.size();) {
		if (caption.at(i) == QLatin1Char('<')) {
			const auto tag = rich_text::tagAt(caption, i);
			using K = rich_text::Tag::Kind;
			if (tag.kind != K::None && (!noParse || (tag.kind == K::NoParse && tag.closing))) {
				out.append({caption.mid(i, tag.length), -1, false, true});
				if (tag.kind == K::NoParse)
					noParse = !tag.closing;
				if (tag.kind == K::Break)
					inWord = false;
				i += tag.length;
				continue;
			}
		}
		const bool space = caption.at(i).isSpace();
		if (!space && !inWord)
			++word;
		inWord = !space;
		const int index = space ? -1 : word;
		if (!out.isEmpty() && out.last().word == index && out.last().noParse == noParse &&
		    !out.last().markup)
			out.last().text += caption.at(i);
		else
			out.append({QString(caption.at(i)), index, noParse});
		++i;
	}
	return out;
}

inline QString colorValue(const QColor &color)
{
	const QColor c = color.isValid() ? color : QColor(255, 228, 94);
	QString value = c.name(QColor::HexRgb);
	if (c.alpha() != 255)
		value += QStringLiteral("%1").arg(c.alpha(), 2, 16, QLatin1Char('0'));
	return value;
}

inline QString highlight(const QString &caption, const QVector<ClipWordTime> &words, qint64 tMs,
			 bool spoken, const QColor &activeColor)
{
	QVector<bool> active(words.size(), false);
	bool any = false;
	int latest = -1;
	for (int i = 0; i < words.size(); ++i) {
		if (words[i].startMs <= tMs) {
			if (latest < 0 || words[i].startMs >= words[latest].startMs)
				latest = i;
			if (spoken)
				active[i] = any = true;
		}
	}
	if (!spoken && latest >= 0 && tMs < words[latest].endMs)
		active[latest] = any = true;
	if (!any)
		return caption;
	const QString open = QStringLiteral("<color=%1>").arg(colorValue(activeColor));
	QString out;
	for (const auto &part : partsOf(caption)) {
		if (part.word < 0 || part.word >= active.size() || !active[part.word]) {
			out += part.text;
			continue;
		}
		if (part.noParse)
			out += QStringLiteral("</noparse>") + open + QStringLiteral("<noparse>") + part.text +
			       QStringLiteral("</noparse></color><noparse>");
		else
			out += open + part.text + QStringLiteral("</color>");
	}
	return out;
}

} // namespace caption_animation

// A short, word-synchronised punch composes with the clip's own zoom. The
// result depends only on this instant, so scrubbing and exporting agree.
inline double captionPopScaleAt(const QString &caption, const QVector<ClipWordTime> &timed,
			       qint64 tMs, qint64 fallbackSpanMs, qint64 durationMs = 120,
			       double strength = 0.10)
{
	// A preceding subtitle Source component has already reconciled these
	// timings against the complete phrase, before revealing only part of it.
	const auto words = timed.isEmpty() ? timedCaptionWords(caption, {}, fallbackSpanMs) : timed;
	int current = -1;
	for (int i = 0; i < words.size(); ++i)
		if (words[i].startMs <= tMs &&
		    (current < 0 || words[i].startMs >= words[current].startMs))
			current = i;
	if (current < 0)
		return 1.0;
	qint64 span = std::clamp<qint64>(durationMs, 1, 500);
	for (const auto &word : words)
		if (word.startMs > words[current].startMs)
			span = std::min(span, word.startMs - words[current].startMs);
	const qint64 elapsed = tMs - words[current].startMs;
	if (elapsed < 0 || elapsed >= span)
		return 1.0;
	const double amount = std::isfinite(strength) ? std::clamp(strength, 0.0, 0.5) : 0.10;
	const double pulse = std::sin(3.14159265358979323846 * double(elapsed) / double(span));
	return 1.0 + amount * pulse * pulse;
}

// What to draw at `tMs` (clip-relative). Before the first word: nothing for
// OneWord and BuildUp (the speaker has not started), the phrase for Whole.
// Between words: OneWord keeps the last word up until the next starts, so the
// screen never flickers empty mid-sentence.
inline QString subtitleTextAt(const QString &caption, const QVector<ClipWordTime> &timed, qint64 tMs,
			      SubtitleMode mode, qint64 fallbackSpanMs = 0,
			      const QColor &activeColor = QColor(255, 228, 94))
{
	if (mode == SubtitleMode::Whole)
		return caption;
	if (mode == SubtitleMode::HighlightCurrent || mode == SubtitleMode::HighlightSpoken)
		return caption_animation::highlight(caption, timedCaptionWords(caption, timed, fallbackSpanMs),
						    tMs, mode == SubtitleMode::HighlightSpoken, activeColor);
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
