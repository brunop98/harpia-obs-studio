#pragma once

// Text to speech: the services the Text to speech window can use, and the pure
// half of talking to each -- the request, the reply, and where every word of
// the text is spoken in the audio that comes back. TtsSynthesizer moves the
// bytes; nothing here touches the network, so every format is checkable in a
// test without a server.
//
//   OpenAI      JSON in, WAV out               (Bearer key)  words: transcribed
//   ElevenLabs  JSON in, JSON out: MP3 + the   (xi-api-key)  words: from the
//               time of every character                      characters
//   Google      SSML in (a <mark> before each  (x-goog-api-  words: from the
//               word), JSON out: WAV + marks    key)          marks
//   Deepgram    JSON in, WAV out               ("Token" key) words: transcribed
//
// "Transcribed": the service gives audio only, so the audio is sent back
// through speech to text (the same service, the same key) and the words heard
// are matched to the words written (alignToTranscript), so a caption shows
// the text as it was typed -- spelling, punctuation and all -- at the time it
// is said.
//
// A long text is cut at sentences into pieces each service accepts
// (ttsChunks); the pieces' audio is joined afterwards and their word times
// shifted to match.

#include "../subtitles/Transcript.hpp" // ClipWordTime

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <functional>

namespace harpia {

enum class TtsProvider { OpenAI = 0, ElevenLabs, Google, Deepgram };

struct TtsVoice {
	QString id;   // what is sent ("marin", an ElevenLabs voice id, "pt-BR-Neural2-A")
	QString name; // what the picker shows
};

struct TtsProviderInfo {
	TtsProvider provider = TtsProvider::OpenAI;
	const char *id = "";           // settings: "openai", "elevenlabs", ...
	const char *name = "";         // what the picker shows
	const char *model = "";        // sent to the service, where it takes one
	const char *keyUrl = "";       // where to get a key
	const char *keySteps = "";     // the clicks on that page
	const char *keyPlaceholder = "";
	const char *about = "";        // one line under the picker
	int maxChars = 4000;           // per request; longer text is cut at sentences
	bool nativeTimings = false;    // word times come with the audio
	bool listsVoices = false;      // "Load my voices" asks the service
	bool hasSpeed = false;         // a speaking-rate setting
	bool hasStyle = false;         // a free-text delivery instruction
};

inline const QVector<TtsProviderInfo> &ttsProviders()
{
	static const QVector<TtsProviderInfo> all = {
		{TtsProvider::OpenAI, "openai", "OpenAI", "gpt-4o-mini-tts", "https://platform.openai.com/api-keys",
		 "OpenAI dashboard ▸ API keys ▸ Create new secret key", "sk-…  (paste your OpenAI API key)",
		 "Natural voices; describe the delivery in Style (\"calm and warm\"). Word times come from "
		 "transcribing the result with Whisper, on the same key.",
		 4000, false, false, false, true},
		{TtsProvider::ElevenLabs, "elevenlabs", "ElevenLabs", "eleven_multilingual_v2",
		 "https://elevenlabs.io/app/settings/api-keys", "ElevenLabs ▸ Settings ▸ API Keys ▸ Create API Key",
		 "sk_…  (paste your ElevenLabs API key)",
		 "The most lifelike voices, many languages; exact word times come with the audio. The key "
		 "needs the Text to Speech permission. Load my voices lists your account's voices.",
		 4500, true, true, true, false},
		{TtsProvider::Google, "google", "Google Cloud", "", "https://console.cloud.google.com/apis/credentials",
		 "Google Cloud console ▸ enable the Text-to-Speech API ▸ Credentials ▸ Create credentials ▸ API key",
		 "AIza…  (paste your Google Cloud API key)",
		 "Hundreds of voices in many languages, including Brazilian Portuguese; exact word times. "
		 "Neural2 and WaveNet voices (Chirp voices cannot mark words).",
		 4800, true, true, true, false},
		{TtsProvider::Deepgram, "deepgram", "Deepgram (Aura-2)", "", "https://console.deepgram.com/",
		 "Deepgram console ▸ API Keys ▸ Create a New API Key", "paste your Deepgram API key",
		 "Fast, clear voices in English and Spanish (and a few more). Word times come from "
		 "transcribing the result with Nova-3, on the same key.",
		 1900, false, false, false, false},
	};
	return all;
}

// The models a service offers, as (id, what the picker says); empty when it
// has one. ElevenLabs: Multilingual v2 takes no marks but pace; v3/v4 take its
// audio tags (moods, whispers, laughs).
inline QVector<QPair<QString, QString>> ttsModels(TtsProvider p)
{
	if (p != TtsProvider::ElevenLabs)
		return {};
	return {{QStringLiteral("eleven_multilingual_v2"), QStringLiteral("Multilingual v2: steady, long texts")},
		{QStringLiteral("eleven_v4"), QStringLiteral("v4: expressive (moods, whispers, laughs)")},
		{QStringLiteral("eleven_v3"), QStringLiteral("v3: expressive (the previous generation)")}};
}

inline const TtsProviderInfo &ttsProviderInfo(TtsProvider p)
{
	for (const TtsProviderInfo &i : ttsProviders())
		if (i.provider == p)
			return i;
	return ttsProviders().first();
}

inline TtsProvider ttsProviderFromId(const QString &id)
{
	for (const TtsProviderInfo &i : ttsProviders())
		if (id == QLatin1String(i.id))
			return i.provider;
	return TtsProvider::OpenAI;
}

inline QString ttsProviderName(TtsProvider p)
{
	return QString::fromUtf8(ttsProviderInfo(p).name);
}

// The languages offered, as (label, ISO-639-1). Google needs it to pick a
// voice; the transcribing services use it as a hint.
inline QVector<QPair<QString, QString>> ttsLanguages()
{
	return {{QStringLiteral("Portuguese (Brazil)"), QStringLiteral("pt")},
		{QStringLiteral("English"), QStringLiteral("en")},
		{QStringLiteral("Spanish"), QStringLiteral("es")},
		{QStringLiteral("French"), QStringLiteral("fr")},
		{QStringLiteral("German"), QStringLiteral("de")},
		{QStringLiteral("Italian"), QStringLiteral("it")},
		{QStringLiteral("Japanese"), QStringLiteral("ja")},
		{QStringLiteral("Korean"), QStringLiteral("ko")},
		{QStringLiteral("Dutch"), QStringLiteral("nl")},
		{QStringLiteral("Polish"), QStringLiteral("pl")},
		{QStringLiteral("Turkish"), QStringLiteral("tr")},
		{QStringLiteral("Russian"), QStringLiteral("ru")},
		{QStringLiteral("Hindi"), QStringLiteral("hi")}};
}

// Google names its voices by locale ("pt-BR-Neural2-A").
inline QString googleLocale(const QString &language)
{
	static const QHash<QString, QString> m = {
		{QStringLiteral("pt"), QStringLiteral("pt-BR")}, {QStringLiteral("en"), QStringLiteral("en-US")},
		{QStringLiteral("es"), QStringLiteral("es-ES")}, {QStringLiteral("fr"), QStringLiteral("fr-FR")},
		{QStringLiteral("de"), QStringLiteral("de-DE")}, {QStringLiteral("it"), QStringLiteral("it-IT")},
		{QStringLiteral("ja"), QStringLiteral("ja-JP")}, {QStringLiteral("ko"), QStringLiteral("ko-KR")},
		{QStringLiteral("nl"), QStringLiteral("nl-NL")}, {QStringLiteral("pl"), QStringLiteral("pl-PL")},
		{QStringLiteral("tr"), QStringLiteral("tr-TR")}, {QStringLiteral("ru"), QStringLiteral("ru-RU")},
		{QStringLiteral("hi"), QStringLiteral("hi-IN")}};
	return m.value(language, QStringLiteral("en-US"));
}

// The voices offered before (or without) asking the service. ElevenLabs and
// Google can list the account's own with Load my voices; any id can also be
// typed into the box.
inline QVector<TtsVoice> ttsDefaultVoices(TtsProvider p, const QString &language)
{
	const auto v = [](const char *id, const char *name) {
		return TtsVoice{QString::fromLatin1(id), QString::fromUtf8(name)};
	};
	switch (p) {
	case TtsProvider::OpenAI:
		return {v("marin", "Marin (recommended)"), v("cedar", "Cedar (recommended)"), v("coral", "Coral"),
			v("sage", "Sage"), v("ash", "Ash"), v("ballad", "Ballad"), v("verse", "Verse"),
			v("alloy", "Alloy"), v("echo", "Echo"), v("fable", "Fable"), v("nova", "Nova"),
			v("onyx", "Onyx"), v("shimmer", "Shimmer")};
	case TtsProvider::ElevenLabs:
		return {v("21m00Tcm4TlvDq8ikWAM", "Rachel"), v("JBFqnCBsd6RMkjVDRZzb", "George"),
			v("EXAVITQu4vr4xnSDxMaL", "Sarah"), v("nPczCjzI2devNBz1zQrb", "Brian"),
			v("XB0fDUnXU5powFXDhCwa", "Charlotte"), v("onwK4e9ZLuTAKqWW03F9", "Daniel")};
	case TtsProvider::Google: {
		const QString loc = googleLocale(language);
		QVector<TtsVoice> out;
		for (const char *kind : {"Neural2-A", "Neural2-B", "Neural2-C", "Wavenet-A", "Wavenet-B"})
			out.append({loc + QLatin1Char('-') + QLatin1String(kind),
				    loc + QLatin1Char(' ') + QLatin1String(kind)});
		return out;
	}
	case TtsProvider::Deepgram:
		if (language == QLatin1String("es"))
			return {v("aura-2-celeste-es", "Celeste"), v("aura-2-estrella-es", "Estrella"),
				v("aura-2-nestor-es", "Néstor"), v("aura-2-agustina-es", "Agustina")};
		return {v("aura-2-thalia-en", "Thalia"), v("aura-2-andromeda-en", "Andromeda"),
			v("aura-2-helena-en", "Helena"), v("aura-2-apollo-en", "Apollo"),
			v("aura-2-arcas-en", "Arcas"), v("aura-2-zeus-en", "Zeus")};
	}
	return {};
}

// ---- the text ---------------------------------------------------------------

// One word of the text as written, with where it is in that text. A run of
// punctuation on its own ("—") is not a word anyone says: it joins the word
// before it.
struct TtsWord {
	QString text;
	int pos = 0; // in the text
	int len = 0;
};

inline bool ttsHasLetter(const QString &s)
{
	for (const QChar c : s)
		if (c.isLetterOrNumber())
			return true;
	return false;
}

inline QVector<TtsWord> ttsWords(const QString &text)
{
	QVector<TtsWord> out;
	static const QRegularExpression ws(QStringLiteral("\\S+"));
	auto it = ws.globalMatch(text);
	while (it.hasNext()) {
		const QRegularExpressionMatch m = it.next();
		TtsWord w{m.captured(), int(m.capturedStart()), int(m.capturedLength())};
		if (!ttsHasLetter(w.text) && !out.isEmpty()) {
			TtsWord &prev = out.last();
			prev.text += QLatin1Char(' ') + w.text;
			continue;
		}
		out.append(w);
	}
	return out;
}

// The text in pieces of at most `limit` (as `cost` measures a piece; its
// length by default), cut between sentences -- and between words when one
// sentence alone is too long. Blank lines are kept as the end of a sentence.
inline QStringList ttsChunks(const QString &text, int limit,
			     const std::function<int(const QString &)> &cost = nullptr)
{
	const auto size = [&](const QString &s) { return cost ? cost(s) : int(s.size()); };
	// Sentences: up to and including their closing marks and any quotes.
	QStringList sentences;
	static const QRegularExpression end(QStringLiteral("[.!?…]+[\"'”’)\\]]*(\\s+|$)|\\n\\s*\\n"));
	int from = 0;
	auto it = end.globalMatch(text);
	while (it.hasNext()) {
		const QRegularExpressionMatch m = it.next();
		const int to = int(m.capturedEnd());
		sentences << text.mid(from, to - from).trimmed();
		from = to;
	}
	if (from < text.size())
		sentences << text.mid(from).trimmed();
	sentences.removeAll(QString());

	QStringList out;
	QString cur;
	const auto flush = [&]() {
		if (!cur.trimmed().isEmpty())
			out << cur.trimmed();
		cur.clear();
	};
	for (const QString &s : sentences) {
		const QString joined = cur.isEmpty() ? s : cur + QLatin1Char(' ') + s;
		if (size(joined) <= limit) {
			cur = joined;
			continue;
		}
		flush();
		if (size(s) <= limit) {
			cur = s;
			continue;
		}
		// One sentence longer than a whole piece: by words.
		for (const QString &w : s.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
			const QString j = cur.isEmpty() ? w : cur + QLatin1Char(' ') + w;
			if (!cur.isEmpty() && size(j) > limit) {
				flush();
				cur = w;
			} else {
				cur = j;
			}
		}
	}
	flush();
	return out;
}

// Google: the text as SSML with a <mark name="wN"/> before word N, so the
// reply says when each word starts.
inline QString googleSsml(const QString &text)
{
	const QVector<TtsWord> words = ttsWords(text);
	QString s = QStringLiteral("<speak>");
	for (int i = 0; i < words.size(); ++i)
		s += QStringLiteral("<mark name=\"w%1\"/>").arg(i) + words[i].text.toHtmlEscaped() + QLatin1Char(' ');
	s += QStringLiteral("</speak>");
	return s;
}

// The size Google counts: the SSML's bytes.
inline int googleSsmlBytes(const QString &text)
{
	return int(googleSsml(text).toUtf8().size());
}

// ---- requests ---------------------------------------------------------------

struct TtsRequest {
	TtsProvider provider = TtsProvider::OpenAI;
	QString apiKey;
	QString text;       // one piece, in the service's own form (TtsPlan.hpp; Google: SSML)
	QString timingText; // its plain spoken words, where the word times are found ("" = text)
	QString voice;      // TtsVoice::id
	QString model;      // ElevenLabs: the model ("" = the service's default)
	QString language;   // ISO-639-1
	QString style;      // OpenAI: its instructions
	double speed = 1.0; // ElevenLabs, Google: the rate; Deepgram: this passage's
	// ElevenLabs: the text around this piece, so the joins sound continuous.
	QString previousText, nextText;
};

inline QUrl ttsUrl(const TtsRequest &r)
{
	switch (r.provider) {
	case TtsProvider::OpenAI: return QUrl(QStringLiteral("https://api.openai.com/v1/audio/speech"));
	case TtsProvider::ElevenLabs: {
		QUrl u(QStringLiteral("https://api.elevenlabs.io/v1/text-to-speech/%1/with-timestamps")
			       .arg(QString::fromLatin1(QUrl::toPercentEncoding(r.voice.trimmed()))));
		QUrlQuery q;
		q.addQueryItem(QStringLiteral("output_format"), QStringLiteral("mp3_44100_128"));
		u.setQuery(q);
		return u;
	}
	case TtsProvider::Google:
		return QUrl(QStringLiteral("https://texttospeech.googleapis.com/v1beta1/text:synthesize"));
	case TtsProvider::Deepgram: {
		QUrl u(QStringLiteral("https://api.deepgram.com/v1/speak"));
		QUrlQuery q;
		q.addQueryItem(QStringLiteral("model"), r.voice.trimmed());
		q.addQueryItem(QStringLiteral("encoding"), QStringLiteral("linear16"));
		q.addQueryItem(QStringLiteral("container"), QStringLiteral("wav"));
		q.addQueryItem(QStringLiteral("sample_rate"), QStringLiteral("24000"));
		if (std::abs(r.speed - 1.0) > 0.001)
			q.addQueryItem(QStringLiteral("speed"), QString::number(std::clamp(r.speed, 0.7, 1.5), 'f', 2));
		u.setQuery(q);
		return u;
	}
	}
	return QUrl();
}

// The headers each service wants: the key where it expects it, and JSON.
inline QVector<QPair<QByteArray, QByteArray>> ttsHeaders(TtsProvider p, const QString &key)
{
	const QByteArray k = key.trimmed().toUtf8();
	QVector<QPair<QByteArray, QByteArray>> h = {{"Content-Type", "application/json"}};
	switch (p) {
	case TtsProvider::OpenAI: h.append({"Authorization", "Bearer " + k}); break;
	case TtsProvider::ElevenLabs: h.append({"xi-api-key", k}); break;
	case TtsProvider::Google: h.append({"x-goog-api-key", k}); break;
	case TtsProvider::Deepgram: h.append({"Authorization", "Token " + k}); break;
	}
	return h;
}

inline QByteArray ttsBody(const TtsRequest &r)
{
	QJsonObject o;
	switch (r.provider) {
	case TtsProvider::OpenAI:
		o.insert(QStringLiteral("model"), QString::fromLatin1(ttsProviderInfo(r.provider).model));
		o.insert(QStringLiteral("input"), r.text);
		o.insert(QStringLiteral("voice"), r.voice.trimmed());
		o.insert(QStringLiteral("response_format"), QStringLiteral("wav"));
		if (!r.style.trimmed().isEmpty())
			o.insert(QStringLiteral("instructions"), r.style.trimmed());
		break;
	case TtsProvider::ElevenLabs:
		o.insert(QStringLiteral("text"), r.text);
		o.insert(QStringLiteral("model_id"),
			 r.model.isEmpty() ? QString::fromLatin1(ttsProviderInfo(r.provider).model) : r.model);
		if (std::abs(r.speed - 1.0) > 0.001) {
			QJsonObject vs;
			vs.insert(QStringLiteral("speed"), std::clamp(r.speed, 0.7, 1.2));
			o.insert(QStringLiteral("voice_settings"), vs);
		}
		if (!r.previousText.isEmpty())
			o.insert(QStringLiteral("previous_text"), r.previousText.right(1000));
		if (!r.nextText.isEmpty())
			o.insert(QStringLiteral("next_text"), r.nextText.left(1000));
		break;
	case TtsProvider::Google: {
		QJsonObject in;
		// Ready SSML (marks and all) from the planner, or plain text.
		in.insert(QStringLiteral("ssml"), r.text.startsWith(QLatin1String("<speak>")) ? r.text : googleSsml(r.text));
		o.insert(QStringLiteral("input"), in);
		QJsonObject voice;
		const QString name = r.voice.trimmed();
		// "pt-BR-Neural2-A" -> "pt-BR"
		const QString loc = name.section(QLatin1Char('-'), 0, 1);
		voice.insert(QStringLiteral("languageCode"), loc.isEmpty() ? googleLocale(r.language) : loc);
		voice.insert(QStringLiteral("name"), name);
		o.insert(QStringLiteral("voice"), voice);
		QJsonObject ac;
		ac.insert(QStringLiteral("audioEncoding"), QStringLiteral("LINEAR16"));
		ac.insert(QStringLiteral("sampleRateHertz"), 24000);
		ac.insert(QStringLiteral("speakingRate"), std::clamp(r.speed, 0.25, 4.0));
		o.insert(QStringLiteral("audioConfig"), ac);
		o.insert(QStringLiteral("enableTimePointing"), QJsonArray{QStringLiteral("SSML_MARK")});
		break;
	}
	case TtsProvider::Deepgram:
		o.insert(QStringLiteral("text"), r.text);
		break;
	}
	return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// Listing the account's voices (ElevenLabs, Google).
inline QUrl ttsVoicesUrl(TtsProvider p, const QString &language)
{
	if (p == TtsProvider::ElevenLabs)
		return QUrl(QStringLiteral("https://api.elevenlabs.io/v1/voices"));
	QUrl u(QStringLiteral("https://texttospeech.googleapis.com/v1/voices"));
	QUrlQuery q;
	q.addQueryItem(QStringLiteral("languageCode"), googleLocale(language));
	u.setQuery(q);
	return u;
}

// How a narration was made, kept on its audio clip (TlClip::tts) so one
// sentence can be spoken again later in the same voice, at the same pace.
struct TtsSpec {
	TtsProvider provider = TtsProvider::OpenAI;
	QString voice, language, style;
	QString model;      // ElevenLabs: the model it was made with
	double speed = 1.0; // the service's own speaking rate
	double tempo = 1.0; // the stretch applied afterwards (Fit to selection)

	QString toJson() const
	{
		QJsonObject o;
		o.insert(QStringLiteral("provider"), QString::fromLatin1(ttsProviderInfo(provider).id));
		o.insert(QStringLiteral("voice"), voice);
		o.insert(QStringLiteral("language"), language);
		if (!style.isEmpty())
			o.insert(QStringLiteral("style"), style);
		if (!model.isEmpty())
			o.insert(QStringLiteral("model"), model);
		o.insert(QStringLiteral("speed"), speed);
		o.insert(QStringLiteral("tempo"), tempo);
		return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
	}
	static TtsSpec fromJson(const QString &json, bool *ok = nullptr)
	{
		TtsSpec s;
		const QJsonDocument d = QJsonDocument::fromJson(json.toUtf8());
		const QJsonObject o = d.object();
		s.provider = ttsProviderFromId(o.value(QStringLiteral("provider")).toString());
		s.voice = o.value(QStringLiteral("voice")).toString();
		s.language = o.value(QStringLiteral("language")).toString();
		s.style = o.value(QStringLiteral("style")).toString();
		s.model = o.value(QStringLiteral("model")).toString();
		s.speed = o.value(QStringLiteral("speed")).toDouble(1.0);
		s.tempo = o.value(QStringLiteral("tempo")).toDouble(1.0);
		if (ok)
			*ok = d.isObject() && !s.voice.isEmpty();
		return s;
	}
};

// ---- replies ----------------------------------------------------------------

namespace ttsdetail {
inline bool jsonObject(const QByteArray &json, QJsonObject *o)
{
	const QJsonDocument doc = QJsonDocument::fromJson(json);
	if (!doc.isObject())
		return false;
	*o = doc.object();
	return true;
}
inline qint64 ms(double seconds)
{
	return qint64(std::llround(seconds * 1000.0));
}
} // namespace ttsdetail

// The service's own words for what went wrong, from an error body; empty
// when there are none.
inline QString ttsErrorMessage(TtsProvider p, const QByteArray &json)
{
	QJsonObject o;
	if (!ttsdetail::jsonObject(json, &o))
		return QString();
	switch (p) {
	case TtsProvider::OpenAI:
	case TtsProvider::Google: {
		const QJsonValue e = o.value(QStringLiteral("error"));
		return e.isObject() ? e.toObject().value(QStringLiteral("message")).toString() : e.toString();
	}
	case TtsProvider::ElevenLabs: {
		const QJsonValue d = o.value(QStringLiteral("detail"));
		if (d.isString())
			return d.toString();
		if (d.isObject())
			return d.toObject().value(QStringLiteral("message")).toString();
		if (d.isArray() && !d.toArray().isEmpty())
			return d.toArray().first().toObject().value(QStringLiteral("msg")).toString();
		return QString();
	}
	case TtsProvider::Deepgram:
		if (!o.value(QStringLiteral("err_msg")).toString().isEmpty())
			return o.value(QStringLiteral("err_msg")).toString();
		return o.value(QStringLiteral("message")).toString();
	}
	return QString();
}

// A WAV's length from its header (16-bit PCM, as these services send it).
inline qint64 wavBytesDurationMs(const QByteArray &wav)
{
	if (wav.size() < 44 || !wav.startsWith("RIFF") || wav.mid(8, 4) != "WAVE")
		return 0;
	const auto u16 = [&](int at) { return int(quint8(wav[at])) | int(quint8(wav[at + 1])) << 8; };
	const auto u32 = [&](int at) { return quint32(u16(at)) | quint32(u16(at + 2)) << 16; };
	int at = 12, channels = 0, rate = 0, bits = 0;
	while (at + 8 <= wav.size()) {
		const QByteArray id = wav.mid(at, 4);
		quint32 len = u32(at + 4);
		if (id == "fmt " && at + 24 <= wav.size()) {
			channels = u16(at + 10);
			rate = int(u32(at + 12));
			bits = u16(at + 22);
		} else if (id == "data") {
			// A streamed WAV says 0 or 0xFFFFFFFF here: the rest of the file.
			if (len == 0 || len == 0xFFFFFFFFu || at + 8 + qint64(len) > wav.size())
				len = quint32(wav.size() - at - 8);
			if (channels <= 0 || rate <= 0 || bits <= 0)
				return 0;
			return qint64(len) * 1000 / (qint64(rate) * channels * (bits / 8));
		}
		at += 8 + int(len) + int(len & 1);
	}
	return 0;
}

// Where each written word falls, from the time of each CHARACTER (ElevenLabs):
// a word runs from its first character's start to its last one's end. A word
// not found in the characters (the service rewrote it) gets no time here;
// fillUnmatched() places it between its neighbours.
inline QVector<ClipWordTime> wordsFromCharacters(const QVector<TtsWord> &words, const QStringList &chars,
						 const QVector<double> &starts, const QVector<double> &ends,
						 QVector<bool> *matched)
{
	QString all;
	QVector<int> elemAt; // for each UTF-16 unit of `all`: which character it belongs to
	for (int i = 0; i < chars.size(); ++i) {
		all += chars[i];
		for (int k = 0; k < chars[i].size(); ++k)
			elemAt.append(i);
	}
	QVector<ClipWordTime> out;
	matched->fill(false, words.size());
	int cursor = 0;
	for (int i = 0; i < words.size(); ++i) {
		ClipWordTime w;
		w.text = words[i].text;
		// The word as it stands, or (joined punctuation aside) its first part.
		const QString core = words[i].text.section(QLatin1Char(' '), 0, 0);
		// Case aside: a stressed word is sent in CAPITALS.
		const int p = int(all.indexOf(core, cursor, Qt::CaseInsensitive));
		if (p >= 0 && p - cursor < 200) {
			const int a = elemAt[p], b = elemAt[p + core.size() - 1];
			if (a < starts.size() && b < ends.size()) {
				w.startMs = ttsdetail::ms(starts[a]);
				w.endMs = std::max(w.startMs, ttsdetail::ms(ends[b]));
				(*matched)[i] = true;
				cursor = p + int(core.size());
			}
		}
		out.append(w);
	}
	return out;
}

// From Google's marks: word N starts at mark wN; it ends where the next word
// starts, or sooner when that is a long way off (a pause after a sentence) --
// at about the time the word itself takes to say.
inline QVector<ClipWordTime> wordsFromMarks(const QVector<TtsWord> &words, const QHash<QString, double> &marks,
					    qint64 totalMs, QVector<bool> *matched)
{
	QVector<ClipWordTime> out(words.size());
	matched->fill(false, words.size());
	for (int i = 0; i < words.size(); ++i) {
		out[i].text = words[i].text;
		const auto it = marks.constFind(QStringLiteral("w%1").arg(i));
		if (it != marks.constEnd()) {
			out[i].startMs = ttsdetail::ms(it.value());
			(*matched)[i] = true;
		}
	}
	for (int i = 0; i < words.size(); ++i) {
		if (!(*matched)[i])
			continue;
		qint64 next = totalMs;
		for (int j = i + 1; j < words.size(); ++j)
			if ((*matched)[j]) {
				next = out[j].startMs;
				break;
			}
		const qint64 say = 120 + 70 * qint64(words[i].text.size());
		out[i].endMs = std::max(out[i].startMs, std::min(next, out[i].startMs + say));
	}
	return out;
}

// Lower case, no accents, letters and digits only: "Você," and "voce" match.
inline QString ttsNormal(const QString &s)
{
	const QString d = s.normalized(QString::NormalizationForm_D).toLower();
	QString out;
	for (const QChar c : d)
		if (c.isLetterOrNumber())
			out += c;
	return out;
}

// From a transcription of the audio (OpenAI, Deepgram): the written words are
// matched to the heard ones in order (the longest common run, allowing a
// letter's difference in longer words), and each written word takes the time
// of the heard word it matches.
inline QVector<ClipWordTime> alignToTranscript(const QVector<TtsWord> &words, const QVector<ClipWordTime> &heard,
					       QVector<bool> *matched)
{
	const int n = int(words.size()), m = int(heard.size());
	QVector<QString> a(n), b(m);
	for (int i = 0; i < n; ++i)
		a[i] = ttsNormal(words[i].text);
	for (int j = 0; j < m; ++j)
		b[j] = ttsNormal(heard[j].text);
	const auto same = [&](int i, int j) {
		const QString &x = a[i], &y = b[j];
		if (x.isEmpty() || y.isEmpty())
			return false;
		if (x == y)
			return true;
		if (x.size() < 4 || std::abs(int(x.size()) - int(y.size())) > 1)
			return false;
		// One letter different, missing or extra.
		int i1 = 0, j1 = 0, diff = 0;
		while (i1 < x.size() && j1 < y.size()) {
			if (x[i1] == y[j1]) {
				++i1, ++j1;
				continue;
			}
			if (++diff > 1)
				return false;
			if (x.size() > y.size())
				++i1;
			else if (y.size() > x.size())
				++j1;
			else
				++i1, ++j1;
		}
		return diff + (x.size() - i1) + (y.size() - j1) <= 1;
	};
	// Longest common subsequence, then walk it back.
	QVector<int> dp(size_t(n + 1) * size_t(m + 1), 0);
	const auto at = [&](int i, int j) -> int & { return dp[size_t(i) * size_t(m + 1) + size_t(j)]; };
	for (int i = n - 1; i >= 0; --i)
		for (int j = m - 1; j >= 0; --j)
			at(i, j) = same(i, j) ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
	QVector<ClipWordTime> out(n);
	matched->fill(false, n);
	for (int i = 0; i < n; ++i)
		out[i].text = words[i].text;
	int i = 0, j = 0;
	while (i < n && j < m) {
		if (same(i, j) && at(i, j) == at(i + 1, j + 1) + 1) {
			out[i].startMs = heard[j].startMs;
			out[i].endMs = std::max(heard[j].startMs, heard[j].endMs);
			(*matched)[i] = true;
			++i, ++j;
		} else if (at(i + 1, j) >= at(i, j + 1)) {
			++i;
		} else {
			++j;
		}
	}
	return out;
}

// The words left without a time take the gap between their timed neighbours,
// shared out by length -- the start of the audio before the first, its end
// after the last.
inline void fillUnmatched(QVector<ClipWordTime> &words, const QVector<bool> &matched, qint64 totalMs)
{
	const int n = int(words.size());
	int i = 0;
	while (i < n) {
		if (matched.value(i)) {
			++i;
			continue;
		}
		int j = i;
		while (j < n && !matched.value(j))
			++j;
		const qint64 from = i > 0 ? words[i - 1].endMs : 0;
		qint64 to = j < n ? words[j].startMs : std::max(totalMs, from);
		to = std::max(to, from);
		int letters = 0;
		for (int k = i; k < j; ++k)
			letters += std::max(1, int(words[k].text.size()));
		qint64 t = from;
		for (int k = i; k < j; ++k) {
			const qint64 len = (to - from) * std::max(1, int(words[k].text.size())) / std::max(1, letters);
			words[k].startMs = t;
			words[k].endMs = t + len;
			t += len;
		}
		i = j;
	}
	// Never backwards, whatever the services said.
	for (int k = 1; k < n; ++k)
		if (words[k].startMs < words[k - 1].startMs)
			words[k].startMs = words[k - 1].startMs;
	for (ClipWordTime &w : words)
		w.endMs = std::max(w.endMs, w.startMs);
}

// One piece's reply: its audio and, when the service gives them, the times of
// its words (relative to the start of this piece's audio).
struct TtsReply {
	QByteArray audio;
	QString ext;                  // "wav" | "mp3": what `audio` is
	QVector<ClipWordTime> words;  // empty when the service gives none (timed = false)
	bool timed = false;
};

inline TtsReply parseTtsReply(TtsProvider p, const QByteArray &body, const QString &pieceText, QString *err)
{
	TtsReply r;
	const QVector<TtsWord> words = ttsWords(pieceText);
	switch (p) {
	case TtsProvider::OpenAI:
	case TtsProvider::Deepgram:
		r.audio = body;
		r.ext = QStringLiteral("wav");
		if (!body.startsWith("RIFF") && err)
			*err = QStringLiteral("the reply was not audio");
		return r;
	case TtsProvider::ElevenLabs: {
		QJsonObject o;
		if (!ttsdetail::jsonObject(body, &o)) {
			if (err)
				*err = QStringLiteral("the reply was not JSON");
			return r;
		}
		r.audio = QByteArray::fromBase64(o.value(QStringLiteral("audio_base64")).toString().toLatin1());
		r.ext = QStringLiteral("mp3");
		const QJsonObject al = o.value(QStringLiteral("alignment")).toObject();
		QStringList chars;
		QVector<double> starts, ends;
		for (const QJsonValue &v : al.value(QStringLiteral("characters")).toArray())
			chars << v.toString();
		for (const QJsonValue &v : al.value(QStringLiteral("character_start_times_seconds")).toArray())
			starts << v.toDouble();
		for (const QJsonValue &v : al.value(QStringLiteral("character_end_times_seconds")).toArray())
			ends << v.toDouble();
		if (r.audio.isEmpty()) {
			if (err)
				*err = ttsErrorMessage(p, body).isEmpty() ? QStringLiteral("the reply carried no audio")
									  : ttsErrorMessage(p, body);
			return r;
		}
		if (chars.isEmpty() || starts.size() != chars.size() || ends.size() != chars.size()) {
			// No character times (a model that does not give them): the
			// words are timed by transcribing the audio, as for OpenAI.
			r.timed = false;
			return r;
		}
		QVector<bool> matched;
		r.words = wordsFromCharacters(words, chars, starts, ends, &matched);
		const qint64 total = ends.isEmpty() ? 0 : ttsdetail::ms(ends.last());
		fillUnmatched(r.words, matched, total);
		r.timed = true;
		return r;
	}
	case TtsProvider::Google: {
		QJsonObject o;
		if (!ttsdetail::jsonObject(body, &o)) {
			if (err)
				*err = QStringLiteral("the reply was not JSON");
			return r;
		}
		r.audio = QByteArray::fromBase64(o.value(QStringLiteral("audioContent")).toString().toLatin1());
		r.ext = QStringLiteral("wav"); // LINEAR16 comes with a WAV header
		if (r.audio.isEmpty()) {
			if (err)
				*err = ttsErrorMessage(p, body).isEmpty() ? QStringLiteral("the reply carried no audio")
									  : ttsErrorMessage(p, body);
			return r;
		}
		QHash<QString, double> marks;
		for (const QJsonValue &v : o.value(QStringLiteral("timepoints")).toArray()) {
			const QJsonObject tp = v.toObject();
			marks.insert(tp.value(QStringLiteral("markName")).toString(),
				     tp.value(QStringLiteral("timeSeconds")).toDouble());
		}
		const qint64 total = wavBytesDurationMs(r.audio);
		QVector<bool> matched;
		r.words = wordsFromMarks(words, marks, total, &matched);
		fillUnmatched(r.words, matched, total);
		r.timed = true;
		return r;
	}
	}
	return r;
}

// The account's voices, from Load my voices. Google's Chirp voices are left
// out: they cannot mark words, so their captions would have no times.
inline QVector<TtsVoice> parseTtsVoices(TtsProvider p, const QByteArray &body)
{
	QVector<TtsVoice> out;
	QJsonObject o;
	if (!ttsdetail::jsonObject(body, &o))
		return out;
	for (const QJsonValue &v : o.value(QStringLiteral("voices")).toArray()) {
		const QJsonObject vo = v.toObject();
		if (p == TtsProvider::ElevenLabs) {
			const QString id = vo.value(QStringLiteral("voice_id")).toString();
			QString name = vo.value(QStringLiteral("name")).toString();
			const QJsonObject labels = vo.value(QStringLiteral("labels")).toObject();
			QStringList about;
			for (const char *k : {"accent", "gender", "description"})
				if (!labels.value(QLatin1String(k)).toString().isEmpty())
					about << labels.value(QLatin1String(k)).toString();
			if (!about.isEmpty())
				name += QStringLiteral("  (%1)").arg(about.join(QStringLiteral(", ")));
			if (!id.isEmpty())
				out.append({id, name});
		} else {
			const QString name = vo.value(QStringLiteral("name")).toString();
			if (name.isEmpty() || name.contains(QLatin1String("Chirp")))
				continue;
			const QString gender = vo.value(QStringLiteral("ssmlGender")).toString().toLower();
			out.append({name, gender.isEmpty() ? name : QStringLiteral("%1  (%2)").arg(name, gender)});
		}
	}
	if (p == TtsProvider::Google) {
		// The better families first.
		const auto rank = [](const QString &n) {
			return n.contains(QLatin1String("Neural2"))   ? 0
			       : n.contains(QLatin1String("Wavenet")) ? 1
			       : n.contains(QLatin1String("Studio"))  ? 2
								      : 3;
		};
		std::stable_sort(out.begin(), out.end(), [&](const TtsVoice &a, const TtsVoice &b) {
			return rank(a.id) != rank(b.id) ? rank(a.id) < rank(b.id) : a.id < b.id;
		});
	}
	return out;
}

} // namespace harpia
