#include "SilenceTrim.hpp"

#include "component/TextSubtitle.hpp"
#include "timeline/ClipVariations.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace harpia {
namespace silence_trim {
namespace {

bool canceled(std::atomic<bool> *cancel)
{
	return cancel && cancel->load();
}

QVector<TlSpan> detectFrames(const float *pcm, size_t frames, qint64 frameOffset,
			     int rate, int channels, const Options &options,
			     std::atomic<bool> *cancel)
{
	QVector<TlSpan> out;
	if (!pcm || frames == 0 || rate <= 0 || channels <= 0 || channels > 64 ||
	    !std::isfinite(options.thresholdDb) || canceled(cancel))
		return out;
	const double threshold = std::pow(10.0, std::clamp(options.thresholdDb, -120.0, 0.0) / 20.0);
	const size_t window = size_t(std::max(1, rate / 100));
	const qint64 minimum = std::max<qint64>(10, options.minSilenceMs);
	const qint64 keep = std::max<qint64>(0, options.keepPauseMs);
	bool haveSound = false;
	qint64 quietFrom = -1;
	const auto atMs = [rate, frameOffset](size_t frame) {
		return qint64(std::llround((double(frameOffset) + double(frame)) * 1000.0 / rate));
	};
	const auto finishQuiet = [&](size_t until) {
		if (quietFrom < 0)
			return;
		const size_t fromFrame = size_t(quietFrom);
		const qint64 start = atMs(fromFrame), end = atMs(until);
		const bool leading = fromFrame == 0, trailing = until == frames;
		if ((options.includeEdges || (!leading && !trailing)) && end - start >= minimum &&
		    end - start > keep) {
			// Internal pauses keep breathing room on both sides. At an outer
			// edge all the retained room stays next to the audible content.
			const qint64 left = leading ? 0 : (trailing ? keep : keep / 2);
			const qint64 right = trailing ? 0 : (leading ? keep : keep - keep / 2);
			if (end - right > start + left)
				out.append({start + left, end - right});
		}
		quietFrom = -1;
	};
	for (size_t from = 0; from < frames; from += window) {
		if (canceled(cancel))
			return {};
		const size_t count = std::min(window, frames - from);
		double loudestRms = 0.0, peak = 0.0;
		bool finite = true;
		for (int channel = 0; channel < channels; ++channel) {
			double energy = 0.0;
			for (size_t frame = from; frame < from + count; ++frame) {
				const double sample = pcm[frame * size_t(channels) + size_t(channel)];
				if (!std::isfinite(sample)) {
					finite = false;
					continue;
				}
				energy += sample * sample;
				peak = std::max(peak, std::abs(sample));
			}
			loudestRms = std::max(loudestRms, std::sqrt(energy / count));
		}
		// Opposite-polarity stereo cannot cancel into false silence, and a
		// short click cannot disappear just because its window's RMS is low.
		const bool quiet = finite && loudestRms < threshold && peak < threshold * 4.0;
		if (quiet) {
			if (quietFrom < 0)
				quietFrom = qint64(from);
		} else {
			haveSound = haveSound || (finite && (loudestRms >= threshold || peak >= threshold * 4.0));
			finishQuiet(from);
		}
	}
	finishQuiet(frames);
	return haveSound && !canceled(cancel) ? out : QVector<TlSpan>{};
}

QString trackLabel(const TlTrack &track, int index)
{
	return track.name.isEmpty() ? QStringLiteral("Track %1").arg(index + 1) : track.name;
}

bool inside(qint64 at, qint64 from, qint64 to)
{
	return at > from && at < to;
}

bool hasTimedCaption(const TlClip &clip)
{
	for (const ComponentInstance &component : clip.components)
		if (component.enabled && (component.typeId == QStringLiteral("harpia.subtitle") ||
					  component.typeId == QStringLiteral("harpia.captionPop")))
			return true;
	return false;
}

bool supportsTrimming(const ComponentInstance &component)
{
	const QString &id = component.typeId;
	return id == QStringLiteral("harpia.transform") || id == QStringLiteral("harpia.mask") ||
	       id == QStringLiteral("harpia.blur") || id.startsWith(QStringLiteral("harpia.fx.")) ||
	       id == QStringLiteral("harpia.subtitle") || id == QStringLiteral("harpia.captionPop");
}

QPair<qint64, qint64> envelopeTimes(const ComponentInstance &component, qint64 duration)
{
	qint64 in = std::max<qint64>(0, component.inMs), out = std::max<qint64>(0, component.outMs);
	if (double(in) + double(out) > duration) {
		const double ratio = double(duration) / (double(in) + double(out));
		in = qint64(in * ratio);
		out = qint64(out * ratio);
	}
	return {in, out};
}

QString fragmentError(const TlClip &clip, qint64 from, qint64 to)
{
	if (from == clip.outStartMs && to == clip.outEndMs())
		return {};
	// Re-speaking a generated narration currently owns one continuous audio
	// clip per link. Splitting it would leave later captions attached to the
	// wrong source clock on a future sentence edit.
	if (!clip.tts.isEmpty())
		return QStringLiteral("A pause cuts through editable generated narration. Export and re-add it as ordinary audio before trimming, or keep that pause to preserve Re-speak editing.");
	if (clip.type == TlClip::Type::Text && clip.text.random && !clip.text.variations.isEmpty() &&
	    hasTimedCaption(clip))
		return QStringLiteral("A pause cuts through animated Random text. Choose one text version or keep that pause so every version retains its timing.");
	for (const TlScript &script : clip.scripts)
		if (!script.name.isEmpty())
			return QStringLiteral("A pause cuts through a clip with a transform script. Split or remove that script before trimming silence.");
	for (qint64 boundary : {from, to}) {
		if (inside(boundary, clip.outStartMs, clip.outStartMs + clip.fadeInMs) ||
		    inside(boundary, clip.outEndMs() - clip.fadeOutMs, clip.outEndMs()))
			return QStringLiteral("A pause boundary falls inside an audio fade. Adjust the pause selection or fade before trimming.");
	}
	for (const ComponentInstance &component : clip.components) {
		if (!component.enabled)
			continue;
		if (!supportsTrimming(component))
			return QStringLiteral("A pause cuts through a clip with the component %1. Its timing cannot be preserved automatically; split or disable it before trimming.")
				.arg(component.typeId);
		const auto envelope = envelopeTimes(component, clip.outDurationMs());
		for (qint64 boundary : {from, to}) {
			const qint64 relative = boundary - clip.outStartMs;
			if (inside(relative, 0, envelope.first) ||
			    inside(relative, clip.outDurationMs() - envelope.second, clip.outDurationMs()))
				return QStringLiteral("A pause boundary falls inside a component's fade. Adjust the pause selection or fade before trimming.");
		}
	}
	return {};
}

TlClip fragment(const TlClip &clip, qint64 from, qint64 to, const QVector<TlSpan> &removed)
{
	TlClip result = clip;
	result.outStartMs = mapTime(from, removed);
	if (from == clip.outStartMs && to == clip.outEndMs())
		return result; // a moved whole clip retains every local property verbatim
	const qint64 head = from - clip.outStartMs;
	if (clip.freeDuration()) {
		const double speed = clip.speed > 0.01 ? clip.speed : 1.0;
		result.srcStartMs = 0;
		result.srcEndMs = std::max<qint64>(1, qint64(std::llround((to - from) * speed)));
	} else {
		result.srcStartMs = std::clamp(clip.srcAtOutput(from), clip.srcStartMs, clip.srcEndMs);
		result.srcEndMs = std::clamp(clip.srcAtOutput(to), result.srcStartMs, clip.srcEndMs);
		if (result.isRandomClip()) {
			clip_variations::shiftOthers(result, result.srcStartMs - clip.srcStartMs);
			clip_variations::syncShown(result);
		}
	}
	for (TlKeyframe &key : result.keys)
		key.tMs -= head;
	for (FxKey &key : result.fx.keys)
		key.tMs -= head;
	for (SpotMask &mask : result.fx.spot.masks)
		for (SpotKey &key : mask.keys)
			key.tMs -= head;
	for (ComponentInstance &component : result.components) {
		for (auto keys = component.keys.begin(); keys != component.keys.end(); ++keys)
			for (PropKey &key : keys.value())
				key.tMs -= head;
		const auto envelope = envelopeTimes(component, clip.outDurationMs());
		component.inMs = from == clip.outStartMs ? envelope.first : 0;
		component.outMs = to == clip.outEndMs() ? envelope.second : 0;
	}
	if (hasTimedCaption(clip))
		result.words = timedCaptionWords(clip.text.text, clip.words, clip.outDurationMs());
	for (ClipWordTime &word : result.words) {
		word.startMs -= head;
		word.endMs -= head;
	}
	// A new join does not invent a fade. Original outer fades stay at their
	// original ends; fragmentError rejected boundaries inside an active ramp.
	result.fadeInMs = from == clip.outStartMs ? clip.fadeInMs : 0;
	result.fadeOutMs = to == clip.outEndMs() ? clip.fadeOutMs : 0;
	result.clampFades();
	return result;
}

} // namespace

QVector<TlSpan> detect(const std::vector<float> &pcm, int sampleRate, int channels,
		       const Options &options, std::atomic<bool> *cancel)
{
	if (sampleRate <= 0 || channels <= 0 || channels > 64 || pcm.empty() ||
	    pcm.size() % size_t(channels) != 0)
		return {};
	return detectFrames(pcm.data(), pcm.size() / size_t(channels), 0, sampleRate, channels, options, cancel);
}

QVector<TlSpan> detectRange(const std::vector<float> &pcm, int sampleRate, int channels,
			    qint64 fromMs, qint64 toMs, const Options &options, std::atomic<bool> *cancel)
{
	if (sampleRate <= 0 || channels <= 0 || channels > 64 || pcm.empty() ||
	    pcm.size() % size_t(channels) != 0 || toMs <= fromMs)
		return {};
	const size_t frames = pcm.size() / size_t(channels);
	const auto frameAt = [sampleRate, frames](qint64 ms) {
		return size_t(std::clamp(double(ms) * sampleRate / 1000.0, 0.0, double(frames)));
	};
	const size_t from = frameAt(fromMs), to = frameAt(toMs);
	if (to <= from)
		return {};
	return detectFrames(pcm.data() + from * size_t(channels), to - from, qint64(from),
			    sampleRate, channels, options, cancel);
}

QVector<TlSpan> normalize(const QVector<TlSpan> &ranges, qint64 durationMs)
{
	QVector<TlSpan> result;
	if (durationMs <= 0)
		return result;
	for (const TlSpan &range : ranges) {
		const qint64 from = std::clamp<qint64>(range.fromMs, 0, durationMs);
		const qint64 to = std::clamp<qint64>(range.toMs, 0, durationMs);
		if (to > from)
			result.append({from, to});
	}
	std::sort(result.begin(), result.end(), [](const TlSpan &a, const TlSpan &b) {
		return a.fromMs < b.fromMs || (a.fromMs == b.fromMs && a.toMs < b.toMs);
	});
	QVector<TlSpan> merged;
	for (const TlSpan &range : result) {
		if (!merged.isEmpty() && range.fromMs <= merged.last().toMs)
			merged.last().toMs = std::max(merged.last().toMs, range.toMs);
		else
			merged.append(range);
	}
	return merged;
}

qint64 mapTime(qint64 oldMs, const QVector<TlSpan> &ranges)
{
	oldMs = std::max<qint64>(0, oldMs);
	qint64 removed = 0;
	for (const TlSpan &range : ranges) {
		if (oldMs <= range.fromMs)
			break;
		removed += std::min(oldMs, range.toMs) - range.fromMs;
		if (oldMs < range.toMs)
			break;
	}
	return std::max<qint64>(0, oldMs - removed);
}

EditResult apply(const TimelineModel &model, const QVector<TlSpan> &ranges)
{
	EditResult result;
	result.model = model;
	const qint64 duration = model.durationMs();
	const QVector<TlSpan> removed = normalize(ranges, duration);
	if (removed.isEmpty())
		return result;
	qint64 removedMs = 0;
	for (const TlSpan &range : removed)
		removedMs += range.durationMs();
	if (removedMs >= duration) {
		result.error = QStringLiteral("The selected pauses would remove the entire timeline. Keep at least one audible section.");
		return result;
	}
	QVector<TlSpan> kept;
	qint64 cursor = 0;
	for (const TlSpan &range : removed) {
		if (range.fromMs > cursor)
			kept.append({cursor, range.fromMs});
		cursor = range.toMs;
	}
	// Material beyond an explicit output end is still editable. Ripple it with
	// the removed pauses instead of discarding it at the rendering boundary.
	const qint64 contentEnd = std::max(duration, model.contentDurationMs());
	if (cursor < contentEnd)
		kept.append({cursor, contentEnd});
	TimelineModel changed = model;
	bool anyFragment = false;
	for (int trackIndex = 0; trackIndex < model.tracks.size(); ++trackIndex) {
		const TlTrack &track = model.tracks[trackIndex];
		const QVector<qint64> overlaps = track.overlapsBefore();
		for (int clipIndex = 0; clipIndex < track.clips.size(); ++clipIndex) {
			const TlClip &clip = track.clips[clipIndex];
			if (track.locked && removed.first().fromMs < clip.outEndMs()) {
				result.error = QStringLiteral("%1 is locked. Unlock affected tracks before trimming silence so all video, audio and captions can stay synchronized.")
					.arg(trackLabel(track, trackIndex));
				return result;
			}
			if (clip.transition.enabled && overlaps[clipIndex] > 0) {
				const qint64 end = clip.outStartMs + overlaps[clipIndex];
				for (const TlSpan &range : removed)
					if (range.fromMs < end && range.toMs > clip.outStartMs) {
						result.error = QStringLiteral("A selected pause intersects a transition on %1. Keep that pause or remove the transition before trimming.")
							.arg(trackLabel(track, trackIndex));
						return result;
					}
			}
		}
		changed.tracks[trackIndex].clips.clear();
		for (const TlClip &clip : track.clips) {
			for (const TlSpan &window : kept) {
				const qint64 from = std::max(clip.outStartMs, window.fromMs);
				const qint64 to = std::min(clip.outEndMs(), window.toMs);
				if (to <= from)
					continue;
				const QString error = fragmentError(clip, from, to);
				if (!error.isEmpty()) {
					result.error = error;
					return result;
				}
				TlClip piece = fragment(clip, from, to, removed);
				if (piece.outDurationMs() != to - from) {
					result.error = QStringLiteral("A pause boundary cannot preserve exact timing at a clip's playback speed (%1×). Adjust the selected pause boundary or use normal speed before trimming.")
						.arg(clip.speed, 0, 'g', 4);
					return result;
				}
				anyFragment = anyFragment || from != clip.outStartMs || to != clip.outEndMs();
				changed.tracks[trackIndex].clips.append(piece);
			}
		}
	}
	if (anyFragment) {
		for (const TlSoundRule &rule : model.soundRules)
			if (rule.enabled) {
				result.error = QStringLiteral("Automatic sound rules are enabled. Disable them before trimming pauses inside clips so split fragments do not create extra sound effects.");
				return result;
			}
	}
	changed.markers.clear();
	changed.markerLabels.clear();
	for (qint64 marker : model.markers) {
		bool cut = false;
		for (const TlSpan &range : removed)
			if (marker >= range.fromMs && marker < range.toMs) {
				cut = true;
				break;
			}
		if (!cut) {
			const qint64 time = mapTime(marker, removed);
			changed.markers.append(time);
			if (model.markerLabels.contains(marker))
				changed.markerLabels.insert(time, model.markerLabels.value(marker));
		}
	}
	std::sort(changed.markers.begin(), changed.markers.end());
	changed.markers.erase(std::unique(changed.markers.begin(), changed.markers.end()), changed.markers.end());
	if (model.hasVideoEnd())
		changed.videoEndMs = mapTime(model.videoEndMs, removed);
	result.model = changed;
	result.removed = removed;
	result.removedMs = removedMs;
	return result;
}

} // namespace silence_trim
} // namespace harpia
