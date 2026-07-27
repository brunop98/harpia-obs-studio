#pragma once

#include <QString>

#include <atomic>
#include <vector>

namespace harpia {

// Post-process pass that mixes recorded narration takes onto an already-exported
// video's audio, in the sample domain (decode → mix floats → re-encode AAC →
// remux with the copied video). Sample-domain mixing keeps it deterministic and
// free of filter-graph path-escaping pitfalls, and makes the gain/fade/duck math
// testable. Video is stream-copied — only the audio is rebuilt.
class VoiceoverMixer {
public:
	struct Take {
		QString path;          // WAV on disk (any rate/channels; resampled)
		qint64 outStartMs = 0; // placement on the output timeline
		qint64 srcStartMs = 0; // offset into the source (trim/split)
		qint64 playMs = 0;     // played length (0 = to end of source)
		double volume = 1.0;   // linear gain
		int fadeInMs = 15;
		int fadeOutMs = 15;
	};

	// Mix `takes` over `videoPath`'s audio, replacing `videoPath` in place.
	// originalVolume scales the source audio (0..2); when duck is true the source
	// dips under narration. Returns "" on success, else an error message. Honors
	// *cancel if provided.
	static QString mix(const QString &videoPath, double originalVolume, bool duck,
			   const std::vector<Take> &takes, std::atomic<bool> *cancel);

	// Decode any audio file to a 16-bit PCM WAV (48 kHz stereo) at `outWav`, so an
	// imported track becomes a normal voiceover take (waveform + trim + mix all
	// work uniformly). `speed` > 1 shortens the result (pitch preserved via
	// atempo), matching a clip played faster. Returns true on success.
	static bool decodeToWav(const QString &inPath, const QString &outWav, double speed = 1.0);

	// Exposed for unit testing: apply gain + linear fades to one take and add it
	// into `mix` (interleaved stereo) at `startFrame`; and build the ducking gain
	// envelope. Pure math, no libav.
	static void addTake(std::vector<float> &mix, const std::vector<float> &take,
			    long long startFrame, double volume, int fadeInFrames,
			    int fadeOutFrames);
	static std::vector<float> duckEnvelope(long long totalFrames,
					       const std::vector<std::pair<long long, long long>> &spans,
					       float duckLevel, int attackFrames, int releaseFrames);
};

} // namespace harpia
