// Subtitles from speech: the pure half. The API's JSON becomes words; words
// become captions by the user's rules; captions become clips aligned to the
// media clip they came from; and the Subtitle component shows them the way
// the speaker said them. None of this needs a network or a decoder, which is
// what makes it checkable here -- the network half is a thin envelope around
// it and is checked only for what it puts on the wire.
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ComponentStack.hpp"
#include "editor/component/TextSubtitle.hpp"
#include "editor/subtitles/AudioForSpeech.hpp"
#include "editor/subtitles/SecretStore.hpp"
#include "editor/subtitles/SpeechProviders.hpp"
#include "editor/subtitles/SpeechTranscriber.hpp"
#include "editor/subtitles/SubtitleDialog.hpp"
#include "editor/subtitles/Transcript.hpp"
#include "editor/timeline/TimelineJson.hpp"

#include <QApplication>
#include <QTemporaryDir>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static void eqs(const QString &got, const char *want, const char *w)
{
	const bool good = got == QString::fromUtf8(want);
	std::printf("  %s %s (got \"%s\")\n", good ? "PASS" : "FAIL", w, qPrintable(got));
	if (!good)
		++failures;
}

static ClipWordTime W(const char *t, qint64 s, qint64 e)
{
	ClipWordTime w;
	w.text = QString::fromUtf8(t);
	w.startMs = s;
	w.endMs = e;
	return w;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- the API's JSON becomes words in ms --\n");
	{
		const QByteArray json = R"({
		  "task":"transcribe","language":"portuguese","duration":3.2,
		  "text":" Olá, mundo. Tudo bem? ",
		  "words":[{"word":"Olá,","start":0.32,"end":0.61},{"word":"mundo.","start":0.7,"end":1.18},
		           {"word":"Tudo","start":2.0,"end":2.3},{"word":"bem?","start":2.31,"end":2.8}],
		  "segments":[{"text":" Olá, mundo. Tudo bem?","start":0.3,"end":2.8}]
		})";
		QString err;
		const Transcript t = parseOpenAiVerboseJson(json, &err);
		ok(err.isEmpty() && t.words.size() == 4, "four words, no error");
		ok(t.words[0].startMs == 320 && t.words[0].endMs == 610, "seconds become milliseconds");
		eqs(t.words[1].text, "mundo.", "punctuation stays on the word");
		eqs(t.text, "Olá, mundo. Tudo bem?", "and the whole text is kept, trimmed");

		// No word timestamps: the segment is spread evenly.
		const QByteArray segOnly = R"({"text":"one two three four","segments":[{"text":" one two three four","start":10.0,"end":12.0}]})";
		const Transcript s = parseOpenAiVerboseJson(segOnly, &err);
		ok(s.words.size() == 4 && s.words[0].startMs == 10000 && s.words[3].endMs == 12000 &&
			   s.words[1].startMs == 10500,
		   "without word times, a segment's words are paced evenly across it");

		const Transcript bad = parseOpenAiVerboseJson(R"({"error":{"message":"Incorrect API key provided"}})", &err);
		ok(bad.words.isEmpty() && err.contains(QStringLiteral("Incorrect API key")),
		   "the API's own error message is what comes back");
		err.clear();
		parseOpenAiVerboseJson("<html>502</html>", &err);
		ok(!err.isEmpty(), "and a non-JSON body is an error, not a crash");
	}

	std::printf("\n-- words become captions by the rules --\n");
	{
		// "we go live at nine [pause] and again at noon" -- a long pause after "nine".
		const QVector<ClipWordTime> words = {W("we", 0, 200), W("go", 250, 400), W("live", 450, 800),
						     W("at", 850, 950), W("nine", 1000, 1400),
						     W("and", 2400, 2500), W("again", 2550, 2900),
						     W("at", 2950, 3050), W("noon", 3100, 3500)};
		GroupRule r; // 3 words, 2.5 s, 400 ms pause, 32 chars
		QVector<CaptionPiece> p = groupWords(words, r);
		QStringList texts;
		for (const CaptionPiece &c : p)
			texts << c.text();
		eqs(texts.join(QStringLiteral(" | ")), "we go live | at nine | and again at | noon",
		    "three words a caption, and the pause after 'nine' starts a new one");

		r.maxWords = 1;
		ok(groupWords(words, r).size() == 9, "one word per caption when asked");
		r.maxWords = 50;
		r.pauseMs = 0;
		r.maxChars = 100;
		r.maxDurationMs = 1000;
		p = groupWords(words, r);
		ok(p.size() >= 3 && (p[0].endMs() - p[0].startMs()) <= 1000, "the on-screen time cap splits too");
		r.maxDurationMs = 100000;
		r.maxChars = 12;
		p = groupWords(words, r);
		bool narrow = true;
		for (const CaptionPiece &c : p)
			narrow = narrow && c.text().size() <= 12;
		ok(narrow && p.size() > 1, "and so does the character cap");
		ok(groupWords({}, r).isEmpty(), "no words, no captions");
	}

	std::printf("\n-- captions become clips aligned to the media clip --\n");
	{
		// A media clip trimmed to start 5 s into its file, placed at 20 s on the
		// timeline, playing at 2x.
		TlClip media;
		media.type = TlClip::Type::Video;
		media.sourceId = 3;
		media.srcStartMs = 5000;
		media.srcEndMs = 15000;
		media.outStartMs = 20000;
		media.speed = 2.0;
		const QVector<ClipWordTime> words = {W("hello", 6000, 6400), W("there", 6500, 7000),
						     W("friend", 9000, 9500), W("late", 16000, 16500)};
		GroupRule r;
		r.pauseMs = 1000;
		SubtitleLook look;
		look.mode = SubtitleMode::OneWord;
		look.position = SubtitleLook::Position::Bottom;
		look.holdGapMs = 0;
		look.minDurationMs = 0;
		const QVector<TlClip> clips = buildSubtitleClips(groupWords(words, r), media, look);
		ok(clips.size() == 2, "two captions: 'hello there' and 'friend'; 'late' is past the trim and dropped");
		// "hello" at source 6000 = 1000 ms into the clip, at 2x = 500 ms after 20000.
		ok(clips[0].outStartMs == 20500, "a word 1 s into a 2x clip appears 0.5 s after the clip starts");
		ok(clips[0].outEndMs() == 20000 + (7000 - 5000) / 2, "and ends when its last word does, in timeline time");
		ok(clips[0].type == TlClip::Type::Text && clips[0].text.text == QStringLiteral("hello there"),
		   "it is a caption with the phrase as its text");
		ok(clips[0].words.size() == 2 && clips[0].words[0].startMs == 0 && clips[0].words[1].startMs == 250,
		   "word times are stored relative to the caption, scaled by the speed");
		ok(clips[0].components.size() == 1 && clips[0].components[0].typeId == QStringLiteral("harpia.subtitle") &&
			   clips[0].components[0].props.value(QStringLiteral("mode")).toInt() == 1,
		   "one-word mode attaches the Subtitle component set to one word at a time");
		ok(std::abs(clips[0].posY - 0.88) < 1e-9 && std::abs(clips[0].posX - 0.5) < 1e-9,
		   "bottom centre, inset from the edge");
		look.position = SubtitleLook::Position::Top;
		look.mode = SubtitleMode::Whole;
		const QVector<TlClip> top = buildSubtitleClips(groupWords(words, r), media, look);
		ok(std::abs(top[0].posY - 0.12) < 1e-9 && top[0].components.isEmpty(),
		   "top when asked, and Whole phrase needs no component at all");

		// Hold across a short gap, and a readable minimum.
		SubtitleLook held;
		held.holdGapMs = 700;
		held.minDurationMs = 600;
		TlClip plain;
		plain.type = TlClip::Type::Video;
		plain.srcStartMs = 0;
		plain.srcEndMs = 60000;
		plain.outStartMs = 0;
		const QVector<TlClip> h = buildSubtitleClips(
			groupWords({W("a", 0, 100), W("b", 500, 600), W("c", 3000, 3100)}, GroupRule{1, 5000, 0, 32}), plain, held);
		ok(h.size() == 3 && h[0].outEndMs() == 500, "a 400 ms gap is held to the next caption");
		ok(h[1].outEndMs() == 500 + 600, "a blink of a word is stretched to the readable minimum");
		ok(h[2].outEndMs() == 3600, "the last one too, with nothing to hold to");

		// Round trip through the project JSON keeps the word times.
		const TlClip back = clipFromJson(clipToJson(clips[0]));
		ok(back == clips[0] && back.words.size() == 2, "a subtitle clip survives save and load, word times included");
	}

	std::printf("\n-- the Subtitle component shows what the speaker said --\n");
	{
		const QString caption = QStringLiteral("we go live");
		const QVector<ClipWordTime> timed = {W("we", 0, 200), W("go", 500, 700), W("live", 1000, 1400)};
		eqs(subtitleTextAt(caption, timed, 600, SubtitleMode::Whole), "we go live", "Whole: the phrase, always");
		eqs(subtitleTextAt(caption, timed, 100, SubtitleMode::OneWord), "we", "OneWord: the first word while it is said");
		eqs(subtitleTextAt(caption, timed, 400, SubtitleMode::OneWord), "we",
		    "and held through the gap until the next word starts");
		eqs(subtitleTextAt(caption, timed, 1200, SubtitleMode::OneWord), "live", "then the last");
		eqs(subtitleTextAt(caption, timed, 600, SubtitleMode::BuildUp), "we go", "BuildUp: the words so far");
		ok(subtitleTextAt(caption, timed, -1, SubtitleMode::BuildUp).isEmpty(), "and nothing before the first");

		// A fixed typo keeps the timing; a rewrite is re-paced evenly.
		eqs(subtitleTextAt(QStringLiteral("we GO live"), timed, 600, SubtitleMode::OneWord), "GO",
		    "a one-word fix keeps every timing (same word count)");
		const QVector<ClipWordTime> re = reconciledWords(QStringLiteral("now we go live tonight"), timed);
		ok(re.size() == 5 && re[0].startMs == 0 && re[4].endMs == 1400 && re[2].startMs == 560,
		   "a rewrite with a different word count is spread evenly over the original span");
		ok(reconciledWords(QStringLiteral("solo"), {}).size() == 1 && reconciledWords(QString(), timed).isEmpty(),
		   "no timing at all still gives words a span; no words gives nothing");

		// Through the stack, from a clip's stored words.
		ComponentRegistry reg;
		registerBuiltinComponents(reg);
		const ComponentType *t = reg.find(QStringLiteral("harpia.subtitle"));
		ok(t && t->stage == Stage::Source && t->clipKinds == unsigned(ClipKindText) && t->props.size() == 1 &&
			   t->props[0].choices.size() == 3,
		   "Subtitle is a Source component for captions with one three-way choice");
		QVector<ComponentInstance> list;
		ComponentInstance ci;
		ci.typeId = QStringLiteral("harpia.subtitle");
		ci.instanceId = QStringLiteral("s1");
		ci.props.insert(QStringLiteral("mode"), 2.0);
		list.append(ci);
		ComponentStack st(list, reg);
		EvalContext ctx;
		ctx.durMs = 1400;
		ctx.tMs = 600;
		eqs(st.evaluatePose(ctx, TlTransform{}, &caption, &timed).text, "we go",
		    "the stack hands the words in and the component builds up");
		eqs(st.evaluatePose(ctx, TlTransform{}, &caption).text, "we go",
		    "with no stored words the caption is paced evenly, so it still animates");
	}

	std::printf("\n-- audio for the transcriber --\n");
	{
		// 48 kHz stereo, 6 frames -> 2 output samples, each the mean of its six inputs.
		const qint16 s[] = {600, 600, 600, 600, 600, 600, /**/ -300, 300, -300, 300, 0, 0};
		const QVector<qint16> m = AudioForSpeech::downmixTo16k(s, 6);
		ok(m.size() == 2 && m[0] == 600 && m[1] == 0, "16 kHz mono is the mean of three stereo frames");
		QTemporaryDir dir;
		const QString wav = dir.filePath(QStringLiteral("t.wav"));
		ok(AudioForSpeech::writeWav16k(wav, QVector<qint16>(16000, 1000)), "a second of 16 kHz WAV writes");
		QFile f(wav);
		f.open(QIODevice::ReadOnly);
		const QByteArray b = f.readAll();
		ok(b.size() == 44 + 32000 && b.startsWith("RIFF") && b.mid(24, 4) == QByteArray::fromHex("803e0000"),
		   "44-byte header, 16000 Hz, 32 KB of samples");
	}

	std::printf("\n-- what goes on the wire --\n");
	{
		SpeechTranscriber::Job j;
		j.language = QStringLiteral("pt");
		j.apiKey = QStringLiteral("sk-secret");
		const QByteArray body = SpeechTranscriber::multipartBody(j, QByteArrayLiteral("RIFFxxxx"), "B0UNDARY");
		ok(body.contains("name=\"model\"\r\n\r\nwhisper-1") && body.contains("name=\"response_format\"\r\n\r\nverbose_json") &&
			   body.contains("name=\"timestamp_granularities[]\"\r\n\r\nword") &&
			   body.contains("name=\"language\"\r\n\r\npt") &&
			   body.contains("filename=\"speech.wav\"") && body.contains("RIFFxxxx") && body.endsWith("--B0UNDARY--\r\n"),
		   "model, verbose_json, word timestamps, the language and the file, closed properly");
		ok(!body.contains("sk-secret"), "and the key is never in the body (it goes in the header)");
		j.language.clear();
		ok(!SpeechTranscriber::multipartBody(j, {}, "B").contains("name=\"language\""),
		   "no language field when detecting");
		const auto langs = SubtitleDialog::languages();
		ok(langs.size() >= 10 && langs[0].second.isEmpty() && langs[1].second == QStringLiteral("pt"),
		   "the picker offers Detect first, then Portuguese");
	}

	std::printf("\n-- other speech services --\n");
	{
		const auto &all = speechProviders();
		ok(all.size() == 5 && all[0].provider == SpeechProvider::OpenAI, "five services, OpenAI first");
		bool linked = true;
		for (const SpeechProviderInfo &i : all)
			linked = linked && QString::fromLatin1(i.keyUrl).startsWith(QLatin1String("https://")) && *i.name && *i.id;
		ok(linked, "each has a name, an id and an https link to get a key");
		ok(speechProviderFromId(QStringLiteral("deepgram")) == SpeechProvider::Deepgram &&
			   speechProviderFromId(QStringLiteral("nonsense")) == SpeechProvider::OpenAI &&
			   speechProviderFromId(QString()) == SpeechProvider::OpenAI,
		   "ids round-trip; unknown or missing falls back to OpenAI");
		ok(SecretStore::settingsName(SpeechProvider::OpenAI) == QStringLiteral("subtitles/apiKey") &&
			   SecretStore::settingsName(SpeechProvider::Groq) == QStringLiteral("subtitles/apiKey_groq"),
		   "a key saved before other services existed is still OpenAI's; the others get their own");

		// Groq: OpenAI's request with its own model.
		SpeechJob g;
		g.provider = SpeechProvider::Groq;
		g.language = QStringLiteral("pt");
		const QByteArray gb = SpeechTranscriber::multipartBody(g, "RIFF", "B");
		ok(gb.contains("name=\"model\"\r\n\r\nwhisper-large-v3-turbo") && gb.contains("timestamp_granularities[]\"\r\n\r\nword"),
		   "Groq: the OpenAI request with whisper-large-v3-turbo and word timestamps");

		// Where the key goes.
		const QUrl u(QStringLiteral("https://example.test/"));
		ok(SpeechTranscriber::authorized(SpeechProvider::Groq, u, QStringLiteral(" k1 ")).rawHeader("Authorization") == "Bearer k1" &&
			   SpeechTranscriber::authorized(SpeechProvider::Deepgram, u, QStringLiteral("k2")).rawHeader("Authorization") == "Token k2" &&
			   SpeechTranscriber::authorized(SpeechProvider::AssemblyAI, u, QStringLiteral("k3")).rawHeader("Authorization") == "k3" &&
			   SpeechTranscriber::authorized(SpeechProvider::ElevenLabs, u, QStringLiteral("k4")).rawHeader("xi-api-key") == "k4" &&
			   !SpeechTranscriber::authorized(SpeechProvider::ElevenLabs, u, QStringLiteral("k4")).hasRawHeader("Authorization"),
		   "each service gets the key in the header it expects, trimmed");

		// Deepgram: options in the query.
		SpeechJob d;
		d.provider = SpeechProvider::Deepgram;
		d.language = QStringLiteral("pt");
		const QUrlQuery dq(deepgramUrl(d));
		ok(deepgramUrl(d).host() == QStringLiteral("api.deepgram.com") && dq.queryItemValue(QStringLiteral("model")) == QStringLiteral("nova-3") &&
			   dq.queryItemValue(QStringLiteral("language")) == QStringLiteral("pt") && !dq.hasQueryItem(QStringLiteral("detect_language")) &&
			   dq.queryItemValue(QStringLiteral("smart_format")) == QStringLiteral("true"),
		   "Deepgram: nova-3, the language, punctuation, in the query");
		d.language.clear();
		ok(QUrlQuery(deepgramUrl(d)).queryItemValue(QStringLiteral("detect_language")) == QStringLiteral("true") &&
			   !QUrlQuery(deepgramUrl(d)).hasQueryItem(QStringLiteral("language")),
		   "Deepgram: detect_language when detecting");
		QString err;
		const Transcript dt = parseDeepgramJson(R"({"results":{"channels":[{"detected_language":"pt","alternatives":[{"transcript":"olá mundo",
			"words":[{"word":"mundo","start":0.9,"end":1.3,"punctuated_word":"mundo."},{"word":"olá","start":0.25,"end":0.6,"punctuated_word":"Olá,"}]}]}]}})", &err);
		ok(dt.words.size() == 2 && dt.words[0].text == QStringLiteral("Olá,") && dt.words[0].startMs == 250 &&
			   dt.words[1].endMs == 1300 && dt.language == QStringLiteral("pt") && err.isEmpty(),
		   "Deepgram reply: punctuated words, seconds to ms, sorted, detected language");
		err.clear();
		parseDeepgramJson(R"({"err_code":"INVALID_AUTH","err_msg":"Invalid credentials.","request_id":"x"})", &err);
		ok(err == QStringLiteral("Invalid credentials."), "Deepgram error body: its own message");
		ok(speechErrorMessage(SpeechProvider::Deepgram, R"({"category":"INVALID_AUTH","message":"Bad key"})") == QStringLiteral("Bad key"),
		   "and the newer error shape too");

		// AssemblyAI: upload -> create -> poll.
		ok(parseAssemblyAiUploadUrl(R"({"upload_url":"https://cdn.assemblyai.com/upload/abc"})") ==
			   QStringLiteral("https://cdn.assemblyai.com/upload/abc"),
		   "AssemblyAI: where the upload went");
		const QJsonObject cb = QJsonDocument::fromJson(assemblyAiCreateBody(QStringLiteral("https://u"), QStringLiteral("pt"))).object();
		ok(cb.value(QStringLiteral("audio_url")).toString() == QStringLiteral("https://u") &&
			   cb.value(QStringLiteral("language_code")).toString() == QStringLiteral("pt") && !cb.contains(QStringLiteral("language_detection")),
		   "AssemblyAI job: the audio and the language");
		ok(QJsonDocument::fromJson(assemblyAiCreateBody(QStringLiteral("https://u"), QString())).object().value(QStringLiteral("language_detection")).toBool(),
		   "AssemblyAI job: language detection when detecting");
		const AssemblyAiStatus st = parseAssemblyAiStatus(R"({"id":"t1","status":"processing"})");
		ok(st.id == QStringLiteral("t1") && st.status == QStringLiteral("processing"), "AssemblyAI status: id and state");
		err.clear();
		const Transcript at = parseAssemblyAiJson(R"({"id":"t1","status":"completed","language_code":"en","text":"Hi there.",
			"words":[{"text":"Hi","start":120,"end":400},{"text":"there.","start":450,"end":800}]})", &err);
		ok(at.words.size() == 2 && at.words[0].startMs == 120 && at.words[1].text == QStringLiteral("there.") && at.language == QStringLiteral("en"),
		   "AssemblyAI reply: words already in ms");
		err.clear();
		parseAssemblyAiJson(R"({"id":"t1","status":"error","error":"Audio file has no speech"})", &err);
		ok(err == QStringLiteral("Audio file has no speech"), "AssemblyAI failed job: its reason");

		// ElevenLabs Scribe.
		SpeechJob e;
		e.provider = SpeechProvider::ElevenLabs;
		e.language = QStringLiteral("es");
		e.apiKey = QStringLiteral("sk_secret");
		const QByteArray eb = elevenLabsBody(e, "RIFF", "B");
		ok(eb.contains("name=\"model_id\"\r\n\r\nscribe_v1") && eb.contains("name=\"timestamps_granularity\"\r\n\r\nword") &&
			   eb.contains("name=\"language_code\"\r\n\r\nes") && eb.contains("filename=\"speech.wav\"") && !eb.contains("sk_secret"),
		   "ElevenLabs: scribe_v1, word timestamps, the language and the file, no key");
		err.clear();
		const Transcript et = parseElevenLabsJson(R"J({"language_code":"spa","text":"Hola amigo","words":[
			{"text":"Hola","start":0.1,"end":0.4,"type":"word"},{"text":" ","start":0.4,"end":0.5,"type":"spacing"},
			{"text":"(risas)","start":0.5,"end":0.7,"type":"audio_event"},{"text":"amigo","start":0.7,"end":1.1,"type":"word"}]})J", &err);
		ok(et.words.size() == 2 && et.words[1].text == QStringLiteral("amigo") && et.words[1].startMs == 700,
		   "ElevenLabs reply: only the words, not spacing or audio events");
		err.clear();
		parseElevenLabsJson(R"({"detail":{"status":"invalid_api_key","message":"Invalid API key"}})", &err);
		ok(err == QStringLiteral("Invalid API key"), "ElevenLabs error body: its own message");
		ok(speechErrorMessage(SpeechProvider::ElevenLabs, R"({"detail":[{"loc":["body","file"],"msg":"field required"}]})") ==
			   QStringLiteral("field required"),
		   "and a validation error list");
		ok(speechErrorMessage(SpeechProvider::Groq, R"({"error":{"message":"Invalid API Key"}})") == QStringLiteral("Invalid API Key") &&
			   speechErrorMessage(SpeechProvider::AssemblyAI, "<html>") .isEmpty(),
		   "Groq's error shape; a non-JSON body has no service message");
		ok(parseSpeechReply(SpeechProvider::AssemblyAI, R"({"status":"completed","words":[{"text":"a","start":1,"end":2}]})").words.size() == 1 &&
			   parseSpeechReply(SpeechProvider::Groq, R"({"words":[{"word":"a","start":0.001,"end":0.002}]})").words.size() == 1,
		   "the final reply goes to each service's own parser");
	}

	std::printf("\n-- key links and the preview --\n");
	{
		const QString html = speechKeyLinksHtml();
		bool every = true;
		for (const SpeechProviderInfo &i : speechProviders())
			every = every && html.contains(QString::fromLatin1(i.keyUrl));
		ok(every && html.count(QStringLiteral("<a ")) == 5, "one link per service to its key page");

		GroupRule r;
		r.maxWords = 3;
		r.maxChars = 100;
		const QString sent = QStringLiteral("one two three four five");
		ok(SubtitleDialog::previewText(sent, r, SubtitleMode::Whole, 0) == QStringLiteral("one two three") &&
			   SubtitleDialog::previewText(sent, r, SubtitleMode::Whole, 2) == QStringLiteral("one two three") &&
			   SubtitleDialog::previewText(sent, r, SubtitleMode::Whole, 3) == QStringLiteral("four five"),
		   "whole phrase: the caption the current word belongs to, grouped by the rules");
		ok(SubtitleDialog::previewText(sent, r, SubtitleMode::OneWord, 4) == QStringLiteral("five") &&
			   SubtitleDialog::previewText(sent, r, SubtitleMode::BuildUp, 1) == QStringLiteral("one two") &&
			   SubtitleDialog::previewText(sent, r, SubtitleMode::BuildUp, 3) == QStringLiteral("four"),
		   "one word: that word; build up: the caption's words so far");
		ok(SubtitleDialog::previewText(sent, r, SubtitleMode::Whole, 5) == QStringLiteral("one two three"),
		   "and it loops");
		ok(SubtitleDialog::sampleSentence(QStringLiteral("pt")).contains(QStringLiteral("legendas")) &&
			   SubtitleDialog::sampleSentence(QString()).startsWith(QStringLiteral("This")),
		   "the sample follows the language");

		SubtitleLook look;
		look.style.fontPx = 120;
		look.style.color = Qt::white;
		QImage bg(1920, 1080, QImage::Format_ARGB32);
		bg.fill(Qt::black);
		const QImage bottom = SubtitleDialog::renderPreview(bg, QSize(1920, 1080), look, QStringLiteral("HELLO"), QSize(300, 340));
		ok(bottom.width() == 300 && std::abs(bottom.height() - 169) <= 1, "a 16:9 canvas previews at 16:9 inside the box");
		const auto whiteRows = [](const QImage &im, int y0, int y1) {
			int n = 0;
			for (int y = y0; y < y1; ++y)
				for (int x = 0; x < im.width(); ++x)
					if (qGray(im.pixel(x, y)) > 200)
						++n;
			return n;
		};
		const int h = bottom.height();
		ok(whiteRows(bottom, h * 2 / 3, h) > 20 && whiteRows(bottom, 0, h / 3) == 0,
		   "Bottom draws the text in the lower third, nothing up top");
		look.position = SubtitleLook::Position::Top;
		const QImage top = SubtitleDialog::renderPreview(bg, QSize(1920, 1080), look, QStringLiteral("HELLO"), QSize(300, 340));
		ok(whiteRows(top, 0, h / 3) > 20 && whiteRows(top, h * 2 / 3, h) == 0, "Top moves it up");
		const QImage tall = SubtitleDialog::renderPreview(QImage(), QSize(1080, 1920), look, QString(), QSize(300, 340));
		ok(tall.height() == 340 && tall.width() < 200, "a vertical canvas previews tall, with no frame yet");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
