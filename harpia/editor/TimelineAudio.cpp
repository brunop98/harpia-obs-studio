#include "TimelineAudio.hpp"

#include <QDir>
#include <QTemporaryDir>

#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <optional>
#include <utility>

namespace harpia {

namespace {
// Decoded WAVs, kept for the SESSION rather than for one mix. The mix key
// includes every clip's position, so nudging one clip used to invalidate the
// whole mix -- correct -- and then re-decode every source from scratch --
// wasteful, since the decoded audio of a given (file, speed) never changes.
// Keyed by path+size+mtime so a re-recorded file can never be served stale,
// and capped: decoded WAVs are big (~700 MB per hour of source), so at most a
// handful live at once, evicted least-recently-used with their files deleted.
struct WavSessionCache {
	QMutex mu;
	QTemporaryDir dir;
	std::map<QString, QString> map; // identity key -> wav path
	std::list<QString> lru;         // front = most recent
	int counter = 0;
	static constexpr int kMaxEntries = 12;
};
WavSessionCache &wavSession()
{
	static WavSessionCache c;
	return c;
}

QString wavIdentityKey(const QString &src, double speed)
{
	const QFileInfo fi(src);
	return fi.absoluteFilePath() + QLatin1Char('|') + QString::number(fi.size()) +
	       QLatin1Char('|') + QString::number(fi.lastModified().toMSecsSinceEpoch()) +
	       QLatin1Char('|') + QString::number(qint64(std::llround(speed * 1000.0)));
}

// The decoded WAV for (src, speed), decoding at most once per session. An
// empty VALUE means "this source has no usable audio" -- cached too, so a
// silent source is not re-probed on every edit. nullopt means the cache itself
// is unavailable and the caller should decode into its own directory.
std::optional<QString> sessionWavFor(const QString &src, double speed)
{
	WavSessionCache &c = wavSession();
	if (!c.dir.isValid())
		return std::nullopt; // no temp space: caller falls back to its own dir
	const QString key = wavIdentityKey(src, speed);
	{
		QMutexLocker lock(&c.mu);
		auto it = c.map.find(key);
		if (it != c.map.end()) {
			c.lru.remove(key);
			c.lru.push_front(key);
			return it->second;
		}
	}
	// Decode outside the lock: it can take seconds, and another source's
	// lookup must not wait on it. A racing double-decode of the same key is
	// possible and harmless -- distinct filenames, last insert wins.
	QString wav;
	QString cand;
	{
		QMutexLocker lock(&c.mu);
		cand = QDir(c.dir.path()).filePath(QStringLiteral("s%1.wav").arg(c.counter++));
	}
	if (VoiceoverMixer::decodeToWav(src, cand, speed))
		wav = cand;
	QMutexLocker lock(&c.mu);
	c.map[key] = wav;
	c.lru.push_front(key);
	return wav;
}

// Eviction happens BETWEEN mixes, never during one: buildTakes hands out file
// paths that renderTakes reads afterwards, so evicting inside sessionWavFor
// could delete a WAV this very mix is about to read (any timeline with more
// sources than the cap would sabotage itself). Called at the top of
// buildTakes, where nothing holds paths yet.
void pruneSessionWavs()
{
	WavSessionCache &c = wavSession();
	QMutexLocker lock(&c.mu);
	while (int(c.lru.size()) > WavSessionCache::kMaxEntries) {
		const QString victim = c.lru.back();
		c.lru.pop_back();
		auto it = c.map.find(victim);
		if (it != c.map.end()) {
			if (!it->second.isEmpty())
				QFile::remove(it->second);
			c.map.erase(it);
		}
	}
}
} // namespace

std::vector<VoiceoverMixer::Take> TimelineAudio::buildTakes(const TimelineModel &m,
							    const SourceLookup &pathFor,
							    const QString &workDir)
{
	pruneSessionWavs(); // safe here: no paths from this mix are held yet

	// Cached per (source, speed): a clip played at 1.5x needs its own atempo'd
	// render, but every 1x clip of the same source shares one decode.
	std::map<std::pair<int, int>, QString> wavCache; // (sourceId, speed*1000) -> WAV
	std::vector<VoiceoverMixer::Take> takes;
	int nextWav = 0;

	for (const TlTrack &t : m.tracks) {
		if (!m.trackAudible(t))
			continue; // muted, soloed out, or an effect track (no sound)
		for (const TlClip &c : t.clips) {
			if (c.type == TlClip::Type::Text || c.type == TlClip::Type::Image ||
			    c.type == TlClip::Type::Effect)
				continue; // captions, stills and effects have no audio
			const double speed = (c.speed > 0.01) ? c.speed : 1.0;
			const auto key = std::make_pair(c.sourceId, int(std::lround(speed * 1000.0)));
			auto it = wavCache.find(key);
			if (it == wavCache.end()) {
				const QString src = pathFor(c.sourceId);
				QString wav;
				if (!src.isEmpty()) {
					// Session cache first: a (file, speed) decode never
					// changes, so surviving one edit-to-play cycle to the
					// next is pure win. Time-stretched (pitch preserved) so
					// sped-up clips stay locked to the picture.
					if (const auto cached = sessionWavFor(src, speed)) {
						wav = *cached; // authoritative, even when empty
					} else {
						const QString cand = QDir(workDir).filePath(
							QStringLiteral("a%1.wav").arg(nextWav++));
						if (VoiceoverMixer::decodeToWav(src, cand, speed))
							wav = cand;
					}
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
			// is an audio-track control. The lane's own gain sits on top of
			// either, so a whole music bed comes down with one number.
			tk.volume = ((t.kind == TlTrack::Kind::Audio) ? c.volume : 1.0) * t.gain;
			// Clamped here rather than trusted: a clip trimmed shorter than
			// its fade would otherwise never reach full volume.
			const int dur = int(std::min<qint64>(c.outDurationMs(), 1 << 30));
			tk.fadeInMs = std::clamp(c.fadeInMs, 0, dur);
			tk.fadeOutMs = std::clamp(c.fadeOutMs, 0, dur);
			tk.fadeInCurve = c.fadeInCurve;
			tk.fadeOutCurve = c.fadeOutCurve;

			// An overlap on this track is a transition, and if both clips carry
			// sound it crossfades too. Equal power by default, so the mix does
			// not dip in the middle the way two linear fades would.
			//
			// The transition's fade REPLACES a shorter hand-set one over the
			// same edge rather than adding to it: two fades on one edge would
			// multiply and punch a hole in the middle of the transition.
			const int ci = int(&c - t.clips.constData());
			if (c.transition.enabled) {
				const int inOverlap =
					int(std::clamp<qint64>(t.overlapBefore(ci), 0, dur));
				if (inOverlap > tk.fadeInMs) {
					tk.fadeInMs = inOverlap;
					tk.fadeInCurve = FadeCurve::EqualPower;
				}
			}
			// The other side of the same overlap: whichever later clip starts
			// inside this one takes it out.
			for (int j = 0; j < t.clips.size(); ++j) {
				if (j == ci || !t.clips[j].transition.enabled)
					continue;
				const TlClip &nx = t.clips[j];
				if (nx.outStartMs <= c.outStartMs || nx.outStartMs >= c.outEndMs())
					continue;
				const int outOverlap = int(std::clamp<qint64>(
					std::min(c.outEndMs(), nx.outEndMs()) - nx.outStartMs, 0, dur));
				if (outOverlap > tk.fadeOutMs) {
					tk.fadeOutMs = outOverlap;
					tk.fadeOutCurve = FadeCurve::EqualPower;
				}
			}
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
