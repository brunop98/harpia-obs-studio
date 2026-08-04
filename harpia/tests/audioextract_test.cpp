// Extract Audio Only: the edit arithmetic.
//
// All of it operates on a buffer of samples, where a mistake is inaudible in
// code review and obvious in the exported file. The ones worth pinning:
//
//   * a trim computed in SAMPLES rather than FRAMES lands half a frame off on
//     stereo, and the channels stay swapped for the rest of the export;
//   * fades applied before the trim ramp the ends of the ORIGINAL file, so a
//     clip cut out of the middle gets no fade at all and starts with a click;
//   * two fades longer than the clip multiply into a notch and the middle goes
//     silent;
//   * a normalise on silence divides by nothing;
//   * and gain past full scale wraps into crackle unless something limits it.
//
// Pure: no libav, no file, no window.
#include "editor/AudioExtract.hpp"

#include <cmath>
#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static constexpr int kRate = 48000;
static constexpr int kCh = 2;

// A buffer where every frame is identifiable: left = frame index, right = its
// negative. Any frame-vs-sample slip shows up as a left channel holding a
// negative number.
static std::vector<float> rampBuffer(int frames)
{
	std::vector<float> pcm(size_t(frames) * kCh);
	for (int f = 0; f < frames; ++f) {
		pcm[size_t(f * kCh)] = float(f);
		pcm[size_t(f * kCh + 1)] = float(-f);
	}
	return pcm;
}

static std::vector<float> constBuffer(int frames, float v)
{
	return std::vector<float>(size_t(frames) * kCh, v);
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	std::printf("\n-- what gets kept --\n");
	{
		AudioEdit e;
		qint64 s = 0, en = 0;
		resolveTrim(e, 10000, &s, &en);
		ok(s == 0 && en == 10000, "an untouched edit keeps the whole file");

		e.trimStartMs = 2000;
		e.trimEndMs = 6000;
		resolveTrim(e, 10000, &s, &en);
		ok(s == 2000 && en == 6000, "dragged handles are honoured");

		// A backwards window is "to the end", not an empty export.
		e.trimStartMs = 6000;
		e.trimEndMs = 2000;
		resolveTrim(e, 10000, &s, &en);
		ok(s == 6000 && en == 10000, "a backwards window reads as 'to the end'");

		// Past the end of a file that turned out shorter than the window.
		e.trimStartMs = 1000;
		e.trimEndMs = 99999;
		resolveTrim(e, 10000, &s, &en);
		ok(en == 10000, "an end past the file is clamped to the file");

		e.trimStartMs = 99999;
		e.trimEndMs = 0;
		resolveTrim(e, 10000, &s, &en);
		ok(s == 10000 && en == 10000, "a start past the end gives an empty window, not a negative one");
	}

	std::printf("\n-- how long the export will be --\n");
	{
		AudioEdit e;
		e.trimStartMs = 2000;
		e.trimEndMs = 6000;
		ok(extractDurationMs(e, 10000) == 4000, "the kept span, at normal speed");
		e.speed = 2.0;
		ok(extractDurationMs(e, 10000) == 2000, "twice as fast is half as long");
		e.speed = 0.5;
		ok(extractDurationMs(e, 10000) == 8000, "and half speed is twice as long");
		e.speed = 99.0;
		ok(extractDurationMs(e, 10000) ==
			   qint64(std::llround(4000 / AudioEdit::kMaxSpeed)),
		   "a silly speed is clamped rather than producing a 40 ms file");
	}

	std::printf("\n-- the trim is frame-accurate --\n");
	{
		const std::vector<float> src = rampBuffer(kRate); // exactly 1 s
		AudioEdit e;
		e.trimStartMs = 250;
		e.trimEndMs = 750;
		const std::vector<float> cut = trimPcm(src, kCh, kRate, e);
		std::printf("     kept %zu samples = %zu frames\n", cut.size(), cut.size() / kCh);
		ok(cut.size() == size_t(kRate / 2) * kCh, "half a second of frames came back");
		ok(cut.size() % kCh == 0, "a whole number of frames — no half-frame slip");
		// THE check: left is still the positive channel. Off by one sample and
		// every left value here would be negative.
		ok(cut[0] > 0 && cut[1] < 0, "left is still left after the cut");
		ok(std::abs(cut[0] - float(kRate / 4)) < 1.5f,
		   "and it starts at the frame 250 ms in, not somewhere near it");

		// An empty window produces nothing rather than a wrapped-around buffer.
		AudioEdit empty;
		empty.trimStartMs = 900;
		empty.trimEndMs = 900;
		ok(trimPcm(src, kCh, kRate, empty).empty() ||
			   trimPcm(src, kCh, kRate, empty).size() % kCh == 0,
		   "a zero-length window is empty, or at least whole frames");
	}

	std::printf("\n-- fades land on the KEPT part --\n");
	{
		// The bug this exists for: fading before trimming ramps the original
		// file's ends, so a clip cut from the middle gets full volume at both
		// ends and clicks.
		const std::vector<float> src = constBuffer(kRate * 4, 1.0f); // 4 s of full scale
		AudioEdit e;
		e.trimStartMs = 1000; // cut a second out of the MIDDLE
		e.trimEndMs = 3000;
		e.fadeInMs = 200;
		e.fadeOutMs = 200;
		const std::vector<float> out = applyAudioEdit(src, kCh, kRate, e);
		ok(!out.empty(), "something came out");
		std::printf("     first sample %.4f, middle %.4f, last %.4f\n", out.front(),
			    out[out.size() / 2], out.back());
		ok(std::abs(out.front()) < 0.02f, "the kept part starts near silence");
		ok(std::abs(out.back()) < 0.02f, "and ends near silence");
		ok(out[out.size() / 2] > 0.98f, "with the middle untouched at full level");

		// CONTROL: with no fades the same cut starts and ends at full scale, so
		// the checks above measure the fade rather than the trim.
		AudioEdit noFade = e;
		noFade.fadeInMs = noFade.fadeOutMs = 0;
		const std::vector<float> flat = applyAudioEdit(src, kCh, kRate, noFade);
		ok(flat.front() > 0.98f && flat.back() > 0.98f,
		   "CONTROL: without fades it starts and ends at full level");
	}

	std::printf("\n-- fades longer than the clip --\n");
	{
		// Two 5-second ramps on a 1-second clip. Multiplied naively the middle
		// collapses; they should meet instead.
		const std::vector<float> src = constBuffer(kRate, 1.0f); // 1 s
		AudioEdit e;
		e.fadeInMs = 5000;
		e.fadeOutMs = 5000;
		const std::vector<float> out = applyAudioEdit(src, kCh, kRate, e);
		const float mid = out[out.size() / 2];
		std::printf("     midpoint of an over-faded clip: %.3f\n", mid);
		ok(mid > 0.3f, "it fades up and straight back down rather than vanishing");
		ok(out.front() < 0.05f && out.back() < 0.05f, "still silent at both ends");
	}

	std::printf("\n-- gain and normalise --\n");
	{
		std::vector<float> quiet = constBuffer(1000, 0.1f);
		const double g = normaliseGainFor(quiet);
		std::printf("     a 0.1 peak needs %.2fx (%.1f dB)\n", g, gainToDb(g));
		ok(g > 8.0 || std::abs(g - AudioEdit::kMaxGain) < 1e-9,
		   "a quiet recording gets a big boost");
		ok(0.1 * g < 1.0, "but not past full scale");

		// Silence: the divide-by-nothing case.
		std::vector<float> silence = constBuffer(1000, 0.0f);
		ok(std::abs(normaliseGainFor(silence) - 1.0) < 1e-9,
		   "silence normalises to 1.0, not to infinity");

		// Already loud: leaves headroom rather than pushing to exactly 1.0,
		// because a lossy encoder reconstructs slightly high and clips.
		std::vector<float> loud = constBuffer(1000, 1.0f);
		ok(normaliseGainFor(loud) < 1.0, "an already-full recording is pulled DOWN for headroom");

		ok(std::abs(dbToGain(0.0) - 1.0) < 1e-9, "0 dB is unity");
		ok(std::abs(dbToGain(6.0) - 2.0) < 0.01, "+6 dB is about double");
		ok(std::abs(gainToDb(dbToGain(-13.5)) + 13.5) < 1e-6, "dB and gain round-trip");
	}

	std::printf("\n-- nothing leaves the buffer clipping --\n");
	{
		// +18 dB on a loud recording. Handed to an encoder unclamped, samples
		// past full scale wrap into crackle instead of clipping politely.
		const std::vector<float> src = constBuffer(1000, 0.9f);
		AudioEdit e;
		e.gain = 8.0;
		const std::vector<float> out = applyAudioEdit(src, kCh, kRate, e);
		ok(peakOf(out) <= 1.0 + 1e-6, "the output is limited to full scale");
		ok(out[out.size() / 2] > 0.99f, "and the loud part really is at the ceiling");

		// CONTROL: the unlimited product would have been well past 1.
		ok(0.9 * 8.0 > 1.0, "CONTROL: unlimited, that gain would have overshot");
	}

	std::printf("\n-- degenerate input --\n");
	{
		AudioEdit e;
		ok(applyAudioEdit({}, kCh, kRate, e).empty(), "an empty buffer stays empty");
		ok(applyAudioEdit(constBuffer(10, 0.5f), 0, kRate, e).empty(), "zero channels is refused");
		ok(applyAudioEdit(constBuffer(10, 0.5f), kCh, 0, e).empty(), "so is a zero sample rate");
		std::vector<float> mono(1000, 0.5f);
		AudioEdit m;
		m.fadeInMs = 5;
		applyGainAndFades(mono, 1, kRate, m);
		ok(mono.front() < 0.5f && mono.back() > 0.49f, "mono works too, not just stereo");
	}

	std::printf("\n-- format names --\n");
	{
		ok(QString(audioFormatExtension(AudioFormat::Mp3)) == QStringLiteral("mp3") &&
			   QString(audioFormatExtension(AudioFormat::M4a)) == QStringLiteral("m4a") &&
			   QString(audioFormatExtension(AudioFormat::Wav)) == QStringLiteral("wav"),
		   "every format knows its extension");
	}

	std::printf("\n%s (%d failures)\n",
		    failures ? "FAILURES" : "all audio-extract checks passed", failures);
	return failures ? 1 : 0;
}
