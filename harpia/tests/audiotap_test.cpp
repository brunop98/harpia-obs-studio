// Audio Only recording: the tap's bookkeeping, the scratch file it streams
// into, and the encoder both it and "Extract Audio Only" go through.
//
// This mode has no picture, so every one of its failure modes is silent. A
// recording that is thirty seconds of nothing, a WAV header no decoder will
// open, a pause that shifts the sound out of time -- all of them look exactly
// like success until someone plays the file back. So the checks here play the
// files back: two of them write real audio, read it in again through FFmpeg,
// and compare it with what went in.
//
// The libobs half (obs_add_raw_audio_callback and the writer thread) is not
// covered -- there is no libobs in this container -- but everything that
// decides what lands in the file is.
#include "core/AudioFileWriter.hpp"
#include "library/AudioCard.hpp"
#include "library/ClipLibrary.hpp"
#include "core/AudioTap.hpp"
#include "editor/AudioExtract.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static uint32_t readU32(const std::array<uint8_t, kWavHeaderBytes> &h, size_t at)
{
	return uint32_t(h[at]) | (uint32_t(h[at + 1]) << 8) | (uint32_t(h[at + 2]) << 16) |
	       (uint32_t(h[at + 3]) << 24);
}
static uint16_t readU16(const std::array<uint8_t, kWavHeaderBytes> &h, size_t at)
{
	return uint16_t(uint16_t(h[at]) | (uint16_t(h[at + 1]) << 8));
}
static bool tagAt(const std::array<uint8_t, kWavHeaderBytes> &h, size_t at, const char *s)
{
	return std::memcmp(h.data() + at, s, 4) == 0;
}

// A 440 Hz tone, the same in both channels, so a channel mix-up shows up as a
// difference rather than as nothing.
static std::vector<float> tone(int rate, int channels, int ms, float amp = 0.5f)
{
	const int frames = rate * ms / 1000;
	std::vector<float> pcm(size_t(frames) * size_t(channels));
	for (int f = 0; f < frames; ++f) {
		const float s = amp * std::sin(2.0 * M_PI * 440.0 * f / rate);
		for (int c = 0; c < channels; ++c)
			pcm[size_t(f) * channels + c] = s;
	}
	return pcm;
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir tmp;
	if (!tmp.isValid()) {
		std::printf("  FAIL could not make a temp directory\n");
		return 1;
	}
	const int rate = 48000;
	const int channels = 2;

	std::printf("\n-- what the tap does with a buffer --\n");
	{
		AudioTapState t;
		t.queueCapFrames = 1000;
		ok(t.offer(480, 0) == 480, "an ordinary buffer is taken whole");
		ok(t.frames == 480, "and counted");

		t.paused = true;
		ok(t.offer(480, 480) == 0, "a paused buffer is refused");
		ok(t.frames == 480, "and does not lengthen the file");
		// The distinction that matters: a pause is the user's choice and a
		// drop is a fault. Counting them together would put a warning in the
		// log every time someone pressed Pause.
		ok(t.dropped == 0, "a pause is not a dropped frame");

		t.paused = false;
		ok(t.offer(200, 480) == 200 && t.frames == 680,
		   "resuming carries on from where it left off rather than restarting");
	}

	std::printf("\n-- when the disk cannot keep up --\n");
	{
		AudioTapState t;
		t.queueCapFrames = 1000;
		ok(t.offer(400, 900) == 0, "a buffer that would burst the queue is dropped");
		ok(t.dropped == 400, "and counted as a fault, not as a pause");
		ok(t.frames == 0, "a dropped buffer is not in the file, so it is not in the length");
		ok(t.offer(100, 900) == 100, "while one that still fits is taken");

		// CONTROL: without a cap the same buffer sails through, so the check
		// above is measuring the cap and not something incidental.
		AudioTapState u;
		ok(u.offer(400, 9000000) == 400 && u.dropped == 0,
		   "CONTROL: with no cap set, nothing is ever dropped");
	}

	std::printf("\n-- how long the recording is --\n");
	{
		AudioTapState t;
		t.offer(48000, 0);
		ok(t.durationMs(48000) == 1000, "48000 frames at 48 kHz is one second");
		t.offer(24000, 0);
		ok(t.durationMs(48000) == 1500, "and it accumulates");
		ok(t.durationMs(0) == 0, "an impossible rate reports nothing rather than dividing by it");
	}

	std::printf("\n-- the scratch file's header --\n");
	{
		const auto h = buildWavHeaderFloat32(48000, 2, 800);
		ok(tagAt(h, 0, "RIFF") && tagAt(h, 8, "WAVE") && tagAt(h, 12, "fmt ") &&
			   tagAt(h, 38, "fact") && tagAt(h, 50, "data"),
		   "the four chunks are where the layout says they are");
		ok(readU16(h, 20) == 3, "the format tag says IEEE float, not PCM");
		ok(readU16(h, 34) == 32 && readU16(h, 32) == 8,
		   "32 bits a sample, 8 bytes a stereo frame");
		ok(readU32(h, 24) == 48000 && readU32(h, 28) == 48000 * 8,
		   "the rate and the byte rate agree with each other");
		ok(readU32(h, kWavDataSizeOffset) == 800, "data is the length it was given");
		ok(readU32(h, kWavRiffSizeOffset) == kWavHeaderBytes - 8 + 800,
		   "and RIFF covers everything after its own length field");
		ok(readU32(h, kWavFactFramesOffset) == 100, "fact counts frames, not bytes");

		// The header is written twice -- once empty at open, once with the real
		// length at close -- and the second write must land on exactly the same
		// bytes as the first except for the three length fields.
		const auto empty = buildWavHeaderFloat32(48000, 2, 0);
		int differing = 0;
		for (size_t i = 0; i < kWavHeaderBytes; ++i)
			if (empty[i] != h[i])
				++differing;
		ok(differing > 0 && differing <= 12,
		   "patching the length at close touches only the three length fields");

		// A 32-bit field cannot describe 5 GB. Saturating is recoverable;
		// wrapping to a small number tells the decoder to stop early.
		const auto huge = buildWavHeaderFloat32(48000, 2, 5ull * 1024 * 1024 * 1024);
		ok(readU32(huge, kWavDataSizeOffset) > 0xFFFFFF00u,
		   "a file past 4 GB saturates the length rather than wrapping it");
	}

	std::printf("\n-- the scratch file, read back by FFmpeg --\n");
	{
		// The reason this is worth doing at all: a hand-written float WAV that
		// no decoder will open is invisible until the encode step fails, and by
		// then it is the only copy of the recording.
		const std::vector<float> src = tone(rate, channels, 250);
		const uint64_t dataBytes = src.size() * sizeof(float);
		const QString path = QDir(tmp.path()).filePath(QStringLiteral("scratch.wav"));
		QFile f(path);
		f.open(QIODevice::WriteOnly);
		const auto h = buildWavHeaderFloat32(rate, channels, dataBytes);
		f.write(reinterpret_cast<const char *>(h.data()), qint64(h.size()));
		f.write(reinterpret_cast<const char *>(src.data()), qint64(dataBytes));
		f.close();

		QString err;
		const std::vector<float> back = decodeAudioToPcm(path, rate, channels, &err);
		ok(!back.empty(), "FFmpeg opens the file the tap writes");
		ok(back.size() == src.size(), "and gets back exactly the frames that went in");
		double worst = 0.0;
		for (size_t i = 0; i < std::min(back.size(), src.size()); ++i)
			worst = std::max(worst, double(std::abs(back[i] - src[i])));
		ok(worst < 1e-6, "sample for sample, unchanged -- float PCM is not being quantised");
		ok(peakOf(back) > 0.4, "and it is the tone, not silence");
	}

	std::printf("\n-- the encode the recording finishes with --\n");
	{
		const std::vector<float> src = tone(rate, channels, 500);
		const QString path = QDir(tmp.path()).filePath(QStringLiteral("take.m4a"));
		size_t done = 0;
		const auto pull = [&](float *dst, int maxFrames) -> int {
			const size_t total = src.size() / channels;
			const int n = int(std::min<size_t>(size_t(maxFrames), total - done));
			if (n <= 0)
				return 0;
			std::memcpy(dst, src.data() + done * channels,
				    size_t(n) * channels * sizeof(float));
			done += size_t(n);
			return n;
		};
		std::string why;
		const bool wrote = encodeAudioStream(path.toStdString(), "aac", pull, rate, channels, 160,
						     &why);
		ok(wrote, why.empty() ? "a pulled stream encodes to M4A" : why.c_str());
		ok(QFile::exists(path) && QFileInfo(path).size() > 1000, "and the file has content in it");

		QString err;
		const std::vector<float> back = decodeAudioToPcm(path, rate, channels, &err);
		const qint64 ms = back.empty() ? 0 : qint64(back.size() / channels) * 1000 / rate;
		// AAC pads at both ends, so the length is close rather than exact.
		ok(ms >= 480 && ms <= 620, "playing it back gives half a second of audio");
		ok(peakOf(back) > 0.3, "at roughly the level that went in, so it is not silence");

		// CONTROL: the same call with nothing to pull must not quietly produce
		// a file that looks like a recording. This is the shape of the bug
		// where a take captures nothing and still reports success.
		const QString emptyPath = QDir(tmp.path()).filePath(QStringLiteral("empty.m4a"));
		const auto none = [](float *, int) { return 0; };
		encodeAudioStream(emptyPath.toStdString(), "aac", none, rate, channels, 160, &why);
		const std::vector<float> nothing = decodeAudioToPcm(emptyPath, rate, channels, &err);
		ok(nothing.size() < back.size() / 4,
		   "CONTROL: an empty stream does not produce half a second of anything");
	}

	std::printf("\n-- an encoder this build does not have --\n");
	{
		ok(audioEncoderAvailable("aac"), "AAC is always here");
		ok(!audioEncoderAvailable("no_such_encoder"), "and a made-up name is reported missing");
		const QString path = QDir(tmp.path()).filePath(QStringLiteral("nope.m4a"));
		std::string why;
		const auto none = [](float *, int) { return 0; };
		ok(!encodeAudioStream(path.toStdString(), "no_such_encoder", none, rate, channels, 160,
				      &why),
		   "asking for one fails");
		ok(!why.empty(), "with a reason to show the user");
		ok(!QFile::exists(path), "and leaves no half-written file behind");
	}

	std::printf("\n-- how an audio recording looks in the library --\n");
	{
		// There is no frame to show, so the card draws a shape derived from the
		// path. What matters is that it is STABLE -- a recording that looked
		// different every time the strip repainted, or after a restart, would
		// read as a different file.
		const QString a = QStringLiteral("/recordings/Take 1.m4a");
		const QString b = QStringLiteral("/recordings/Take 2.m4a");
		const QVector<float> barsA = audioCardBars(a, 34);
		ok(barsA.size() == 34, "the card asks for bars and gets that many");
		ok(barsA == audioCardBars(a, 34), "the same recording draws the same shape every time");
		ok(barsA != audioCardBars(b, 34),
		   "and two recordings in a row do not look like copies of each other");

		bool inRange = true;
		for (float v : barsA)
			if (v < 0.15f || v > 1.0f)
				inRange = false;
		// The floor is not cosmetic: a zero-height bar reads as a gap in the
		// recording, which this function is in no position to claim.
		ok(inRange, "every bar is between the floor and full height");

		ok(audioCardBars(a, 0).isEmpty(), "asking for no bars gives none rather than crashing");
		ok(audioCardBars(QString(), 8).size() == 8, "and an empty path still draws something");
	}

	std::printf("\n-- which files count as recordings --\n");
	{
		ok(ClipLibrary::isAudioPath(QStringLiteral("/x/take.m4a")), "an m4a is audio");
		// The .wav matters as much as the m4a: it is what a failed encode
		// leaves behind, and the whole reason for keeping it was that the user
		// still has their recording. A library that hid it would undo that.
		ok(ClipLibrary::isAudioPath(QStringLiteral("/x/take.m4a.part.wav")),
		   "so is the WAV a failed encode leaves behind");
		ok(ClipLibrary::isAudioPath(QStringLiteral("/x/TAKE.M4A")), "case does not matter");
		ok(!ClipLibrary::isAudioPath(QStringLiteral("/x/take.mp4")), "an mp4 is not");
		ok(!ClipLibrary::isAudioPath(QStringLiteral("/x/m4a")), "and neither is a file merely named m4a");
		ok(ClipLibrary::recordingExtensions().contains(QStringLiteral("mp4")) &&
			   ClipLibrary::recordingExtensions().contains(QStringLiteral("m4a")),
		   "the scan looks for both kinds");
	}

	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "all audio-tap checks passed",
		    failures);
	return failures ? 1 : 0;
}
