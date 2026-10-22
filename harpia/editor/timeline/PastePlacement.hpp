#pragma once

// Where a paste goes on the timeline: the "best fit" lane.
//
// The first lane of the clips' kind, top to bottom, that is empty for their
// whole span -- so nothing already on the timeline is covered or cut into. A
// locked lane is never written to, and the Subtitles lane is left to its
// captions. When no lane fits, a new one: a picture on top of the picture
// stack, so what was pasted shows over the footage (but under a Subtitles lane
// at the very top, which stays topmost so the captions stay readable), and a
// sound at the top of the audio lanes.
//
// Pure, on a TimelineModel, so it is tested without a window; TimelineView's
// placePasted() applies it as one undo step.

#include "TimelineModel.hpp"

#include <QColor>
#include <QVector>

#include <algorithm>

namespace harpia {

namespace paste_placement {

inline bool isSubtitlesLane(const TlTrack &t)
{
	return t.kind == TlTrack::Kind::Video && t.name == QLatin1String("Subtitles");
}

// Nothing on `t` overlaps [a, b).
inline bool laneFreeFor(const TlTrack &t, qint64 a, qint64 b)
{
	for (const TlClip &k : t.clips)
		if (k.outStartMs < b && k.outEndMs() > a)
			return false;
	return true;
}

// The lane `clips` would go on, or -1 when a new lane is needed.
inline int bestFitLane(const TimelineModel &m, TlTrack::Kind kind, const QVector<TlClip> &clips)
{
	if (clips.isEmpty())
		return -1;
	qint64 a = clips.first().outStartMs, b = clips.first().outEndMs();
	for (const TlClip &c : clips) {
		a = std::min(a, c.outStartMs);
		b = std::max(b, c.outEndMs());
	}
	for (int i = 0; i < m.tracks.size(); ++i) {
		const TlTrack &t = m.tracks[i];
		if (t.kind == kind && !t.locked && !isSubtitlesLane(t) && laneFreeFor(t, a, b))
			return i;
	}
	return -1;
}

// Where a new lane of `kind` goes: the top of its group.
inline int newLaneIndex(const TimelineModel &m, TlTrack::Kind kind)
{
	if (!TimelineModel::isPictureKind(kind))
		return m.pictureTrackCount();
	int at = 0;
	while (at < m.tracks.size() && isSubtitlesLane(m.tracks[at]))
		++at;
	return at;
}

// Put `clips` on their best-fit lane in `m`, in order, making the lane when
// none fits. Returns the lane, or -1 for no clips.
inline int place(TimelineModel &m, TlTrack::Kind kind, const QVector<TlClip> &clips, const QColor &newLaneColor)
{
	if (clips.isEmpty())
		return -1;
	int lane = bestFitLane(m, kind, clips);
	if (lane < 0) {
		TlTrack t;
		t.kind = kind;
		t.color = newLaneColor;
		t.autoLane = true; // made by the paste, tidied away if its clips leave
		lane = newLaneIndex(m, kind);
		m.tracks.insert(lane, t);
	}
	for (const TlClip &c : clips)
		m.tracks[lane].clips.append(c);
	return lane;
}

} // namespace paste_placement

} // namespace harpia
