#include "TimelineAudio.hpp"

#include <QDir>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace harpia {

std::vector<VoiceoverMixer::Take> TimelineAudio::buildTakes(const TimelineModel &m,
							    const SourceLookup &pathFor,
							    const QString &workDir)
{
	// Cached per (source, speed): a clip played at 1.5x needs its own atempo'd
	// render, but every 1x clip of the same source shares one decode.
	std::map<std::pair<int, int>, QString> wavCache; // (sourceId, speed*1000) -> WAV
	std::vector<VoiceoverMixer::Take> takes;
	int nextWav = 0;

	for (const TlTrack &t : m.tracks) {
		if (t.muted)
			continue;
		for (const TlClip &c : t.clips) {
			if (c.type == TlClip::Type::Text || c.type == TlClip::Type::Image)
				continue; // captions and stills have no audio
			const double speed = (c.speed > 0.01) ? c.speed : 1.0;
			const auto key = std::make_pair(c.sourceId, int(std::lround(speed * 1000.0)));
			auto it = wavCache.find(key);
			if (it == wavCache.end()) {
				const QString src = pathFor(c.sourceId);
				QString wav;
				if (!src.isEmpty()) {
					const QString cand =
						QDir(workDir).filePath(QStringLiteral("a%1.wav").arg(nextWav++));
					// Time-stretched (pitch preserved) so sped-up clips stay
					// locked to the picture.
					if (VoiceoverMixer::decodeToWav(src, cand, speed))
						wav = cand;
				}
				it = wavCache.emplace(key, wav).first;
			}
			if (it->second.isEmpty())
				continue; // that source has no usable audio

			VoiceoverMixer::Take tk;
			tk.path = it->second;
			tk.outStartMs = c.outStartMs;
			// The cached WAV is already in OUTPUT time, so the clip's source
			// offsets scale by the same factor.
			tk.srcStartMs = qint64(std::llround(c.srcStartMs / speed));
			tk.playMs = c.outDurationMs();
			// A video track carries its footage's sound at unity; per-clip volume
			// is an audio-track control.
			tk.volume = (t.kind == TlTrack::Kind::Audio) ? c.volume : 1.0;
			// Clamped here rather than trusted: a clip trimmed shorter than
			// its fade would otherwise never reach full volume.
			const int dur = int(std::min<qint64>(c.outDurationMs(), 1 << 30));
			tk.fadeInMs = std::clamp(c.fadeInMs, 0, dur);
			tk.fadeOutMs = std::clamp(c.fadeOutMs, 0, dur);
			tk.fadeInCurve = c.fadeInCurve;
			tk.fadeOutCurve = c.fadeOutCurve;
			takes.push_back(tk);
		}
	}
	return takes;
}

std::vector<float> TimelineAudio::mixToBuffer(const TimelineModel &m, const SourceLookup &pathFor,
					      std::atomic<bool> *cancel)
{
	// The decoded WAVs only need to live until renderTakes has read them.
	QTemporaryDir tmp;
	if (!tmp.isValid())
		return {};
	const std::vector<VoiceoverMixer::Take> takes = buildTakes(m, pathFor, tmp.path());
	if (takes.empty() || (cancel && cancel->load()))
		return {};
	return VoiceoverMixer::renderTakes(takes, cancel);
}

} // namespace harpia
