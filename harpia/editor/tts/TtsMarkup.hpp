#pragma once

// Delivery marks in a text-to-speech text: one set of tags, the same for every
// service, typed or put in by the toolbar over the text box --
//
//   [pause 1s]  [pause 500ms]                 silence (also a blank line)
//   [slow]…[/slow]  [fast]…[/fast]            pace
//   [high]…[/high]  [low]…[/low]              pitch
//   [loud]…[/loud]  [soft]…[/soft]            volume
//   [stress]…[/stress]                        emphasis
//   [whisper] [excited] [sad] [calm] [cheerful] … with their [/…]   mood
//   [laugh]  [sigh]                           a sound, where it stands
//   [anything else in words]                  ElevenLabs v3/v4 only: its own
//                                             audio tags ("[strong accent]")
//
// -- and each service gets them in its own form, because each documents a
// different way (TtsPlan.hpp renders them):
//
//   Google (SSML voices)  <prosody rate/pitch/volume>, <emphasis>; moods as
//                         the nearest prosody; no sounds
//   ElevenLabs v3/v4      its audio tags ([whispers], [laughs], …); stress
//                         in CAPITALS (as its guide says)
//   ElevenLabs v2         speed per passage (0.7-1.2); stress in CAPITALS;
//                         no pitch, volume, moods or sounds
//   OpenAI                no markup at all: its instructions say it ("Say
//                         \"vamos lavar\" slowly.")
//   Deepgram Aura-2       speed per passage; nothing else
//
// Pauses are Harpia's own everywhere: silence between requests, so every
// word keeps an exact time for its caption. No tag is ever said or shown.

#include "TtsProviders.hpp"

#include <QHash>
#include <QRegularExpression>
#include <cmath>
#include <QStringList>
#include <QVector>

namespace harpia {

// The silence a blank line makes.
constexpr qint64 kBlankLinePauseMs = 700;

// What can be asked of a passage: bits, so several can apply at once.
enum TtsFx : quint32 {
	FxSlow = 1u << 0,
	FxFast = 1u << 1,
	FxHigh = 1u << 2,
	FxLow = 1u << 3,
	FxLoud = 1u << 4,
	FxSoft = 1u << 5,
	FxStress = 1u << 6,
	FxWhisper = 1u << 7,
	FxExcited = 1u << 8,
	FxSad = 1u << 9,
	FxCalm = 1u << 10,
	FxCheerful = 1u << 11,
};
enum class TtsSound { Laugh, Sigh };

struct TtsTag {
	quint32 fx = 0;     // a passage effect, or
	bool sound = false; // a sound at one point
	TtsSound which = TtsSound::Laugh;
	const char *tag = "";      // as typed: "slow"
	const char *label = "";    // the toolbar: "Slower"
	const char *say = "";      // OpenAI's instructions: "slowly"
	const char *eleven = "";   // ElevenLabs v3/v4 audio tag
};

inline const QVector<TtsTag> &ttsTags()
{
	static const QVector<TtsTag> all = {
		{FxSlow, false, TtsSound::Laugh, "slow", "Slower", "slowly", "[slowly]"},
		{FxFast, false, TtsSound::Laugh, "fast", "Faster", "quickly", "[quickly]"},
		{FxHigh, false, TtsSound::Laugh, "high", "Higher", "with a higher pitch", "[high-pitched]"},
		{FxLow, false, TtsSound::Laugh, "low", "Lower", "with a lower, deeper pitch", "[low voice]"},
		{FxLoud, false, TtsSound::Laugh, "loud", "Louder", "loudly, projecting the voice", "[loudly]"},
		{FxSoft, false, TtsSound::Laugh, "soft", "Softer", "softly", "[softly]"},
		{FxStress, false, TtsSound::Laugh, "stress", "Stress", "with strong emphasis", ""},
		{FxWhisper, false, TtsSound::Laugh, "whisper", "Whisper", "in a whisper", "[whispers]"},
		{FxExcited, false, TtsSound::Laugh, "excited", "Excited", "excitedly, full of energy", "[excited]"},
		{FxSad, false, TtsSound::Laugh, "sad", "Sad", "sadly", "[sad]"},
		{FxCalm, false, TtsSound::Laugh, "calm", "Calm", "calmly, relaxed", "[calm]"},
		{FxCheerful, false, TtsSound::Laugh, "cheerful", "Cheerful", "cheerfully", "[cheerfully]"},
		{0, true, TtsSound::Laugh, "laugh", "Laugh", "a short laugh", "[laughs]"},
		{0, true, TtsSound::Sigh, "sigh", "Sigh", "a sigh", "[sighs]"},
	};
	return all;
}

// Other names people type for the same thing.
inline QString ttsTagAlias(const QString &name)
{
	static const QHash<QString, QString> a = {
		{QStringLiteral("emphasis"), QStringLiteral("stress")},  {QStringLiteral("whispers"), QStringLiteral("whisper")},
		{QStringLiteral("laughs"), QStringLiteral("laugh")},     {QStringLiteral("sighs"), QStringLiteral("sigh")},
		{QStringLiteral("slower"), QStringLiteral("slow")},      {QStringLiteral("faster"), QStringLiteral("fast")},
		{QStringLiteral("louder"), QStringLiteral("loud")},      {QStringLiteral("softer"), QStringLiteral("soft")},
		{QStringLiteral("higher"), QStringLiteral("high")},      {QStringLiteral("lower"), QStringLiteral("low")},
	};
	return a.value(name, name);
}

inline const TtsTag *ttsTagNamed(const QString &name)
{
	const QString n = ttsTagAlias(name);
	for (const TtsTag &t : ttsTags())
		if (n == QLatin1String(t.tag))
			return &t;
	return nullptr;
}

inline const TtsTag *ttsTagForFx(quint32 fx)
{
	for (const TtsTag &t : ttsTags())
		if (t.fx == fx)
			return &t;
	return nullptr;
}

// ---- what each service can do with each mark ---------------------------------

enum class TtsSupport { Native, Approx, None };

struct TtsSupportInfo {
	TtsSupport level = TtsSupport::None;
	QString how; // for the toolbar's tooltip
};

inline bool elevenExpressive(const QString &model)
{
	return model.startsWith(QLatin1String("eleven_v3")) || model.startsWith(QLatin1String("eleven_v4"));
}

// How `p` (with ElevenLabs `model`) does `tag`.
inline TtsSupportInfo ttsSupport(TtsProvider p, const QString &model, const TtsTag &tag)
{
	const quint32 fx = tag.fx;
	const auto native = [](const QString &h) { return TtsSupportInfo{TtsSupport::Native, h}; };
	const auto approx = [](const QString &h) { return TtsSupportInfo{TtsSupport::Approx, h}; };
	const auto none = [](const QString &h) { return TtsSupportInfo{TtsSupport::None, h}; };
	const QString say = QString::fromUtf8(tag.say);
	switch (p) {
	case TtsProvider::OpenAI:
		return approx(QStringLiteral("OpenAI reads no marks: Harpia adds to its instructions, e.g. "
					     "“Say ‘…’ %1.” It usually follows them, not always exactly.")
				      .arg(tag.sound ? QStringLiteral("after %1").arg(say) : say));
	case TtsProvider::ElevenLabs:
		if (elevenExpressive(model)) {
			if (fx == FxStress)
				return native(QStringLiteral("ElevenLabs v3/v4: the words in CAPITALS, as its guide says."));
			return native(QStringLiteral("ElevenLabs v3/v4 audio tag %1. Some tags vary from voice to voice.")
					      .arg(QString::fromLatin1(tag.eleven)));
		}
		if (fx == FxSlow || fx == FxFast)
			return approx(QStringLiteral("ElevenLabs v2: the passage is asked for on its own at %1 speed "
						     "(v2 has no marks; 0.7 to 1.2).")
					      .arg(fx == FxSlow ? QStringLiteral("0.85") : QStringLiteral("1.15")));
		if (fx == FxStress)
			return approx(QStringLiteral("ElevenLabs v2: the words in CAPITALS (works best on v3/v4)."));
		return none(QStringLiteral("ElevenLabs Multilingual v2 cannot do this. Choose the v4 model above."));
	case TtsProvider::Google:
		if (tag.sound)
			return none(QStringLiteral("Google voices make no sounds like laughs or sighs."));
		if (fx & (FxSlow | FxFast | FxHigh | FxLow | FxLoud | FxSoft))
			return native(QStringLiteral("Google SSML <prosody %1>.")
					      .arg(fx == FxSlow   ? QStringLiteral("rate=\"slow\"")
						   : fx == FxFast ? QStringLiteral("rate=\"fast\"")
						   : fx == FxHigh ? QStringLiteral("pitch=\"+3st\"")
						   : fx == FxLow  ? QStringLiteral("pitch=\"-3st\"")
						   : fx == FxLoud ? QStringLiteral("volume=\"loud\"")
								  : QStringLiteral("volume=\"soft\"")));
		if (fx == FxStress)
			return native(QStringLiteral("Google SSML <emphasis level=\"strong\">."));
		return approx(QStringLiteral("Google has no moods for these voices: the nearest pace, pitch and volume "
					     "(%1).")
				      .arg(fx == FxWhisper   ? QStringLiteral("very soft and slow")
					   : fx == FxExcited ? QStringLiteral("faster, higher, louder")
					   : fx == FxSad     ? QStringLiteral("slower and lower")
					   : fx == FxCalm    ? QStringLiteral("slower and softer")
							     : QStringLiteral("higher")));
	case TtsProvider::Deepgram:
		if (fx == FxSlow || fx == FxFast)
			return approx(QStringLiteral("Deepgram: the passage is asked for on its own at %1 speed "
						     "(not every language follows it).")
					      .arg(fx == FxSlow ? QStringLiteral("0.85") : QStringLiteral("1.15")));
		return none(QStringLiteral("Deepgram Aura-2 takes no marks besides pace; this is read normally."));
	}
	return none(QString());
}

// ---- reading the marks ----------------------------------------------------------

// The text with its marks taken out: what is said and shown, and where each
// mark applies in it.
struct TtsMarked {
	QString plain;                         // no marks
	QVector<quint32> fx;                   // per character of `plain`
	QVector<QPair<int, qint64>> pauses;    // (position in plain, ms)
	QVector<QPair<int, QString>> points;   // (position, tag): sounds ("laugh") and own tags
	QStringList problems;                  // "[/slow] without [slow]", ...
};

inline bool ttsIsOwnTag(const QString &inside)
{
	// ElevenLabs-style free tags: words only ("strong French accent").
	static const QRegularExpression words(QStringLiteral("^[\\p{L} '\\-]{2,40}$"));
	return words.match(inside).hasMatch();
}

inline TtsMarked ttsMark(const QString &text)
{
	static const QRegularExpression token(QStringLiteral("\\[([^\\[\\]\\n]{1,48})\\]|\\n[ \\t]*\\n\\s*"));
	static const QRegularExpression pause(
		QStringLiteral("^(?:pause|pausa)\\s*(?:(\\d+(?:[.,]\\d+)?)\\s*(ms|s|seg|sec)?)?$"),
		QRegularExpression::CaseInsensitiveOption);
	TtsMarked m;
	quint32 open = 0;
	qsizetype from = 0;
	const auto add = [&](const QString &s) {
		m.plain += s;
		for (qsizetype i = 0; i < s.size(); ++i)
			m.fx.append(open);
	};
	auto it = token.globalMatch(text);
	while (it.hasNext()) {
		const QRegularExpressionMatch t = it.next();
		add(text.mid(from, t.capturedStart() - from));
		from = t.capturedEnd();
		if (!t.captured(0).startsWith(QLatin1Char('['))) { // a blank line
			m.pauses.append({int(m.plain.size()), kBlankLinePauseMs});
			add(QStringLiteral(" "));
			continue;
		}
		const QString inside = t.captured(1).trimmed();
		const QString low = inside.toLower();
		if (const QRegularExpressionMatch p = pause.match(inside); p.hasMatch()) {
			const QString num = p.captured(1);
			const QString unit = p.captured(2).toLower();
			const double v = num.isEmpty() ? 1.0 : QString(num).replace(QLatin1Char(','), QLatin1Char('.')).toDouble();
			m.pauses.append({int(m.plain.size()), unit == QLatin1String("ms") ? qint64(std::llround(v))
										 : qint64(std::llround(v * 1000.0))});
			continue;
		}
		const bool closing = low.startsWith(QLatin1Char('/'));
		const TtsTag *tag = ttsTagNamed(closing ? low.mid(1).trimmed() : low);
		if (tag && tag->sound) {
			if (!closing)
				m.points.append({int(m.plain.size()), QString::fromLatin1(tag->tag)});
			continue;
		}
		if (tag) {
			if (closing) {
				if (!(open & tag->fx))
					m.problems << QStringLiteral("[/%1] without [%1]").arg(QString::fromLatin1(tag->tag));
				open &= ~tag->fx;
			} else {
				open |= tag->fx;
			}
			continue;
		}
		if (!closing && ttsIsOwnTag(inside)) {
			m.points.append({int(m.plain.size()), inside});
			continue;
		}
		add(t.captured(0)); // brackets that are not a mark: text
	}
	add(text.mid(from));
	for (const TtsTag &t : ttsTags())
		if (t.fx && (open & t.fx))
			m.problems << QStringLiteral("[%1] is not closed: it lasts to the end").arg(QString::fromLatin1(t.tag));
	return m;
}

// Every mark this text uses: effect bits, and the sound/own tags.
inline quint32 ttsFxUsed(const TtsMarked &m)
{
	quint32 all = 0;
	for (quint32 f : m.fx)
		all |= f;
	return all;
}

// The marks this service cannot do at all, as their toolbar names: what the
// window warns about before generating.
inline QStringList ttsUnsupported(const TtsMarked &m, TtsProvider p, const QString &model)
{
	QStringList out;
	const quint32 used = ttsFxUsed(m);
	for (const TtsTag &t : ttsTags()) {
		bool present = t.fx ? (used & t.fx) != 0 : false;
		if (t.sound)
			for (const auto &pt : m.points)
				present = present || pt.second == QLatin1String(t.tag);
		if (present && ttsSupport(p, model, t).level == TtsSupport::None)
			out << QString::fromUtf8(t.label);
	}
	bool own = false;
	for (const auto &pt : m.points)
		own = own || !ttsTagNamed(pt.second);
	if (own && !(p == TtsProvider::ElevenLabs && elevenExpressive(model)))
		out << QStringLiteral("your own [tags] (ElevenLabs v3/v4 only)");
	return out;
}

} // namespace harpia
