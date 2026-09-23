#pragma once

// From a transcription to caption clips on the timeline.
//
// Three pure steps, each checked on its own:
//   1. parseOpenAiVerboseJson: the API's verbose_json (words with start/end in
//      seconds; segments when words are absent) -> a Transcript in ms.
//   2. groupWords: words -> caption pieces, by the user's rules: at most N
//      words, at most D ms, break on a pause longer than P ms, at most C
//      characters. Every rule is a ceiling; a piece ends when any is reached.
//   3. buildSubtitleClips: pieces -> TlClips, ALIGNED to the media clip the
//      audio came from: a word said at source time s appears on the timeline
//      at clip.outStartMs + (s - clip.srcStartMs) / clip.speed. Clipped to the
//      media clip's span, so a word outside the trimmed range makes no caption.
//
// Nothing here talks to the network or the model; the transcriber and the
// dialog do, and hand their results through these.

#include "../component/Component.hpp"      // ClipWordTime
#include "../component/TextSubtitle.hpp"   // SubtitleMode
#include "../timeline/TimelineModel.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace harpia {

struct Transcript {
	QVector<ClipWordTime> words; // in SOURCE time, ms, ascending
	QString language;            // as detected or requested, e.g. "pt"
	QString text;                // the whole thing, for the log
};

// One caption's worth of words.
struct CaptionPiece {
	QVector<ClipWordTime> words;
	qint64 startMs() const { return words.isEmpty() ? 0 : words.first().startMs; }
	qint64 endMs() const { return words.isEmpty() ? 0 : words.last().endMs; }
	QString text() const
	{
		QString t;
		for (const ClipWordTime &w : words) {
			if (!t.isEmpty())
				t += QLatin1Char(' ');
			t += w.text;
		}
		return t;
	}
};

struct GroupRule {
	int maxWords = 3;          // 1 = one word per caption
	qint64 maxDurationMs = 2500;
	qint64 pauseMs = 400;      // a silence at least this long ends the piece
	int maxChars = 32;         // characters of text, spaces included
};

inline Transcript parseOpenAiVerboseJson(const QByteArray &json, QString *err = nullptr)
{
	Transcript t;
	QJsonParseError pe;
	const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
	if (doc.isNull() || !doc.isObject()) {
		if (err)
			*err = QStringLiteral("not JSON: %1").arg(pe.errorString());
		return t;
	}
	const QJsonObject o = doc.object();
	if (o.contains(QStringLiteral("error"))) {
		if (err)
			*err = o.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
		return t;
	}
	t.language = o.value(QStringLiteral("language")).toString();
	t.text = o.value(QStringLiteral("text")).toString().trimmed();
	const auto ms = [](const QJsonValue &v) { return qint64(std::llround(v.toDouble() * 1000.0)); };
	for (const QJsonValue &wv : o.value(QStringLiteral("words")).toArray()) {
		const QJsonObject wo = wv.toObject();
		ClipWordTime w;
		w.text = wo.value(QStringLiteral("word")).toString().trimmed();
		w.startMs = ms(wo.value(QStringLiteral("start")));
		w.endMs = std::max(w.startMs, ms(wo.value(QStringLiteral("end"))));
		if (!w.text.isEmpty())
			t.words.append(w);
	}
	if (t.words.isEmpty()) {
		// No word timestamps: spread each segment's words evenly over it. Worse
		// than real timings, far better than nothing.
		for (const QJsonValue &sv : o.value(QStringLiteral("segments")).toArray()) {
			const QJsonObject so = sv.toObject();
			const QStringList words =
				so.value(QStringLiteral("text")).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
			if (words.isEmpty())
				continue;
			const qint64 s = ms(so.value(QStringLiteral("start")));
			const qint64 e = std::max(s + 1, ms(so.value(QStringLiteral("end"))));
			const double step = double(e - s) / words.size();
			for (int i = 0; i < words.size(); ++i) {
				ClipWordTime w;
				w.text = words[i];
				w.startMs = s + qint64(std::llround(i * step));
				w.endMs = s + qint64(std::llround((i + 1) * step));
				t.words.append(w);
			}
		}
	}
	std::stable_sort(t.words.begin(), t.words.end(),
			 [](const ClipWordTime &a, const ClipWordTime &b) { return a.startMs < b.startMs; });
	if (t.words.isEmpty() && err && err->isEmpty())
		*err = QStringLiteral("the transcription came back without any words");
	return t;
}

inline QVector<CaptionPiece> groupWords(const QVector<ClipWordTime> &words, const GroupRule &rule)
{
	QVector<CaptionPiece> out;
	CaptionPiece cur;
	const int maxWords = std::max(1, rule.maxWords);
	const int maxChars = std::max(1, rule.maxChars);
	for (const ClipWordTime &w : words) {
		if (!cur.words.isEmpty()) {
			const bool tooMany = cur.words.size() >= maxWords;
			const bool tooLong = rule.maxDurationMs > 0 && (w.endMs - cur.startMs()) > rule.maxDurationMs;
			const bool pause = rule.pauseMs > 0 && (w.startMs - cur.endMs()) >= rule.pauseMs;
			const bool tooWide = (cur.text().size() + 1 + w.text.size()) > maxChars;
			if (tooMany || tooLong || pause || tooWide) {
				out.append(cur);
				cur = CaptionPiece();
			}
		}
		cur.words.append(w);
	}
	if (!cur.words.isEmpty())
		out.append(cur);
	return out;
}

// Where the captions sit and how they look. Position is a preset plus an
// optional nudge, because "bottom" is what nearly everyone wants and a slider
// from 0 to 1 is the wrong first question.
struct SubtitleLook {
	enum class Position { Bottom = 0, Middle = 1, Top = 2 };
	Position position = Position::Bottom;
	double posX = 0.5;          // 0..1 across the canvas
	double marginY = 0.12;      // how far in from the edge, as a fraction of height
	TlText style;               // font, colour, outline, box... as the Inspector shows
	SubtitleMode mode = SubtitleMode::Whole;
	// Gap closing: a caption is held until the next one starts when the gap is
	// shorter than this, so speech does not flicker text on and off between
	// phrases. 0 = every caption ends when its last word does.
	qint64 holdGapMs = 700;
	qint64 minDurationMs = 600; // a single short word still gets a readable moment
};

inline double subtitlePosY(const SubtitleLook &look)
{
	switch (look.position) {
	case SubtitleLook::Position::Top: return look.marginY;
	case SubtitleLook::Position::Middle: return 0.5;
	case SubtitleLook::Position::Bottom: break;
	}
	return 1.0 - look.marginY;
}

// Caption clips for `pieces` (in the SOURCE time of `media`), aligned to where
// `media` plays on the timeline. `media` is the clip the audio came from.
inline QVector<TlClip> buildSubtitleClips(const QVector<CaptionPiece> &pieces, const TlClip &media,
					  const SubtitleLook &look)
{
	QVector<TlClip> out;
	const double speed = media.speed > 0.01 ? media.speed : 1.0;
	const qint64 outStart = media.outStartMs;
	const qint64 outEnd = media.outEndMs();
	const auto toOut = [&](qint64 srcMs) {
		return outStart + qint64(std::llround(double(srcMs - media.srcStartMs) / speed));
	};
	for (int i = 0; i < pieces.size(); ++i) {
		const CaptionPiece &p = pieces[i];
		if (p.words.isEmpty())
			continue;
		qint64 s = toOut(p.startMs());
		qint64 e = toOut(p.endMs());
		// Hold to the next caption across a short gap; stretch a blink of a
		// caption to something readable.
		if (i + 1 < pieces.size()) {
			const qint64 next = toOut(pieces[i + 1].startMs());
			if (look.holdGapMs > 0 && next - e < look.holdGapMs)
				e = next;
		}
		if (e - s < look.minDurationMs)
			e = s + look.minDurationMs;
		if (i + 1 < pieces.size())
			e = std::min(e, toOut(pieces[i + 1].startMs())); // never over the next one
		// Inside the media clip's own span only.
		s = std::max(s, outStart);
		e = std::min(e, outEnd);
		if (e <= s)
			continue;

		TlClip c;
		c.type = TlClip::Type::Text;
		c.srcStartMs = 0;
		c.srcEndMs = e - s;
		c.outStartMs = s;
		c.text = look.style;
		c.text.text = p.text();
		c.posX = look.posX;
		c.posY = subtitlePosY(look);
		// Word times relative to THIS clip, so they travel with it when moved.
		for (const ClipWordTime &w : p.words) {
			ClipWordTime cw;
			cw.text = w.text;
			cw.startMs = std::max<qint64>(0, toOut(w.startMs) - s);
			cw.endMs = std::max(cw.startMs, toOut(w.endMs) - s);
			c.words.append(cw);
		}
		if (look.mode != SubtitleMode::Whole) {
			ComponentInstance ci;
			ci.typeId = QStringLiteral("harpia.subtitle");
			ci.instanceId = QStringLiteral("sub");
			ci.props.insert(QStringLiteral("mode"), double(int(look.mode)));
			c.components.append(ci);
		}
		out.append(c);
	}
	return out;
}

} // namespace harpia
