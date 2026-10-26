#pragma once

// From a marked text (TtsMarkup.hpp) to the requests a service is sent.
//
// ttsPlan() cuts the text at its pauses, then -- where a service only takes
// one pace per request (ElevenLabs v2, Deepgram) -- at every change of pace,
// then at sentences into pieces the service accepts. Each piece knows, for
// each word it says, which marks apply to it.
//
// ttsRender() writes one piece the way its service documents:
//   Google       SSML: <prosody> and <emphasis> around the words, a <mark>
//                before each (for the word times)
//   ElevenLabs   v3/v4: its audio tags where a mark starts ([whispers]),
//                stressed words in CAPITALS; v2: the plain words (CAPITALS for
//                stress) at the passage's speed
//   OpenAI       the plain words, and instructions saying what the marks ask
//                ("Say \"vamos lavar\" slowly.")
//   Deepgram     the plain words at the passage's speed
// Every service's word times are found on the plain spoken words
// (timingText), never on the marks.

#include "TtsMarkup.hpp"
#include "TtsText.hpp"

#include <functional>

namespace harpia {

constexpr qint64 kTtsSentenceGapMs = 250; // between two pieces: a sentence's end
constexpr qint64 kTtsPartGapMs = 60;      // between two paces in one sentence

struct TtsPlanned {
	SpokenText st;
	QVector<quint32> fx;          // per spoken word: the marks on it
	QVector<QStringList> before;  // per spoken word: sounds / own tags just before it
	QStringList after;            // ... and after the last word
	qint64 gapBeforeMs = 0;
};

struct TtsRendered {
	QString text;         // what is sent, in the service's own form
	QString timingText;   // the plain spoken words: where the word times are found
	QString instructions; // OpenAI: the style and what the marks ask
	double speed = 1.0;   // this request's pace (ElevenLabs v2, Deepgram)
};

inline bool ttsSpeedPerRequest(TtsProvider p, const QString &model)
{
	return p == TtsProvider::Deepgram || (p == TtsProvider::ElevenLabs && !elevenExpressive(model));
}

inline int ttsPaceOf(quint32 fx)
{
	return (fx & FxSlow) ? -1 : (fx & FxFast) ? 1 : 0;
}

// The marks of `m` on the plain text [a, b), as one piece.
inline TtsPlanned ttsPlanRange(const TtsMarked &m, int a, int b, const QVector<PronunciationRule> &rules)
{
	TtsPlanned pl;
	pl.st = ttsSpoken(m.plain.mid(a, b - a), rules);
	const int ns = pl.st.spokenCount;
	pl.fx.fill(0, ns);
	pl.before.resize(ns);
	const int nw = int(pl.st.written.size());
	for (int i = 0; i < nw; ++i) {
		const int pos = a + pl.st.written[i].pos;
		const quint32 f = pos >= 0 && pos < m.fx.size() ? m.fx[pos] : 0;
		const QPair<int, int> r = pl.st.spokenOf[i];
		for (int k = r.first; k <= r.second && k < ns; ++k)
			pl.fx[k] = f;
	}
	// A sound or own tag belongs to the word after it -- or, at the very end
	// of the text, comes after the last word.
	for (const auto &pt : m.points) {
		int owner = int(pt.first);
		while (owner < m.plain.size() && m.plain[owner].isSpace())
			++owner;
		if (owner >= m.plain.size()) {
			if (!ttsHasLetter(m.plain.mid(b)) && nw > 0)
				pl.after << pt.second;
			continue;
		}
		if (owner < a || owner >= b)
			continue;
		int wi = -1;
		for (int i = 0; i < nw && wi < 0; ++i)
			if (a + pl.st.written[i].pos + pl.st.written[i].len > owner)
				wi = i;
		if (wi >= 0 && pl.st.spokenOf[wi].first < ns)
			pl.before[pl.st.spokenOf[wi].first] << pt.second;
	}
	return pl;
}

// Sentences of the plain text [a, b), packed into pieces whose `cost` stays
// within `limit` (a sentence longer than that is cut between words).
inline QVector<QPair<int, int>> ttsChunkRanges(const QString &plain, int a, int b, int limit,
					       const std::function<int(int, int)> &cost)
{
	const auto trim = [&](QPair<int, int> r) {
		while (r.first < r.second && plain[r.first].isSpace())
			++r.first;
		while (r.second > r.first && plain[r.second - 1].isSpace())
			--r.second;
		return r;
	};
	static const QRegularExpression end(QStringLiteral("[.!?…]+[\"'”’)\\]]*(\\s+|$)"));
	QVector<QPair<int, int>> sentences;
	int from = a;
	auto it = end.globalMatch(plain.mid(a, b - a));
	while (it.hasNext()) {
		const int to = a + int(it.next().capturedEnd());
		sentences.append({from, to});
		from = to;
	}
	if (from < b)
		sentences.append({from, b});
	QVector<QPair<int, int>> out;
	QPair<int, int> cur(-1, -1);
	const auto flush = [&]() {
		if (cur.first < 0)
			return;
		// Too long on its own: between words, as much as fits each time.
		while (cost(cur.first, cur.second) > limit) {
			int cut = -1;
			for (int i = cur.first + 1; i < cur.second; ++i)
				if (plain[i].isSpace()) {
					if (cost(cur.first, i) > limit)
						break;
					cut = i;
				}
			if (cut < 0)
				break; // one word longer than a piece: as it is
			const QPair<int, int> t = trim({cur.first, cut});
			if (t.second > t.first)
				out.append(t);
			cur.first = cut;
		}
		const QPair<int, int> t = trim(cur);
		if (t.second > t.first && ttsHasLetter(plain.mid(t.first, t.second - t.first)))
			out.append(t);
		cur = {-1, -1};
	};
	for (const QPair<int, int> &s : sentences) {
		if (cur.first < 0) {
			cur = s;
			continue;
		}
		if (cost(cur.first, s.second) <= limit) {
			cur.second = s.second;
			continue;
		}
		flush();
		cur = s;
	}
	flush();
	return out;
}

namespace ttsplandetail {
inline QString googleProsody(quint32 f)
{
	// The marks first, then the moods' nearest pace / pitch / volume.
	QString rate, pitch, volume;
	if (f & FxWhisper) {
		rate = QStringLiteral("slow");
		volume = QStringLiteral("x-soft");
	}
	if (f & FxExcited) {
		rate = QStringLiteral("fast");
		pitch = QStringLiteral("+2st");
		volume = QStringLiteral("loud");
	}
	if (f & FxSad) {
		rate = QStringLiteral("slow");
		pitch = QStringLiteral("-2st");
	}
	if (f & FxCalm) {
		rate = QStringLiteral("slow");
		volume = QStringLiteral("soft");
	}
	if (f & FxCheerful)
		pitch = QStringLiteral("+2st");
	if (f & FxSlow)
		rate = QStringLiteral("slow");
	if (f & FxFast)
		rate = QStringLiteral("fast");
	if (f & FxHigh)
		pitch = QStringLiteral("+3st");
	if (f & FxLow)
		pitch = QStringLiteral("-3st");
	if (f & FxLoud)
		volume = QStringLiteral("loud");
	if (f & FxSoft)
		volume = QStringLiteral("soft");
	QStringList a;
	if (!rate.isEmpty())
		a << QStringLiteral("rate=\"%1\"").arg(rate);
	if (!pitch.isEmpty())
		a << QStringLiteral("pitch=\"%1\"").arg(pitch);
	if (!volume.isEmpty())
		a << QStringLiteral("volume=\"%1\"").arg(volume);
	return a.join(QLatin1Char(' '));
}

inline QString elevenTag(const QString &point)
{
	if (const TtsTag *t = ttsTagNamed(point))
		return QString::fromLatin1(t->eleven);
	return QStringLiteral("[%1]").arg(point); // the user's own audio tag
}

inline QString sayList(quint32 f)
{
	QStringList s;
	for (const TtsTag &t : ttsTags())
		if (t.fx && (f & t.fx))
			s << QString::fromUtf8(t.say);
	if (s.size() <= 1)
		return s.join(QString());
	const QString last = s.takeLast();
	return s.join(QStringLiteral(", ")) + QStringLiteral(" and ") + last;
}

inline double paceSpeed(int pace)
{
	return pace < 0 ? 0.85 : pace > 0 ? 1.15 : 1.0;
}
} // namespace ttsplandetail

inline TtsRendered ttsRender(TtsProvider p, const QString &model, const TtsPlanned &pl, const QString &baseStyle = QString())
{
	using namespace ttsplandetail;
	TtsRendered r;
	const QVector<TtsWord> words = ttsWords(pl.st.spoken);
	const int n = int(words.size());
	const auto fxAt = [&](int i) { return i < pl.fx.size() ? pl.fx[i] : 0u; };
	const auto beforeAt = [&](int i) { return i < pl.before.size() ? pl.before[i] : QStringList(); };
	r.timingText = pl.st.spoken;
	switch (p) {
	case TtsProvider::Google: {
		constexpr quint32 kGoogle = FxSlow | FxFast | FxHigh | FxLow | FxLoud | FxSoft | FxStress | FxWhisper |
					    FxExcited | FxSad | FxCalm | FxCheerful;
		QString s = QStringLiteral("<speak>");
		quint32 cur = 0;
		bool prosody = false, emphasis = false;
		const auto close = [&]() {
			if (emphasis)
				s += QStringLiteral("</emphasis>");
			if (prosody)
				s += QStringLiteral("</prosody>");
			emphasis = prosody = false;
		};
		for (int i = 0; i < n; ++i) {
			const quint32 f = fxAt(i) & kGoogle;
			if (f != cur) {
				close();
				const QString attrs = googleProsody(f);
				if (!attrs.isEmpty()) {
					s += QStringLiteral("<prosody %1>").arg(attrs);
					prosody = true;
				}
				if (f & FxStress) {
					s += QStringLiteral("<emphasis level=\"strong\">");
					emphasis = true;
				}
				cur = f;
			}
			s += QStringLiteral("<mark name=\"w%1\"/>").arg(i) + words[i].text.toHtmlEscaped() + QLatin1Char(' ');
		}
		close();
		r.text = s + QStringLiteral("</speak>");
		break;
	}
	case TtsProvider::ElevenLabs: {
		QStringList out;
		if (elevenExpressive(model)) {
			quint32 cur = 0;
			for (int i = 0; i < n; ++i) {
				const quint32 f = fxAt(i);
				for (const QString &t : beforeAt(i))
					out << elevenTag(t);
				for (const TtsTag &t : ttsTags())
					if (t.fx && t.fx != FxStress && (f & t.fx) && !(cur & t.fx))
						out << QString::fromLatin1(t.eleven);
				cur = f;
				out << ((f & FxStress) ? words[i].text.toUpper() : words[i].text);
			}
			for (const QString &t : pl.after)
				out << elevenTag(t);
		} else {
			for (int i = 0; i < n; ++i)
				out << ((fxAt(i) & FxStress) ? words[i].text.toUpper() : words[i].text);
			r.speed = paceSpeed(ttsPaceOf(fxAt(0)));
		}
		r.text = out.join(QLatin1Char(' '));
		break;
	}
	case TtsProvider::OpenAI: {
		r.text = pl.st.spoken;
		QStringList ins;
		if (!baseStyle.trimmed().isEmpty()) {
			QString b = baseStyle.trimmed();
			if (!QStringLiteral(".!?").contains(b.back()))
				b += QLatin1Char('.');
			ins << b;
		}
		for (int i = 0; i < n;) {
			const quint32 f = fxAt(i);
			int j = i;
			while (j + 1 < n && fxAt(j + 1) == f)
				++j;
			if (f) {
				if (i == 0 && j == n - 1) {
					ins << QStringLiteral("Speak %1.").arg(sayList(f));
				} else {
					QStringList w;
					for (int k = i; k <= j; ++k)
						w << words[k].text;
					ins << QStringLiteral("Say “%1” %2.").arg(w.join(QLatin1Char(' ')), sayList(f));
				}
			}
			i = j + 1;
		}
		for (int i = 0; i < n; ++i)
			for (const QString &t : beforeAt(i))
				if (const TtsTag *tag = ttsTagNamed(t))
					ins << QStringLiteral("Give %1 just before “%2”.").arg(QString::fromUtf8(tag->say), words[i].text);
		for (const QString &t : pl.after)
			if (const TtsTag *tag = ttsTagNamed(t))
				ins << QStringLiteral("End with %1.").arg(QString::fromUtf8(tag->say));
		r.instructions = ins.join(QLatin1Char(' '));
		break;
	}
	case TtsProvider::Deepgram:
		r.text = pl.st.spoken;
		r.speed = paceSpeed(ttsPaceOf(fxAt(0)));
		break;
	}
	return r;
}

// The whole text, as the requests `p` (and ElevenLabs `model`) is sent, each
// piece at most `limit` (characters; Google: SSML bytes).
inline QVector<TtsPlanned> ttsPlan(const QString &text, TtsProvider p, const QString &model,
				   const QVector<PronunciationRule> &rules, int limit, const QString &baseStyle = QString())
{
	const TtsMarked m = ttsMark(text);
	const bool paceSplit = ttsSpeedPerRequest(p, model);
	const auto cost = [&](int a, int b) {
		const TtsRendered r = ttsRender(p, model, ttsPlanRange(m, a, b, rules), baseStyle);
		return p == TtsProvider::Google ? int(r.text.toUtf8().size()) : int(r.text.size());
	};
	QVector<TtsPlanned> out;
	for (const TtsSegment &seg : ttsSegments(m)) {
		// One pace per part where the pace is per request.
		QVector<QPair<int, int>> parts;
		if (paceSplit) {
			int from = seg.from;
			int pace = ttsPaceOf(m.fx.value(seg.from));
			for (const TtsWord &w : ttsWords(m.plain.mid(seg.from, seg.to - seg.from))) {
				const int at = seg.from + w.pos;
				const int wp = ttsPaceOf(m.fx.value(at));
				if (wp != pace) {
					parts.append({from, at});
					from = at;
					pace = wp;
				}
			}
			parts.append({from, seg.to});
		} else {
			parts.append({seg.from, seg.to});
		}
		bool firstInSegment = true;
		for (int pi = 0; pi < parts.size(); ++pi) {
			bool firstInPart = true;
			for (const QPair<int, int> &r : ttsChunkRanges(m.plain, parts[pi].first, parts[pi].second, limit, cost)) {
				TtsPlanned t = ttsPlanRange(m, r.first, r.second, rules);
				if (t.st.spokenCount == 0)
					continue;
				t.gapBeforeMs = firstInSegment ? seg.pauseBeforeMs
						: firstInPart  ? kTtsPartGapMs
							       : kTtsSentenceGapMs;
				firstInSegment = firstInPart = false;
				out.append(t);
			}
		}
	}
	return out;
}

} // namespace harpia
