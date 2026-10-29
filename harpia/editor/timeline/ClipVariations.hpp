#pragma once

// Random clips: one slot on the timeline, several pieces of footage for it --
// one per version of the video, the way a Random text has its texts.
//
// The slot is the clip: its place, its length, its framing, effects, keys and
// fades are set once and shared. Each version chooses its file, source start
// and an independent canvas position offset (TlClipOption). The clip plays one of
// them (TlClip::clipShown); every rule below keeps that one and the list in
// step, so the rest of the editor -- the timeline, the preview, Slip, the
// exporter -- sees an ordinary clip.
//
// Pure functions over a TlClip, so every rule is checkable without a window.

#include "TimelineModel.hpp"

#include <QString>

#include <algorithm>
#include <functional>
#include <limits>

namespace harpia {

namespace clip_variations {

// Write the clip's own footage back into its shown entry.
inline void syncShown(TlClip &c)
{
	if (!c.isRandomClip())
		return;
	c.clipShown = std::clamp(c.clipShown, 0, int(c.clipOptions.size()) - 1);
	c.clipOptions[c.clipShown].sourceId = c.sourceId;
	c.clipOptions[c.clipShown].srcStartMs = c.srcStartMs;
}

// An ordinary media clip becomes a Random clip of one: itself.
inline void makeRandom(TlClip &c)
{
	if (c.isRandomClip())
		return;
	c.clipOptions = {{c.sourceId, c.srcStartMs}};
	c.clipShown = 0;
}

// Back to an ordinary clip, playing whatever it shows now.
inline void makeOrdinary(TlClip &c)
{
	const QPointF offset = c.optionPositionOffset();
	c.posX += offset.x();
	c.posY += offset.y();
	for (TlKeyframe &key : c.keys) {
		key.tf.posX += offset.x();
		key.tf.posY += offset.y();
	}
	c.clipOptions.clear();
	c.clipShown = 0;
}

// Play choice `k`: same place, same length, its file from its start.
inline bool show(TlClip &c, int k)
{
	if (!c.isRandomClip() || k < 0 || k >= c.clipOptions.size())
		return false;
	if (k == c.clipShown)
		return true;
	syncShown(c);
	const qint64 len = c.srcLenMs();
	c.sourceId = c.clipOptions[k].sourceId;
	c.srcStartMs = c.clipOptions[k].srcStartMs;
	c.srcEndMs = c.srcStartMs + len;
	c.clipShown = k;
	c.peaks.clear(); // a waveform belongs to the file it was read from
	return true;
}

// Whether footage `totalMs` long can fill the slot. `why` says why not.
inline bool fits(const TlClip &c, qint64 totalMs, QString *why = nullptr)
{
	if (totalMs <= 0 || totalMs >= c.srcLenMs())
		return true;
	if (why)
		*why = QStringLiteral("It is %1 s long, and this clip needs %2 s. Every clip in a Random clip "
				      "plays for the same time.")
			       .arg(totalMs / 1000.0, 0, 'f', 1)
			       .arg(c.srcLenMs() / 1000.0, 0, 'f', 1);
	return false;
}

// Add footage from `sourceId` (`totalMs` long), from its start. Returns its
// index, or -1 with `why` when it is too short to fill the slot.
inline int add(TlClip &c, int sourceId, qint64 totalMs, QString *why = nullptr)
{
	if (!fits(c, totalMs, why))
		return -1;
	makeRandom(c);
	c.clipOptions.append({sourceId, 0});
	return int(c.clipOptions.size()) - 1;
}

// Replace one choice's source and moment without changing which choice is
// showing. A known source length lets the Slip picker keep the entire shared
// window inside the file. All validation happens before syncShown mutates the
// cached list, so a refused choice leaves the clip exactly as it was.
inline bool setOption(TlClip &c, int k, int sourceId, qint64 startMs, qint64 totalMs,
		      QString *why = nullptr)
{
	if (why)
		why->clear();
	const auto no = [why](const QString &reason) {
		if (why)
			*why = reason;
		return false;
	};
	if (!c.isRandomClip() || k < 0 || k >= c.clipOptions.size())
		return no(QStringLiteral("This Random clip option no longer exists."));
	if (sourceId < 0 || totalMs <= 0)
		return no(QStringLiteral("Choose a readable video or audio file with a known duration."));
	// These checks also make end - start safe for malformed input: both are
	// non-negative and end is no smaller than start.
	if (c.srcStartMs < 0 || c.srcEndMs <= c.srcStartMs)
		return no(QStringLiteral("This clip has no source duration to preserve."));
	const qint64 length = c.srcEndMs - c.srcStartMs;
	if (!fits(c, totalMs, why))
		return false;
	const qint64 start = std::clamp<qint64>(startMs, 0, totalMs - length);
	syncShown(c);
	c.clipOptions[k].sourceId = sourceId;
	c.clipOptions[k].srcStartMs = start;
	if (k == c.clipShown) {
		if (c.sourceId != sourceId)
			c.peaks.clear();
		c.sourceId = sourceId;
		c.srcStartMs = start;
		c.srcEndMs = start + length; // bounded above by totalMs
	}
	return true;
}

// Remove choice `k`. The last one stays (Make normal clip is how a Random
// clip stops being one); removing the shown one shows its neighbour.
inline bool remove(TlClip &c, int k)
{
	if (!c.isRandomClip() || c.clipOptions.size() <= 1 || k < 0 || k >= c.clipOptions.size())
		return false;
	syncShown(c);
	const bool wasShown = k == c.clipShown;
	c.clipOptions.removeAt(k);
	if (c.clipShown > k)
		--c.clipShown;
	if (wasShown) {
		const int next = std::min(k, int(c.clipOptions.size()) - 1);
		const qint64 len = c.srcLenMs();
		c.clipShown = next;
		c.sourceId = c.clipOptions[next].sourceId;
		c.srcStartMs = c.clipOptions[next].srcStartMs;
		c.srcEndMs = c.srcStartMs + len;
		c.peaks.clear();
	}
	return true;
}

// Trimming the slot's head trims every choice the same way: each starts
// `deltaMs` later (or earlier). The shown one moves with the clip itself.
inline void shiftOthers(TlClip &c, qint64 deltaMs)
{
	for (int k = 0; k < c.clipOptions.size(); ++k)
		if (k != c.clipShown)
			c.clipOptions[k].srcStartMs = std::max<qint64>(0, c.clipOptions[k].srcStartMs + deltaMs);
}

// How far the slot may grow, by its end and by its head, so that every other
// choice still has footage for all of it. `totalOf` gives a file's length
// (0 = unknown, which limits nothing).
struct Room {
	qint64 tailMs = std::numeric_limits<qint64>::max() / 4; // beyond the current end
	qint64 headMs = std::numeric_limits<qint64>::max() / 4; // before the current start
};
inline Room roomOfOthers(const TlClip &c, const std::function<qint64(int)> &totalOf)
{
	Room r;
	for (int k = 0; k < c.clipOptions.size(); ++k) {
		if (k == c.clipShown)
			continue;
		const TlClipOption &o = c.clipOptions[k];
		r.headMs = std::min(r.headMs, o.srcStartMs);
		const qint64 total = totalOf ? totalOf(o.sourceId) : 0;
		if (total > 0)
			r.tailMs = std::min(r.tailMs, std::max<qint64>(0, total - (o.srcStartMs + c.srcLenMs())));
	}
	return r;
}

// "beach.mp4 @ 5.0s": what a choice is called in the list and in file names.
inline QString label(const QString &fileName, const TlClipOption &o)
{
	return QStringLiteral("%1 @ %2s").arg(fileName).arg(o.srcStartMs / 1000.0, 0, 'f', 1);
}

} // namespace clip_variations

} // namespace harpia
