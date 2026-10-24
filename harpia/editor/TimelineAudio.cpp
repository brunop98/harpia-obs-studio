#include "TimelineAudio.hpp"

#include <QDir>
#include <QCryptographicHash>
#include <QDataStream>
#include <QTemporaryDir>

#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <memory>
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
struct CachedWav {
	QString path;
	explicit CachedWav(QString p) : path(std::move(p)) {}
	~CachedWav()
	{
		if (!path.isEmpty())
			QFile::remove(path);
	}
};

struct WavSessionCache {
	QMutex mu;
	QTemporaryDir dir;
	std::map<QString, std::shared_ptr<const CachedWav>> map; // identity -> owned WAV
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
	       QLatin1Char('|') + QString::number(speed, 'g', 17);
}

// The decoded WAV for (src, speed), decoding at most once per session. An
// empty VALUE means "this source has no usable audio" -- cached too, so a
// silent source is not re-probed on every edit. nullopt means the cache itself
// is unavailable and the caller should decode into its own directory.
std::optional<std::shared_ptr<const CachedWav>> sessionWavFor(const QString &src, double speed)
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
	else
		QFile::remove(cand); // a failed decode can leave an incomplete WAV
	const auto decoded = std::make_shared<const CachedWav>(std::move(wav));
	QMutexLocker lock(&c.mu);
	c.map[key] = decoded;
	c.lru.remove(key); // another worker may have inserted this key meanwhile
	c.lru.push_front(key);
	return decoded;
}

// Drop the cache's ownership between builds. Returned takes retain a lease,
// so concurrent preview/export mixes can continue reading evicted WAVs until
// their last take is destroyed.
void pruneSessionWavs()
{
	WavSessionCache &c = wavSession();
	QMutexLocker lock(&c.mu);
	while (int(c.lru.size()) > WavSessionCache::kMaxEntries) {
		const QString victim = c.lru.back();
		c.lru.pop_back();
		auto it = c.map.find(victim);
		if (it != c.map.end()) {
			c.map.erase(it);
		}
	}
}
} // namespace

QString TimelineAudio::mixKey(const TimelineModel &m, const SourceLookup &pathFor)
{
	QByteArray bytes;
	QDataStream out(&bytes, QIODevice::WriteOnly);
	out.setVersion(QDataStream::Qt_6_0);
	bool hasAudioClip = false;
	QSet<int> sourceIdentities;
	for (const TlTrack &track : m.tracks) {
		if (!m.trackAudible(track))
			continue;
		out << qint32(track.kind) << track.gain << qint64(track.clips.size());
		for (const TlClip &clip : track.clips) {
			// Even a silent clip can change an adjacent clip's overlap fade.
			out << qint32(clip.type) << clip.srcStartMs << clip.srcEndMs
			    << clip.outStartMs << clip.speed << clip.transition.enabled;
			if (clip.type != TlClip::Type::Video)
				continue;
			hasAudioClip = true;
			out << qint32(clip.sourceId) << clip.volume << qint32(clip.fadeInMs)
			    << qint32(clip.fadeOutMs) << qint32(clip.fadeInCurve) << qint32(clip.fadeOutCurve);
			if (pathFor && !sourceIdentities.contains(clip.sourceId)) {
				sourceIdentities.insert(clip.sourceId);
				const QString path = pathFor(clip.sourceId);
				const QFileInfo file(path);
				out << qint32(clip.sourceId) << (path.isEmpty() ? QString() : file.absoluteFilePath())
				    << file.size() << file.lastModified().toMSecsSinceEpoch();
			}
		}
	}
	return hasAudioClip ? QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())
			    : QString();
}

std::vector<VoiceoverMixer::Take> TimelineAudio::buildTakes(const TimelineModel &m,
							    const SourceLookup &pathFor,
							    const QString &workDir)
{
	pruneSessionWavs();

	// Cached per (source, speed): a clip played at 1.5x needs its own atempo'd
	// render, but every 1x clip of the same source shares one decode.
	struct WavInput {
		QString path;
		std::shared_ptr<const void> lease;
	};
	std::map<std::pair<int, double>, WavInput> wavCache; // (sourceId, exact speed) -> WAV
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
			const auto key = std::make_pair(c.sourceId, speed);
			auto it = wavCache.find(key);
			if (it == wavCache.end()) {
				const QString src = pathFor(c.sourceId);
				WavInput wav;
				if (!src.isEmpty()) {
					// Session cache first: a (file, speed) decode never
					// changes, so surviving one edit-to-play cycle to the
					// next is pure win. Time-stretched (pitch preserved) so
					// sped-up clips stay locked to the picture.
					if (const auto cached = sessionWavFor(src, speed)) {
						wav.path = (*cached)->path; // authoritative, even when empty
						wav.lease = *cached;
					} else {
						const QString cand = QDir(workDir).filePath(
							QStringLiteral("a%1.wav").arg(nextWav++));
						if (VoiceoverMixer::decodeToWav(src, cand, speed))
							wav.path = cand;
					}
				}
				it = wavCache.emplace(key, wav).first;
			}
			if (it->second.path.isEmpty())
				continue; // that source has no usable audio

			VoiceoverMixer::Take tk;
			tk.path = it->second.path;
			tk.sourceLease = it->second.lease;
			tk.outStartMs = c.outStartMs;
			// The cached WAV is already in OUTPUT time, so the clip's source
			// offsets scale by the same factor.
			tk.srcStartMs = qint64(std::llround(c.srcStartMs / speed));
			tk.playMs = c.outDurationMs();
			// The clip's own level times the lane's, on video and audio lanes
			// alike: see takeVolume.
			tk.volume = takeVolume(t, c);
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
