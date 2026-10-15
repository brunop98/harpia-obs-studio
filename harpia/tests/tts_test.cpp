// Text to speech, the pure half (editor/tts): what each service is sent, how
// its reply becomes audio and word times, how a long text is cut, how written
// words are matched to heard ones, how the pieces' audio is joined, and where
// the narration and its captions land -- linked.
#include "editor/tts/SpeechPlacement.hpp"
#include "editor/tts/TtsAudio.hpp"
#include "editor/tts/TtsProviders.hpp"

#include <QCoreApplication>
#include <QDir>
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

static QJsonObject json(const QByteArray &b)
{
	return QJsonDocument::fromJson(b).object();
}

static QByteArray header(const QVector<QPair<QByteArray, QByteArray>> &h, const QByteArray &name)
{
	for (const auto &p : h)
		if (p.first == name)
			return p.second;
	return QByteArray();
}

// A WAV of `ms` of silence, 24 kHz mono 16-bit, as Google and Deepgram send.
static QByteArray silentWav(int ms)
{
	tts_audio::Pcm p;
	p.rate = 24000;
	p.channels = 1;
	p.samples.resize(24 * ms);
	QTemporaryDir d;
	const QString path = d.filePath(QStringLiteral("s.wav"));
	tts_audio::writeWav16(path, p);
	QFile f(path);
	f.open(QIODevice::ReadOnly);
	return f.readAll();
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	std::printf("\n-- the services --\n");
	ok(ttsProviders().size() == 4, "OpenAI, ElevenLabs, Google, Deepgram");
	ok(ttsProviderFromId(QStringLiteral("google")) == TtsProvider::Google &&
		   ttsProviderFromId(QStringLiteral("nonsense")) == TtsProvider::OpenAI,
	   "ids, unknown falls back to OpenAI");
	ok(header(ttsHeaders(TtsProvider::OpenAI, QStringLiteral(" sk-1 ")), "Authorization") == "Bearer sk-1" &&
		   header(ttsHeaders(TtsProvider::ElevenLabs, QStringLiteral("k")), "xi-api-key") == "k" &&
		   header(ttsHeaders(TtsProvider::Google, QStringLiteral("AIza")), "x-goog-api-key") == "AIza" &&
		   header(ttsHeaders(TtsProvider::Deepgram, QStringLiteral("d")), "Authorization") == "Token d",
	   "the key goes where each service expects it (and only in a header)");

	std::printf("\n-- requests --\n");
	{
		TtsRequest r;
		r.text = QStringLiteral("Olá, mundo.");
		r.voice = QStringLiteral("marin");
		r.style = QStringLiteral("calm");
		r.apiKey = QStringLiteral("sk-secret");
		const QByteArray body = ttsBody(r);
		const QJsonObject o = json(body);
		ok(ttsUrl(r).toString() == QLatin1String("https://api.openai.com/v1/audio/speech"), "OpenAI: the speech endpoint");
		ok(o.value(QStringLiteral("model")).toString() == QLatin1String("gpt-4o-mini-tts") &&
			   o.value(QStringLiteral("input")).toString() == r.text &&
			   o.value(QStringLiteral("voice")).toString() == QLatin1String("marin") &&
			   o.value(QStringLiteral("response_format")).toString() == QLatin1String("wav") &&
			   o.value(QStringLiteral("instructions")).toString() == QLatin1String("calm"),
		   "OpenAI: model, input, voice, WAV, the style as instructions");
		ok(!body.contains("sk-secret"), "the key is never in the body");

		r.provider = TtsProvider::ElevenLabs;
		r.voice = QStringLiteral("21m00Tcm4TlvDq8ikWAM");
		r.speed = 1.1;
		r.previousText = QStringLiteral("Before.");
		const QJsonObject e = json(ttsBody(r));
		ok(ttsUrl(r).toString().startsWith(
			   QLatin1String("https://api.elevenlabs.io/v1/text-to-speech/21m00Tcm4TlvDq8ikWAM/with-timestamps")),
		   "ElevenLabs: the voice in the path, with timestamps");
		ok(e.value(QStringLiteral("model_id")).toString() == QLatin1String("eleven_multilingual_v2") &&
			   std::abs(e.value(QStringLiteral("voice_settings")).toObject().value(QStringLiteral("speed")).toDouble() - 1.1) < 1e-9 &&
			   e.value(QStringLiteral("previous_text")).toString() == QLatin1String("Before."),
		   "ElevenLabs: model, speed, the text before (for a smooth join)");

		r.provider = TtsProvider::Google;
		r.voice = QStringLiteral("pt-BR-Neural2-A");
		r.text = QStringLiteral("A & B <c>");
		const QJsonObject g = json(ttsBody(r));
		const QString ssml = g.value(QStringLiteral("input")).toObject().value(QStringLiteral("ssml")).toString();
		ok(ttsUrl(r).toString().contains(QLatin1String("v1beta1/text:synthesize")), "Google: v1beta1 (marks)");
		// "&" alone is not a word: it joins "A" (see ttsWords).
		ok(ssml == QLatin1String("<speak><mark name=\"w0\"/>A &amp; <mark name=\"w1\"/>B <mark name=\"w2\"/>&lt;c&gt; </speak>"),
		   "Google: a mark before each word, the text escaped");
		std::printf("     %s\n", qPrintable(ssml));
		ok(g.value(QStringLiteral("voice")).toObject().value(QStringLiteral("languageCode")).toString() ==
				   QLatin1String("pt-BR") &&
			   g.value(QStringLiteral("enableTimePointing")).toArray().first().toString() == QLatin1String("SSML_MARK"),
		   "Google: the locale from the voice name, and time points asked for");

		r.provider = TtsProvider::Deepgram;
		r.voice = QStringLiteral("aura-2-thalia-en");
		const QUrl d = ttsUrl(r);
		ok(d.toString().startsWith(QLatin1String("https://api.deepgram.com/v1/speak")) &&
			   QUrlQuery(d).queryItemValue(QStringLiteral("model")) == QLatin1String("aura-2-thalia-en") &&
			   QUrlQuery(d).queryItemValue(QStringLiteral("container")) == QLatin1String("wav"),
		   "Deepgram: the voice as the model, WAV out");
	}

	std::printf("\n-- the text --\n");
	{
		const QVector<TtsWord> w = ttsWords(QStringLiteral("Hello — world!  Again"));
		ok(w.size() == 3 && w[0].text == QStringLiteral("Hello —") && w[1].text == QLatin1String("world!"),
		   "a dash on its own joins the word before");
		QString long1;
		for (int i = 0; i < 40; ++i)
			long1 += QStringLiteral("Sentence number %1 is here. ").arg(i);
		const QStringList c = ttsChunks(long1, 200);
		bool fits = true, whole = true;
		for (const QString &p : c) {
			fits = fits && p.size() <= 200;
			whole = whole && p.endsWith(QLatin1Char('.'));
		}
		ok(c.size() > 5 && fits, "a long text is cut into pieces under the limit");
		ok(whole, "between sentences");
		ok(c.join(QLatin1Char(' ')).simplified() == long1.simplified(), "nothing lost or reordered");
		const QString run = QStringLiteral("word ").repeated(100).trimmed(); // one 499-char sentence
		const QStringList r2 = ttsChunks(run, 120);
		bool fits2 = true;
		for (const QString &p : r2)
			fits2 = fits2 && p.size() <= 120;
		ok(r2.size() >= 5 && fits2, "one sentence longer than a piece is cut between words");
		const QStringList g = ttsChunks(long1, 1000, googleSsmlBytes);
		bool gfits = true;
		for (const QString &p : g)
			gfits = gfits && googleSsmlBytes(p) <= 1000;
		ok(gfits && g.size() > 1, "Google's pieces measured as SSML bytes, marks included");
	}

	std::printf("\n-- ElevenLabs: words from character times --\n");
	{
		const QString text = QStringLiteral("Hi there, you.");
		QStringList chars;
		QVector<double> st, en;
		for (int i = 0; i < text.size(); ++i) {
			chars << QString(text[i]);
			st << i * 0.1;
			en << i * 0.1 + 0.1;
		}
		QJsonObject al;
		al.insert(QStringLiteral("characters"), QJsonArray::fromStringList(chars));
		QJsonArray sa, ea;
		for (double v : st)
			sa.append(v);
		for (double v : en)
			ea.append(v);
		al.insert(QStringLiteral("character_start_times_seconds"), sa);
		al.insert(QStringLiteral("character_end_times_seconds"), ea);
		QJsonObject o;
		o.insert(QStringLiteral("audio_base64"), QString::fromLatin1(QByteArray("ID3fake").toBase64()));
		o.insert(QStringLiteral("alignment"), al);
		QString err;
		const TtsReply r = parseTtsReply(TtsProvider::ElevenLabs, QJsonDocument(o).toJson(), text, &err);
		ok(err.isEmpty() && r.audio == "ID3fake" && r.ext == QLatin1String("mp3") && r.timed, "audio and times");
		ok(r.words.size() == 3 && r.words[0].text == QLatin1String("Hi") && r.words[0].startMs == 0 &&
			   r.words[0].endMs == 200,
		   "\"Hi\": its two characters, 0-200 ms");
		ok(r.words[1].text == QLatin1String("there,") && r.words[1].startMs == 300 && r.words[1].endMs == 900,
		   "\"there,\": 300-900 ms, punctuation and all");
		ok(r.words[2].startMs == 1000, "\"you.\" from 1000 ms");
	}

	std::printf("\n-- Google: words from marks --\n");
	{
		const QByteArray wav = silentWav(3000);
		ok(wavBytesDurationMs(wav) == 3000, "a WAV's length from its header");
		QJsonObject o;
		o.insert(QStringLiteral("audioContent"), QString::fromLatin1(wav.toBase64()));
		QJsonArray tps;
		for (int i = 0; i < 3; ++i) {
			QJsonObject tp;
			tp.insert(QStringLiteral("markName"), QStringLiteral("w%1").arg(i));
			tp.insert(QStringLiteral("timeSeconds"), 0.2 + i * 0.6);
			tps.append(tp);
		}
		o.insert(QStringLiteral("timepoints"), tps);
		QString err;
		const TtsReply r = parseTtsReply(TtsProvider::Google, QJsonDocument(o).toJson(),
						 QStringLiteral("Um dois três"), &err);
		ok(err.isEmpty() && r.timed && r.ext == QLatin1String("wav") && r.words.size() == 3, "audio and three words");
		ok(r.words[0].startMs == 200 && r.words[1].startMs == 800 && r.words[2].startMs == 1400,
		   "each word at its mark");
		ok(r.words[0].endMs <= 800 && r.words[2].endMs <= 3000 && r.words[2].endMs > 1400,
		   "and ends before the next one (or the audio's end)");
	}

	std::printf("\n-- OpenAI / Deepgram: written words matched to heard ones --\n");
	{
		const QVector<TtsWord> written = ttsWords(QStringLiteral("Olá, você está bem? Em 2024 certamente!"));
		const auto w = [](const char *t, qint64 s) {
			ClipWordTime c;
			c.text = QString::fromUtf8(t);
			c.startMs = s;
			c.endMs = s + 300;
			return c;
		};
		// Heard: no accents, "dois mil e vinte e quatro" for 2024, a typo.
		const QVector<ClipWordTime> heard = {w("ola", 0),     w("voce", 400),  w("esta", 800),
						     w("bem", 1200),  w("em", 1700),   w("dois", 2000),
						     w("mil", 2300),  w("e", 2500),    w("vinte", 2700),
						     w("e", 3000),    w("quatro", 3200), w("certamenti", 3700)};
		QVector<bool> matched;
		QVector<ClipWordTime> got = alignToTranscript(written, heard, &matched);
		ok(matched.size() == 7 && matched[0] && matched[1] && matched[2] && matched[3] && matched[4],
		   "accents and punctuation do not stop a match");
		ok(!matched[5], "\"2024\" (said as words) is not matched...");
		ok(matched[6] && got[6].startMs == 3700, "...\"certamente!\" still is, heard a letter off");
		fillUnmatched(got, matched, 4200);
		ok(got[5].startMs >= got[4].endMs && got[5].endMs <= got[6].startMs && got[5].endMs > got[5].startMs,
		   "and \"2024\" takes the gap between its neighbours");
		ok(got[0].text == QStringLiteral("Olá,"), "the caption keeps the text as written");
	}

	std::printf("\n-- voices and errors --\n");
	{
		const QByteArray el = R"({"voices":[{"voice_id":"abc","name":"Ana","labels":{"accent":"brazilian","gender":"female"}}]})";
		const QVector<TtsVoice> v = parseTtsVoices(TtsProvider::ElevenLabs, el);
		ok(v.size() == 1 && v[0].id == QLatin1String("abc") && v[0].name.contains(QLatin1String("brazilian")),
		   "ElevenLabs: id and a description");
		const QByteArray g = R"({"voices":[{"name":"pt-BR-Chirp3-HD-Aoede","ssmlGender":"FEMALE"},
			{"name":"pt-BR-Wavenet-A","ssmlGender":"FEMALE"},{"name":"pt-BR-Neural2-B","ssmlGender":"MALE"}]})";
		const QVector<TtsVoice> gv = parseTtsVoices(TtsProvider::Google, g);
		ok(gv.size() == 2 && gv[0].id == QLatin1String("pt-BR-Neural2-B"),
		   "Google: Chirp left out (no marks), Neural2 first");
		ok(ttsErrorMessage(TtsProvider::OpenAI, R"({"error":{"message":"bad key"}})") == QLatin1String("bad key") &&
			   ttsErrorMessage(TtsProvider::ElevenLabs, R"({"detail":{"status":"x","message":"quota"}})") ==
				   QLatin1String("quota") &&
			   ttsErrorMessage(TtsProvider::Deepgram, R"({"err_msg":"nope"})") == QLatin1String("nope"),
		   "each service's own error message");
		ok(ttsDefaultVoices(TtsProvider::Google, QStringLiteral("pt")).first().id.startsWith(QLatin1String("pt-BR-")),
		   "Google's default voices follow the language");
	}

	std::printf("\n-- joining the audio --\n");
	{
		tts_audio::Pcm a, b;
		a.samples.fill(1000, 48000 * 2);  // 1 s stereo
		b.samples.fill(-1000, 24000 * 2); // 0.5 s
		QVector<qint64> offs;
		const tts_audio::Pcm j = tts_audio::join({a, b}, 250, &offs);
		ok(offs.size() == 2 && offs[0] == 0 && offs[1] == 1250, "the second piece starts after the first and a breath");
		ok(j.durationMs() == 1750, "1.75 s in all");
		ok(j.samples[48000 * 2 + 10] == 0, "the breath is silence");
		QTemporaryDir d;
		const QString p = d.filePath(QStringLiteral("j.wav"));
		tts_audio::Pcm back;
		ok(tts_audio::writeWav16(p, j) && tts_audio::readWav16(p, back) && back.samples == j.samples &&
			   back.rate == 48000 && back.channels == 2,
		   "written and read back the same");
	}

	std::printf("\n-- on the timeline, linked --\n");
	{
		TimelineModel m;
		TlTrack v;
		v.kind = TlTrack::Kind::Video;
		TlClip footage;
		footage.sourceId = 1;
		footage.srcEndMs = 20000;
		footage.link = 4; // an existing group
		v.clips.append(footage);
		m.tracks.append(v);
		TlClip audio;
		audio.sourceId = 2;
		audio.srcEndMs = 5000;
		audio.outStartMs = 1000;
		QVector<TlClip> caps(3);
		for (int i = 0; i < 3; ++i) {
			caps[i].type = TlClip::Type::Text;
			caps[i].srcEndMs = 1000;
			caps[i].outStartMs = 1000 + i * 1500;
		}
		const SpeechPlacement pl = placeSpeech(m, audio, caps);
		ok(pl.link == 5, "a new link id (not the existing group's)");
		ok(pl.captionTrack == 0 && m.tracks[0].name == QLatin1String("Subtitles"), "captions on a new Subtitles lane on top");
		ok(m.tracks[pl.audioTrack].kind == TlTrack::Kind::Audio && m.tracks[pl.audioTrack].name == QLatin1String("Speech"),
		   "the narration on a new Speech lane");
		ok(m.linkedClips(5).size() == 4 && pl.all().size() == 4, "all four clips linked");
		ok(m.tracks[1].clips[0].link == 4, "the footage's own group untouched");

		// Again, at the same time: the lanes are busy there, so new ones.
		TlClip audio2 = audio;
		const SpeechPlacement p2 = placeSpeech(m, audio2, caps);
		ok(p2.link == 6 && p2.captionTrack == 0 && m.tracks.size() == 5, "busy lanes: two more lanes");
		// Later on the timeline: the existing lanes are free there.
		TlClip audio3 = audio;
		audio3.outStartMs = 30000;
		QVector<TlClip> caps3 = caps;
		for (TlClip &c : caps3)
			c.outStartMs += 29000;
		const int lanes = int(m.tracks.size());
		const SpeechPlacement p3 = placeSpeech(m, audio3, caps3);
		ok(int(m.tracks.size()) == lanes, "free lanes are reused");
		ok(m.linkedClips(p3.link).size() == 4, "and linked");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
