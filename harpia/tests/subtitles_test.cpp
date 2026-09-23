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
#include "editor/subtitles/OpenAiTranscriber.hpp"
#include "editor/subtitles/SubtitleDialog.hpp"
#include "editor/subtitles/Transcript.hpp"
#include "editor/timeline/TimelineJson.hpp"

#include <QCoreApplication>
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
	QCoreApplication app(argc, argv);

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
		OpenAiTranscriber::Job j;
		j.language = QStringLiteral("pt");
		j.apiKey = QStringLiteral("sk-secret");
		const QByteArray body = OpenAiTranscriber::multipartBody(j, QByteArrayLiteral("RIFFxxxx"), "B0UNDARY");
		ok(body.contains("name=\"model\"\r\n\r\nwhisper-1") && body.contains("name=\"response_format\"\r\n\r\nverbose_json") &&
			   body.contains("name=\"timestamp_granularities[]\"\r\n\r\nword") &&
			   body.contains("name=\"language\"\r\n\r\npt") &&
			   body.contains("filename=\"speech.wav\"") && body.contains("RIFFxxxx") && body.endsWith("--B0UNDARY--\r\n"),
		   "model, verbose_json, word timestamps, the language and the file, closed properly");
		ok(!body.contains("sk-secret"), "and the key is never in the body (it goes in the header)");
		j.language.clear();
		ok(!OpenAiTranscriber::multipartBody(j, {}, "B").contains("name=\"language\""),
		   "no language field when detecting");
		const auto langs = SubtitleDialog::languages();
		ok(langs.size() >= 10 && langs[0].second.isEmpty() && langs[1].second == QStringLiteral("pt"),
		   "the picker offers Detect first, then Portuguese");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
