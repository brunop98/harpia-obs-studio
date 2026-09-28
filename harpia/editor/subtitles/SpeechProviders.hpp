#pragma once

// The speech-to-text services the Subtitles window can use, and the pure half
// of talking to each: what the request looks like, and how the reply becomes
// a Transcript. SpeechTranscriber moves the bytes; nothing here touches the
// network, so every format is checkable in a test without a server.
//
// Every service here returns WORD-level timestamps. That is the entry ticket:
// word times are what caption grouping and "one word at a time" are made of.
//
//   OpenAI      multipart upload, verbose_json           (Bearer key)
//   Groq        the same request as OpenAI, other host   (Bearer key)
//   Deepgram    raw WAV body, options in the query        ("Token" key)
//   AssemblyAI  upload, create a job, poll until done     (plain key)
//   ElevenLabs  multipart upload, its own field names     (xi-api-key)
//
// Model names are constants in the table below, so moving to a newer model is
// a one-line change.

#include "Transcript.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace harpia {

enum class SpeechProvider { OpenAI = 0, Groq, Deepgram, AssemblyAI, ElevenLabs };

struct SpeechProviderInfo {
	SpeechProvider provider = SpeechProvider::OpenAI;
	const char *id = "";          // settings and key storage: "openai", "groq", ...
	const char *name = "";        // what the picker shows
	const char *model = "";       // sent to the service
	const char *keyUrl = "";      // where to get a key
	const char *keySteps = "";    // the clicks on that page
	const char *keyPlaceholder = "";
	const char *about = "";       // one line under the picker
	bool uploadLimit25Mb = false; // refuses one request over 25 MB
};

inline const QVector<SpeechProviderInfo> &speechProviders()
{
	static const QVector<SpeechProviderInfo> all = {
		{SpeechProvider::OpenAI, "openai", "OpenAI (Whisper)", "whisper-1", "https://platform.openai.com/api-keys",
		 "OpenAI dashboard ▸ API keys ▸ Create new secret key", "sk-…  (paste your OpenAI API key)",
		 "Reliable all-rounder. Billed per minute of audio; needs billing set up on the account.", true},
		{SpeechProvider::Groq, "groq", "Groq (Whisper, fast)", "whisper-large-v3-turbo", "https://console.groq.com/keys",
		 "GroqCloud console ▸ API Keys ▸ Create API Key", "gsk_…  (paste your Groq API key)",
		 "Whisper large-v3 on very fast hardware: usually the quickest and cheapest. Has a free tier.", true},
		{SpeechProvider::Deepgram, "deepgram", "Deepgram (Nova-3)", "nova-3",
		 "https://console.deepgram.com/",
		 "Deepgram console ▸ API Keys ▸ Create a New API Key", "paste your Deepgram API key",
		 "Very accurate, adds punctuation and capitals. New accounts start with free credit.", false},
		{SpeechProvider::AssemblyAI, "assemblyai", "AssemblyAI", "", "https://www.assemblyai.com/app/api-keys",
		 "AssemblyAI dashboard ▸ API Keys", "paste your AssemblyAI API key",
		 "Accurate; uploads the audio, then waits for the job, so it takes a little longer. Free credit to start.",
		 false},
		{SpeechProvider::ElevenLabs, "elevenlabs", "ElevenLabs (Scribe)", "scribe_v1",
		 "https://elevenlabs.io/app/settings/api-keys",
		 "ElevenLabs ▸ Settings ▸ API Keys ▸ Create API Key", "sk_…  (paste your ElevenLabs API key)",
		 "Strong across many languages. The key needs the Speech to Text permission.", false},
	};
	return all;
}

inline const SpeechProviderInfo &speechProviderInfo(SpeechProvider p)
{
	for (const SpeechProviderInfo &i : speechProviders())
		if (i.provider == p)
			return i;
	return speechProviders().first();
}

// Unknown or empty ids fall back to OpenAI, the one the app shipped with.
inline SpeechProvider speechProviderFromId(const QString &id)
{
	for (const SpeechProviderInfo &i : speechProviders())
		if (id == QLatin1String(i.id))
			return i.provider;
	return SpeechProvider::OpenAI;
}

inline QString speechProviderName(SpeechProvider p)
{
	return QString::fromUtf8(speechProviderInfo(p).name);
}

// ---- requests -------------------------------------------------------------

struct SpeechJob {
	SpeechProvider provider = SpeechProvider::OpenAI;
	QString wavPath;
	QString apiKey;
	QString language; // ISO-639-1 ("pt", "en"); empty = let the service detect
	QString prompt;   // optional vocabulary hint (OpenAI and Groq only)
};

namespace speechdetail {
inline void formField(QByteArray &b, const QByteArray &boundary, const char *name, const QString &value)
{
	b += "--" + boundary + "\r\n";
	b += "Content-Disposition: form-data; name=\"" + QByteArray(name) + "\"\r\n\r\n";
	b += value.toUtf8() + "\r\n";
}
inline void formFile(QByteArray &b, const QByteArray &boundary, const QByteArray &wavBytes)
{
	b += "--" + boundary + "\r\n";
	b += "Content-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n";
	b += "Content-Type: audio/wav\r\n\r\n";
	b += wavBytes;
	b += "\r\n--" + boundary + "--\r\n";
}
inline qint64 secToMs(const QJsonValue &v)
{
	return qint64(std::llround(v.toDouble() * 1000.0));
}
inline void sortWords(Transcript &t)
{
	std::stable_sort(t.words.begin(), t.words.end(),
			 [](const ClipWordTime &a, const ClipWordTime &b) { return a.startMs < b.startMs; });
}
inline bool jsonObject(const QByteArray &json, QJsonObject *o, QString *err)
{
	QJsonParseError pe;
	const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
	if (doc.isNull() || !doc.isObject()) {
		if (err)
			*err = QStringLiteral("not JSON: %1").arg(pe.errorString());
		return false;
	}
	*o = doc.object();
	return true;
}
} // namespace speechdetail

// OpenAI and Groq: multipart/form-data asking for verbose_json with word
// timestamps. The key is never in the body; it goes in the header.
inline QByteArray openAiStyleBody(const SpeechJob &job, const QByteArray &wavBytes, const QByteArray &boundary)
{
	using speechdetail::formField;
	QByteArray b;
	formField(b, boundary, "model", QString::fromUtf8(speechProviderInfo(job.provider).model));
	formField(b, boundary, "response_format", QStringLiteral("verbose_json"));
	formField(b, boundary, "timestamp_granularities[]", QStringLiteral("word"));
	formField(b, boundary, "timestamp_granularities[]", QStringLiteral("segment"));
	if (!job.language.trimmed().isEmpty())
		formField(b, boundary, "language", job.language.trimmed());
	if (!job.prompt.trimmed().isEmpty())
		formField(b, boundary, "prompt", job.prompt.trimmed());
	speechdetail::formFile(b, boundary, wavBytes);
	return b;
}

// ElevenLabs Scribe: multipart with its own field names. Audio events
// ("(laughter)") are switched off: they are not words anyone said.
inline QByteArray elevenLabsBody(const SpeechJob &job, const QByteArray &wavBytes, const QByteArray &boundary)
{
	using speechdetail::formField;
	QByteArray b;
	formField(b, boundary, "model_id", QString::fromUtf8(speechProviderInfo(job.provider).model));
	formField(b, boundary, "timestamps_granularity", QStringLiteral("word"));
	formField(b, boundary, "tag_audio_events", QStringLiteral("false"));
	if (!job.language.trimmed().isEmpty())
		formField(b, boundary, "language_code", job.language.trimmed());
	speechdetail::formFile(b, boundary, wavBytes);
	return b;
}

// Deepgram: the WAV is the body; everything else is in the query.
inline QUrl deepgramUrl(const SpeechJob &job)
{
	QUrl u(QStringLiteral("https://api.deepgram.com/v1/listen"));
	QUrlQuery q;
	q.addQueryItem(QStringLiteral("model"), QString::fromUtf8(speechProviderInfo(job.provider).model));
	q.addQueryItem(QStringLiteral("smart_format"), QStringLiteral("true"));
	q.addQueryItem(QStringLiteral("punctuate"), QStringLiteral("true"));
	if (job.language.trimmed().isEmpty())
		q.addQueryItem(QStringLiteral("detect_language"), QStringLiteral("true"));
	else
		q.addQueryItem(QStringLiteral("language"), job.language.trimmed());
	u.setQuery(q);
	return u;
}

// AssemblyAI step 2: the job, pointing at the uploaded audio.
inline QByteArray assemblyAiCreateBody(const QString &uploadUrl, const QString &language)
{
	QJsonObject o;
	o.insert(QStringLiteral("audio_url"), uploadUrl);
	o.insert(QStringLiteral("punctuate"), true);
	o.insert(QStringLiteral("format_text"), true);
	if (language.trimmed().isEmpty())
		o.insert(QStringLiteral("language_detection"), true);
	else
		o.insert(QStringLiteral("language_code"), language.trimmed());
	return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// ---- replies --------------------------------------------------------------

// The service's own words for what went wrong, from an error body; empty when
// the body carries none. Each service shapes its errors differently.
inline QString speechErrorMessage(SpeechProvider p, const QByteArray &json)
{
	QJsonObject o;
	if (!speechdetail::jsonObject(json, &o, nullptr))
		return QString();
	const auto str = [&](const char *k) { return o.value(QLatin1String(k)).toString(); };
	switch (p) {
	case SpeechProvider::OpenAI:
	case SpeechProvider::Groq: {
		const QJsonValue e = o.value(QStringLiteral("error"));
		return e.isObject() ? e.toObject().value(QStringLiteral("message")).toString() : e.toString();
	}
	case SpeechProvider::Deepgram:
		if (!str("err_msg").isEmpty())
			return str("err_msg");
		return str("message");
	case SpeechProvider::AssemblyAI:
		return str("error");
	case SpeechProvider::ElevenLabs: {
		const QJsonValue d = o.value(QStringLiteral("detail"));
		if (d.isString())
			return d.toString();
		if (d.isObject())
			return d.toObject().value(QStringLiteral("message")).toString();
		if (d.isArray() && !d.toArray().isEmpty())
			return d.toArray().first().toObject().value(QStringLiteral("msg")).toString();
		return QString();
	}
	}
	return QString();
}

// Deepgram: results.channels[0].alternatives[0].words, seconds. The
// punctuated form ("Hello,") is what a caption should show.
inline Transcript parseDeepgramJson(const QByteArray &json, QString *err = nullptr)
{
	Transcript t;
	QJsonObject o;
	if (!speechdetail::jsonObject(json, &o, err))
		return t;
	const QString msg = speechErrorMessage(SpeechProvider::Deepgram, json);
	if (!o.contains(QStringLiteral("results")) && !msg.isEmpty()) {
		if (err)
			*err = msg;
		return t;
	}
	const QJsonArray channels = o.value(QStringLiteral("results")).toObject().value(QStringLiteral("channels")).toArray();
	if (!channels.isEmpty()) {
		const QJsonObject ch = channels.first().toObject();
		t.language = ch.value(QStringLiteral("detected_language")).toString();
		const QJsonArray alts = ch.value(QStringLiteral("alternatives")).toArray();
		if (!alts.isEmpty()) {
			const QJsonObject alt = alts.first().toObject();
			t.text = alt.value(QStringLiteral("transcript")).toString().trimmed();
			for (const QJsonValue &wv : alt.value(QStringLiteral("words")).toArray()) {
				const QJsonObject wo = wv.toObject();
				ClipWordTime w;
				w.text = wo.value(QStringLiteral("punctuated_word")).toString().trimmed();
				if (w.text.isEmpty())
					w.text = wo.value(QStringLiteral("word")).toString().trimmed();
				w.startMs = speechdetail::secToMs(wo.value(QStringLiteral("start")));
				w.endMs = std::max(w.startMs, speechdetail::secToMs(wo.value(QStringLiteral("end"))));
				if (!w.text.isEmpty())
					t.words.append(w);
			}
		}
	}
	speechdetail::sortWords(t);
	if (t.words.isEmpty() && err && err->isEmpty())
		*err = QStringLiteral("the transcription came back without any words");
	return t;
}

// AssemblyAI's job status, from a create or poll reply.
struct AssemblyAiStatus {
	QString id;
	QString status; // "queued" | "processing" | "completed" | "error"
	QString error;
};

inline AssemblyAiStatus parseAssemblyAiStatus(const QByteArray &json)
{
	AssemblyAiStatus s;
	QJsonObject o;
	if (!speechdetail::jsonObject(json, &o, &s.error))
		return s;
	s.id = o.value(QStringLiteral("id")).toString();
	s.status = o.value(QStringLiteral("status")).toString();
	s.error = o.value(QStringLiteral("error")).toString();
	return s;
}

// AssemblyAI step 1's reply: where the audio now lives.
inline QString parseAssemblyAiUploadUrl(const QByteArray &json)
{
	QJsonObject o;
	if (!speechdetail::jsonObject(json, &o, nullptr))
		return QString();
	return o.value(QStringLiteral("upload_url")).toString();
}

// AssemblyAI, a completed job: words with start/end already in ms.
inline Transcript parseAssemblyAiJson(const QByteArray &json, QString *err = nullptr)
{
	Transcript t;
	QJsonObject o;
	if (!speechdetail::jsonObject(json, &o, err))
		return t;
	const QString status = o.value(QStringLiteral("status")).toString();
	if (status == QLatin1String("error") || (!o.contains(QStringLiteral("words")) && o.contains(QStringLiteral("error")))) {
		if (err)
			*err = o.value(QStringLiteral("error")).toString();
		return t;
	}
	t.language = o.value(QStringLiteral("language_code")).toString();
	t.text = o.value(QStringLiteral("text")).toString().trimmed();
	for (const QJsonValue &wv : o.value(QStringLiteral("words")).toArray()) {
		const QJsonObject wo = wv.toObject();
		ClipWordTime w;
		w.text = wo.value(QStringLiteral("text")).toString().trimmed();
		w.startMs = qint64(std::llround(wo.value(QStringLiteral("start")).toDouble()));
		w.endMs = std::max(w.startMs, qint64(std::llround(wo.value(QStringLiteral("end")).toDouble())));
		if (!w.text.isEmpty())
			t.words.append(w);
	}
	speechdetail::sortWords(t);
	if (t.words.isEmpty() && err && err->isEmpty())
		*err = QStringLiteral("the transcription came back without any words");
	return t;
}

// ElevenLabs: words, spacing and audio events in one list; only the words
// are kept. Times in seconds.
inline Transcript parseElevenLabsJson(const QByteArray &json, QString *err = nullptr)
{
	Transcript t;
	QJsonObject o;
	if (!speechdetail::jsonObject(json, &o, err))
		return t;
	if (o.contains(QStringLiteral("detail")) && !o.contains(QStringLiteral("words"))) {
		if (err)
			*err = speechErrorMessage(SpeechProvider::ElevenLabs, json);
		return t;
	}
	t.language = o.value(QStringLiteral("language_code")).toString();
	t.text = o.value(QStringLiteral("text")).toString().trimmed();
	for (const QJsonValue &wv : o.value(QStringLiteral("words")).toArray()) {
		const QJsonObject wo = wv.toObject();
		const QString type = wo.value(QStringLiteral("type")).toString();
		if (!type.isEmpty() && type != QLatin1String("word"))
			continue;
		ClipWordTime w;
		w.text = wo.value(QStringLiteral("text")).toString().trimmed();
		w.startMs = speechdetail::secToMs(wo.value(QStringLiteral("start")));
		w.endMs = std::max(w.startMs, speechdetail::secToMs(wo.value(QStringLiteral("end"))));
		if (!w.text.isEmpty())
			t.words.append(w);
	}
	speechdetail::sortWords(t);
	if (t.words.isEmpty() && err && err->isEmpty())
		*err = QStringLiteral("the transcription came back without any words");
	return t;
}

// The final reply of any service, as a Transcript.
inline Transcript parseSpeechReply(SpeechProvider p, const QByteArray &json, QString *err = nullptr)
{
	switch (p) {
	case SpeechProvider::OpenAI:
	case SpeechProvider::Groq: return parseOpenAiVerboseJson(json, err);
	case SpeechProvider::Deepgram: return parseDeepgramJson(json, err);
	case SpeechProvider::AssemblyAI: return parseAssemblyAiJson(json, err);
	case SpeechProvider::ElevenLabs: return parseElevenLabsJson(json, err);
	}
	return parseOpenAiVerboseJson(json, err);
}

} // namespace harpia
