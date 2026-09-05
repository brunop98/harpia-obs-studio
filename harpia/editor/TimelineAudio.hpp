#pragma once

// Turning a Full-editing timeline into audio.
//
// The placement rules — which clips contribute sound, where each lands, how
// speed, volume and fades apply — live here so the preview and the exporter
// share them. They used to exist only inside ClipExporter, which is why the
// preview had no way to play the same mix it was going to render.
//
// This is the audio counterpart of TimelineCompositor: one description of the
// timeline, used by both what you hear and what you get.

#include "VoiceoverMixer.hpp"
#include "timeline/TimelineModel.hpp"

#include <QString>

#include <atomic>
#include <functional>
#include <vector>

namespace harpia {

class TimelineAudio {
public:
	// Maps a clip's sourceId to a media file, or "" when it isn't available.
	using SourceLookup = std::function<QString(int)>;

	// Decode every distinct (source, speed) into `workDir` once and return the
	// clips placed as mixer takes. Muted tracks, captions and stills contribute
	// nothing. A clip's speed is applied by time-stretching its audio, so sped-up
	// footage stays locked to the picture.
	//
	// The WAVs are written into `workDir`, which must outlive any use of the
	// returned takes (they reference those files by path).
	static std::vector<VoiceoverMixer::Take> buildTakes(const TimelineModel &m,
							    const SourceLookup &pathFor,
							    const QString &workDir);

	// The gain a clip's take is mixed at: the clip's own volume times its
	// lane's. The clip's volume counts on a VIDEO lane too. It used to be pinned
	// to unity there, which left footage the one kind of sound you could not
	// turn down -- and the Inspector now shows the same Audio section for it.
	// Header-only so the rule can be checked without a decoder.
	static double takeVolume(const TlTrack &t, const TlClip &c)
	{
		return std::clamp(c.volume, 0.0, 2.0) * std::clamp(t.gain, 0.0, 2.0);
	}

	// The whole timeline as one interleaved stereo 48 kHz float buffer, ready to
	// play. Empty when the timeline has no audio. Honors *cancel.
	static std::vector<float> mixToBuffer(const TimelineModel &m, const SourceLookup &pathFor,
					      std::atomic<bool> *cancel = nullptr);

	static constexpr int kRate = 48000;
	static constexpr int kChannels = 2;

	// Byte offset in a mixToBuffer() result for an output-time position.
	static qint64 frameForMs(qint64 ms) { return ms * kRate / 1000; }
};

} // namespace harpia
