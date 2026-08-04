#pragma once

// "Extract Audio Only": pull the soundtrack out of a recording, trim it, tidy
// it up, and write it somewhere.
//
// The split here is the same one the rest of the app uses. Everything that is
// arithmetic on a buffer of samples -- where the trim lands, what a fade curve
// does, how much gain a normalise needs -- is pure and lives in this header, so
// audioextract_test can check it without libav, a file or a window. The three
// things that genuinely need FFmpeg (decode, time-stretch, encode) are declared
// at the bottom and implemented in the .cpp.
//
// Samples are interleaved 32-bit float throughout, which is what every other
// audio path in the editor already speaks (TimelineAudio::kRate / kChannels).

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <vector>

namespace harpia {

// What the export can write. WAV needs no encoder at all; AAC is always present
// in FFmpeg; MP3 needs libmp3lame, which the bundled build may not carry --
// hence availableAudioFormats(), which asks rather than assumes.
enum class AudioFormat {
	Mp3,
	M4a,
	Wav,
};

inline const char *audioFormatName(AudioFormat f)
{
	switch (f) {
	case AudioFormat::Mp3:
		return "MP3";
	case AudioFormat::M4a:
		return "M4A";
	case AudioFormat::Wav:
		return "WAV";
	}
	return "MP3";
}

inline const char *audioFormatExtension(AudioFormat f)
{
	switch (f) {
	case AudioFormat::Mp3:
		return "mp3";
	case AudioFormat::M4a:
		return "m4a";
	case AudioFormat::Wav:
		return "wav";
	}
	return "mp3";
}

// Everything the window can change about the sound. Plain data, so a test can
// build one inline and so the dialog has nothing to remember.
struct AudioEdit {
	// The part to keep, in SOURCE milliseconds. trimEndMs <= trimStartMs means
	// "to the end of the file" -- the state a freshly-opened window is in
	// before anything has been dragged.
	qint64 trimStartMs = 0;
	qint64 trimEndMs = 0;
	// Ramps at each end of the KEPT part, in ms. These are what stop an
	// extracted clip starting and ending with a click.
	int fadeInMs = 0;
	int fadeOutMs = 0;
	// Linear, 1.0 = unchanged. The window shows dB; this is the raw multiplier
	// so the arithmetic below has no logarithms in it.
	double gain = 1.0;
	// Pitch-corrected playback rate. 1.0 = unchanged.
	double speed = 1.0;

	static constexpr double kMinSpeed = 0.25;
	static constexpr double kMaxSpeed = 4.0;
	static constexpr double kMaxGain = 8.0; // +18 dB, past which it is all clipping
};

// Where the kept part actually starts and ends, resolved against a real
// duration. Handles the "not dragged yet" case and refuses to return a
// backwards or out-of-range window.
inline void resolveTrim(const AudioEdit &e, qint64 sourceMs, qint64 *outStart, qint64 *outEnd)
{
	const qint64 start = std::clamp<qint64>(e.trimStartMs, 0, std::max<qint64>(0, sourceMs));
	qint64 end = e.trimEndMs;
	if (end <= start || end > sourceMs)
		end = sourceMs;
	*outStart = start;
	*outEnd = std::max(start, end);
}

// How long the exported file will be. Speed divides it -- twice as fast is half
// as long -- which is the bit a progress bar and a "3.2 s" label both need.
inline qint64 extractDurationMs(const AudioEdit &e, qint64 sourceMs)
{
	qint64 start = 0, end = 0;
	resolveTrim(e, sourceMs, &start, &end);
	const double sp = std::clamp(e.speed, AudioEdit::kMinSpeed, AudioEdit::kMaxSpeed);
	return qint64(std::llround((end - start) / sp));
}

// The loudest sample in a buffer, 0..1+ (it can exceed 1 if something upstream
// already clipped).
inline double peakOf(const std::vector<float> &pcm)
{
	double peak = 0.0;
	for (float s : pcm)
		peak = std::max(peak, double(std::abs(s)));
	return peak;
}

// The gain that would put the loudest sample just under full scale.
// headroomDb backs off from the ceiling, because a sample sitting at exactly
// 1.0 clips the moment a lossy encoder reconstructs it slightly high.
//
// Silence returns 1.0 rather than an enormous number: multiplying nothing by a
// million is still nothing, but it would make the gain readout nonsense and
// blow up the moment a single non-zero sample arrived.
inline double normaliseGainFor(const std::vector<float> &pcm, double headroomDb = 1.0)
{
	const double peak = peakOf(pcm);
	if (peak < 1e-6)
		return 1.0;
	const double ceiling = std::pow(10.0, -std::abs(headroomDb) / 20.0);
	return std::clamp(ceiling / peak, 0.0, AudioEdit::kMaxGain);
}

// dB <-> linear, for the window's slider.
inline double dbToGain(double db)
{
	return std::pow(10.0, db / 20.0);
}
inline double gainToDb(double g)
{
	return g > 1e-9 ? 20.0 * std::log10(g) : -96.0;
}

// Cut the kept part out of a decoded buffer. Frame-accurate: the maths is done
// in FRAMES, not in samples, or a stereo trim lands half a frame off and the
// channels swap for the rest of the file.
inline std::vector<float> trimPcm(const std::vector<float> &pcm, int channels, int rate,
				  const AudioEdit &e)
{
	if (pcm.empty() || channels <= 0 || rate <= 0)
		return {};
	const qint64 totalFrames = qint64(pcm.size()) / channels;
	const qint64 sourceMs = totalFrames * 1000 / rate;
	qint64 startMs = 0, endMs = 0;
	resolveTrim(e, sourceMs, &startMs, &endMs);

	const qint64 firstFrame = std::clamp<qint64>(startMs * rate / 1000, 0, totalFrames);
	const qint64 lastFrame = std::clamp<qint64>(endMs * rate / 1000, firstFrame, totalFrames);
	return std::vector<float>(pcm.begin() + firstFrame * channels,
				  pcm.begin() + lastFrame * channels);
}

// Gain and the two fades, in place. Applied AFTER the trim, so the ramps sit at
// the ends of what was kept rather than at the ends of the original file --
// which is the whole point of a fade on a trimmed clip.
//
// Overlapping fades (both longer than half the clip) are scaled down to meet in
// the middle instead of multiplying into a notch: a 2-second clip with 5-second
// ramps at each end should fade up then straight back down, not vanish.
inline void applyGainAndFades(std::vector<float> &pcm, int channels, int rate, const AudioEdit &e)
{
	if (pcm.empty() || channels <= 0 || rate <= 0)
		return;
	const qint64 frames = qint64(pcm.size()) / channels;
	const double gain = std::clamp(e.gain, 0.0, AudioEdit::kMaxGain);

	qint64 inFrames = std::max<qint64>(0, qint64(e.fadeInMs) * rate / 1000);
	qint64 outFrames = std::max<qint64>(0, qint64(e.fadeOutMs) * rate / 1000);
	if (inFrames + outFrames > frames && inFrames + outFrames > 0) {
		const double squeeze = double(frames) / double(inFrames + outFrames);
		inFrames = qint64(inFrames * squeeze);
		outFrames = frames - inFrames;
	}

	for (qint64 f = 0; f < frames; ++f) {
		double g = gain;
		if (inFrames > 0 && f < inFrames)
			g *= double(f) / double(inFrames);
		if (outFrames > 0 && f >= frames - outFrames)
			g *= double(frames - 1 - f) / double(outFrames);
		for (int c = 0; c < channels; ++c)
			pcm[size_t(f * channels + c)] = float(pcm[size_t(f * channels + c)] * g);
	}
}

// Hard-limit to [-1, 1]. A gain of +12 dB on an already-loud recording produces
// samples well past full scale, and an encoder handed those wraps them into
// crackle rather than clipping them politely.
inline void clampPcm(std::vector<float> &pcm)
{
	for (float &s : pcm)
		s = std::clamp(s, -1.0f, 1.0f);
}

// The whole chain except the time-stretch, which needs a filter graph. Ordered
// deliberately: trim first so the fades land on the kept part, then gain and
// fades, then the limiter last so it catches whatever the gain produced.
inline std::vector<float> applyAudioEdit(const std::vector<float> &pcm, int channels, int rate,
					 const AudioEdit &e)
{
	std::vector<float> out = trimPcm(pcm, channels, rate, e);
	applyGainAndFades(out, channels, rate, e);
	clampPcm(out);
	return out;
}

// Waveform buckets from an already-decoded buffer: the peak magnitude in each
// of `buckets` equal slices, 0..1.
//
// Computed from the SAME samples the export uses rather than by re-reading the
// file, so the picture cannot disagree with the sound -- and a second decode of
// a long recording is not free.
inline std::vector<float> peaksFromPcm(const std::vector<float> &pcm, int channels, int buckets)
{
	std::vector<float> out;
	if (pcm.empty() || channels <= 0 || buckets <= 0)
		return out;
	const qint64 frames = qint64(pcm.size()) / channels;
	if (frames <= 0)
		return out;
	// More buckets than frames would leave empty slices reading as silence, so
	// never ask for finer than one frame per bucket.
	const int n = int(std::min<qint64>(buckets, frames));
	out.resize(size_t(n), 0.0f);
	for (int b = 0; b < n; ++b) {
		const qint64 from = frames * b / n;
		const qint64 to = std::max(from + 1, frames * (b + 1) / n);
		float peak = 0.0f;
		for (qint64 f = from; f < to; ++f)
			for (int c = 0; c < channels; ++c)
				peak = std::max(peak, std::abs(pcm[size_t(f * channels + c)]));
		out[size_t(b)] = std::min(peak, 1.0f);
	}
	return out;
}

// ---- the parts that need FFmpeg (AudioExtract.cpp) -------------------------

// Which formats this build can actually write. Probed, not assumed: MP3 needs
// libmp3lame and the bundled FFmpeg may be built without it, and offering a
// format that fails at the last step is worse than not offering it.
QStringList availableAudioFormatNames();
bool audioFormatAvailable(AudioFormat f);

// Decode any media file's first audio stream to interleaved float at
// `rate`/`channels`. Empty with *err set on failure -- including the ordinary
// case of a video that has no audio track at all, which the caller reports
// rather than treating as an error.
std::vector<float> decodeAudioToPcm(const QString &path, int rate, int channels, QString *err);

// Pitch-preserving time-stretch, via FFmpeg's atempo. Returns the input
// unchanged for speeds within a thousandth of 1.0.
std::vector<float> retimePcm(const std::vector<float> &pcm, int rate, int channels, double speed,
			     QString *err);

// Write a buffer out. WAV is written directly; MP3 and M4A go through libav.
bool encodeAudioFile(const QString &path, AudioFormat format, const std::vector<float> &pcm,
		     int rate, int channels, int bitrateKbps, QString *err);

} // namespace harpia
