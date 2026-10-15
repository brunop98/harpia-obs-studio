#pragma once

// What the Text to speech window does with the text before and after the
// service speaks it -- pure, so all of it is checked in a test.
//
//   * Pauses: "[pause 1s]", "[pause 500ms]" ("[pausa 1s]" too) or a blank
//     line split the text into segments with that much silence between them
//     (ttsSegments). The markers are never spoken nor shown.
//   * Pronunciation: "Harpia = Árpia", one rule per line. The service is sent
//     the spoken form; the captions keep the text as written (ttsSpoken maps
//     each written word to the spoken words that say it, ttsWrittenTimes
//     brings the times back).
//   * Re-speaking one sentence of a narration: where the sentence is in the
//     audio (sentenceAround), and the narration with the new take spliced in
//     its place (spliceSentence) -- everything after it moves by the
//     difference in length.

#include "TtsAudio.hpp"
#include "TtsProviders.hpp"

#include <QRegularExpression>
#include <QStringList>

namespace harpia {

// ---- pauses -----------------------------------------------------------------

struct TtsSegment {
	QString text;
	qint64 pauseBeforeMs = 0; // silence before it (0 for the first)
};

constexpr qint64 kBlankLinePauseMs = 700;

inline QVector<TtsSegment> ttsSegments(const QString &text)
{
	static const QRegularExpression marker(
		QStringLiteral("\\[\\s*(?:pause|pausa)\\s*(?:(\\d+(?:[.,]\\d+)?)\\s*(ms|s|seg|sec)?)?\\s*\\]|\\n[ \\t]*\\n\\s*"),
		QRegularExpression::CaseInsensitiveOption);
	QVector<TtsSegment> out;
	qint64 pending = 0;
	qsizetype from = 0;
	const auto flush = [&](qsizetype to) {
		const QString t = text.mid(from, to - from).trimmed();
		if (t.isEmpty() || !ttsHasLetter(t))
			return;
		out.append({t, out.isEmpty() ? 0 : pending});
		pending = 0;
	};
	auto it = marker.globalMatch(text);
	while (it.hasNext()) {
		const QRegularExpressionMatch m = it.next();
		flush(m.capturedStart());
		if (m.captured(0).startsWith(QLatin1Char('['))) {
			const QString num = m.captured(1);
			const QString unit = m.captured(2).toLower();
			const double v = num.isEmpty() ? 1.0 : QString(num).replace(QLatin1Char(','), QLatin1Char('.')).toDouble();
			pending += unit == QLatin1String("ms") ? qint64(std::llround(v)) : qint64(std::llround(v * 1000.0));
		} else {
			pending += kBlankLinePauseMs;
		}
		from = m.capturedEnd();
	}
	flush(text.size());
	return out;
}

// The text as it will be said and shown: no markers, no blank lines.
inline QString ttsPlainText(const QString &text)
{
	QStringList parts;
	for (const TtsSegment &s : ttsSegments(text))
		parts << s.text;
	return parts.join(QLatin1Char(' '));
}

// ---- pronunciation ------------------------------------------------------------

struct PronunciationRule {
	QStringList written; // the words it replaces, normalised (ttsNormal)
	QStringList spoken;  // what the service is sent instead
	bool operator==(const PronunciationRule &o) const { return written == o.written && spoken == o.spoken; }
};

// "Harpia = Árpia" (or -> or →), one per line; "#" starts a comment. Lines
// that are not rules are reported in *bad, with their number.
inline QVector<PronunciationRule> parsePronunciations(const QString &lines, QStringList *bad = nullptr)
{
	QVector<PronunciationRule> out;
	const QStringList all = lines.split(QLatin1Char('\n'));
	for (int n = 0; n < all.size(); ++n) {
		const QString line = all[n].trimmed();
		if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
			continue;
		qsizetype at = -1, len = 0;
		for (const QString sep : {QStringLiteral("->"), QStringLiteral("→"), QStringLiteral("=")}) {
			at = line.indexOf(sep);
			if (at >= 0) {
				len = sep.size();
				break;
			}
		}
		PronunciationRule r;
		if (at > 0) {
			for (const QString &w : line.left(at).split(QLatin1Char(' '), Qt::SkipEmptyParts))
				if (!ttsNormal(w).isEmpty())
					r.written << ttsNormal(w);
			for (const QString &w : line.mid(at + len).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
				if (!ttsHasLetter(w) && !r.spoken.isEmpty())
					r.spoken.last() += QLatin1Char(' ') + w; // punctuation stays with its word
				else if (ttsHasLetter(w))
					r.spoken << w;
			}
		}
		if (r.written.isEmpty() || r.spoken.isEmpty()) {
			if (bad)
				*bad << QStringLiteral("line %1: \"%2\"").arg(n + 1).arg(line);
			continue;
		}
		out.append(r);
	}
	return out;
}

// The text as the service is sent it, and which spoken words say each
// written one.
struct SpokenText {
	QString spoken;
	QVector<TtsWord> written;
	QVector<QPair<int, int>> spokenOf; // written word i -> spoken words [first, last]
	int spokenCount = 0;
};

namespace ttstextdetail {
inline QString edgePunct(const QString &s, bool leading)
{
	QString out;
	if (leading) {
		for (const QChar c : s) {
			if (c.isLetterOrNumber())
				break;
			out += c;
		}
	} else {
		for (qsizetype i = s.size() - 1; i >= 0 && !s[i].isLetterOrNumber(); --i)
			out.prepend(s[i]);
	}
	return out;
}
} // namespace ttstextdetail

inline SpokenText ttsSpoken(const QString &written, const QVector<PronunciationRule> &rules)
{
	SpokenText st;
	st.written = ttsWords(written);
	const int n = int(st.written.size());
	QVector<QString> cores(n);
	for (int i = 0; i < n; ++i)
		cores[i] = ttsNormal(st.written[i].text);
	// Longest rule first: "Harpia Recorder" before "Harpia".
	QVector<const PronunciationRule *> order;
	for (const PronunciationRule &r : rules)
		order.append(&r);
	std::stable_sort(order.begin(), order.end(), [](const PronunciationRule *a, const PronunciationRule *b) {
		return a->written.size() > b->written.size();
	});
	QStringList tokens;
	st.spokenOf.resize(n);
	for (int i = 0; i < n;) {
		const PronunciationRule *hit = nullptr;
		for (const PronunciationRule *r : order) {
			const int k = int(r->written.size());
			if (k == 0 || i + k > n)
				continue;
			bool all = true;
			for (int j = 0; j < k && all; ++j)
				all = cores[i + j] == r->written[j];
			if (all) {
				hit = r;
				break;
			}
		}
		if (!hit) {
			st.spokenOf[i] = {int(tokens.size()), int(tokens.size())};
			tokens << st.written[i].text;
			++i;
			continue;
		}
		const int k = int(hit->written.size());
		// The written words' own punctuation goes round the spoken ones, so the
		// pause and the intonation stay: "(OBS," -> "(ó bê ésse,".
		QStringList sp = hit->spoken;
		sp.first().prepend(ttstextdetail::edgePunct(st.written[i].text, true));
		sp.last().append(ttstextdetail::edgePunct(st.written[i + k - 1].text, false));
		const int a = int(tokens.size());
		tokens << sp;
		for (int j = 0; j < k; ++j)
			st.spokenOf[i + j] = {a, int(tokens.size()) - 1};
		i += k;
	}
	st.spoken = tokens.join(QLatin1Char(' '));
	st.spokenCount = int(ttsWords(st.spoken).size());
	return st;
}

// The written words' times, from the spoken words' (as the service or the
// transcription gave them). Written words that share spoken words share
// their time, by length.
inline QVector<ClipWordTime> ttsWrittenTimes(const SpokenText &st, const QVector<ClipWordTime> &spoken)
{
	const int n = int(st.written.size()), m = int(spoken.size());
	QVector<ClipWordTime> out(n);
	for (int i = 0; i < n; ++i)
		out[i].text = st.written[i].text;
	if (m == 0)
		return out;
	for (int i = 0; i < n;) {
		int j = i;
		while (j + 1 < n && st.spokenOf[j + 1] == st.spokenOf[i])
			++j;
		const int a = std::clamp(st.spokenOf[i].first, 0, m - 1);
		const int b = std::clamp(st.spokenOf[i].second, a, m - 1);
		const qint64 s = spoken[a].startMs, e = std::max(s, spoken[b].endMs);
		int letters = 0;
		for (int k = i; k <= j; ++k)
			letters += std::max(1, int(st.written[k].text.size()));
		qint64 t = s;
		for (int k = i; k <= j; ++k) {
			const qint64 len = k == j ? e - t : (e - s) * std::max(1, int(st.written[k].text.size())) / letters;
			out[k].startMs = t;
			out[k].endMs = t + len;
			t += len;
		}
		i = j + 1;
	}
	return out;
}

// ---- re-speaking one sentence ---------------------------------------------------

// One caption of a narration: its text, and its words' times in the
// narration's own audio (source time).
struct NarrationCaption {
	QString text;
	QVector<ClipWordTime> words;
	qint64 firstMs() const { return words.isEmpty() ? 0 : words.first().startMs; }
	qint64 lastMs() const { return words.isEmpty() ? 0 : words.last().endMs; }
};

inline bool endsSentence(const QString &text)
{
	QString t = text.trimmed();
	while (!t.isEmpty() && QStringLiteral("\"'”’)]»").contains(t.back()))
		t.chop(1);
	return !t.isEmpty() && QStringLiteral(".!?…。！？").contains(t.back());
}

// The sentence caption `k` belongs to: from the caption after the previous
// sentence's end to the caption that ends this one. A sentence ends with
// its punctuation, or at a silence of `pauseMs` or more (a [pause] marker
// leaves no punctuation behind). The cut points sit halfway into the
// silences on either side, so the take removed is the sentence's own.
struct SentenceSpan {
	int first = -1, last = -1;   // captions
	qint64 cutFromMs = 0, cutToMs = 0;   // the audio replaced
	qint64 wordsFromMs = 0, wordsToMs = 0; // where its words are in it
	QString text;
	bool valid() const { return first >= 0 && last >= first; }
};

inline SentenceSpan sentenceAround(const QVector<NarrationCaption> &caps, int k, qint64 audioMs, qint64 pauseMs = 500)
{
	SentenceSpan s;
	const int n = int(caps.size());
	if (k < 0 || k >= n)
		return s;
	const auto boundaryAfter = [&](int i) {
		if (i + 1 >= n)
			return true;
		return endsSentence(caps[i].text) || caps[i + 1].firstMs() - caps[i].lastMs() >= pauseMs;
	};
	s.first = k;
	while (s.first > 0 && !boundaryAfter(s.first - 1))
		--s.first;
	s.last = k;
	while (!boundaryAfter(s.last))
		++s.last;
	QStringList t;
	for (int i = s.first; i <= s.last; ++i)
		t << caps[i].text.trimmed();
	s.text = t.join(QLatin1Char(' '));
	s.wordsFromMs = caps[s.first].firstMs();
	s.wordsToMs = caps[s.last].lastMs();
	s.cutFromMs = s.first > 0 ? (caps[s.first - 1].lastMs() + s.wordsFromMs) / 2 : 0;
	s.cutToMs = s.last + 1 < n ? (s.wordsToMs + caps[s.last + 1].firstMs()) / 2 : std::max(audioMs, s.wordsToMs);
	return s;
}

struct Spliced {
	tts_audio::Pcm pcm;
	QVector<ClipWordTime> words; // the new take's words, in the new narration's time
	qint64 deltaMs = 0;          // how much longer (or shorter) it got
	bool ok = false;
};

// The narration with [cutFrom, cutTo) replaced by the new take: its words,
// trimmed to them, with the silences the old sentence had on either side.
inline Spliced spliceSentence(const tts_audio::Pcm &old, const SentenceSpan &span, const tts_audio::Pcm &fresh,
			      const QVector<ClipWordTime> &freshWords)
{
	Spliced out;
	if (!span.valid() || fresh.rate != old.rate || fresh.channels != old.channels || freshWords.isEmpty())
		return out;
	const qint64 lead = std::max<qint64>(0, span.wordsFromMs - span.cutFromMs);
	const qint64 tail = std::max<qint64>(0, span.cutToMs - span.wordsToMs);
	const qint64 takeFrom = std::max<qint64>(0, freshWords.first().startMs - 40);
	const qint64 takeTo = std::min(fresh.durationMs(), freshWords.last().endMs + 80);
	QVector<qint64> offs;
	const tts_audio::Pcm seg = tts_audio::join({tts_audio::silence(lead, old.rate, old.channels),
						    tts_audio::slice(fresh, takeFrom, takeTo),
						    tts_audio::silence(tail, old.rate, old.channels)},
						   0, &offs);
	out.pcm = tts_audio::join({tts_audio::slice(old, 0, span.cutFromMs), seg,
				   tts_audio::slice(old, span.cutToMs, old.durationMs())},
				  0, &offs);
	for (ClipWordTime w : freshWords) {
		w.startMs = w.startMs - takeFrom + span.cutFromMs + lead;
		w.endMs = w.endMs - takeFrom + span.cutFromMs + lead;
		out.words.append(w);
	}
	out.deltaMs = seg.durationMs() - (span.cutToMs - span.cutFromMs);
	out.ok = true;
	return out;
}

} // namespace harpia
