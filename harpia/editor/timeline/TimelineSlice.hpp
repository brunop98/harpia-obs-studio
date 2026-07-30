#pragma once

// Cut a sub-range out of a timeline and return it as a timeline in its own
// right, starting at zero.
//
// This is how "export just this part" works. The alternative -- teaching the
// exporter a start/end window -- would have meant a range parameter threaded
// through runTimeline, the audio mixer, the size estimate and the progress
// arithmetic, with four separate chances to disagree about where the range
// begins. Reshaping the MODEL instead means every one of those paths runs
// exactly as it does for a whole-project export, because as far as they can
// tell, it is one.
//
// The contract: for any t in [0, to-from), the sliced timeline at t renders
// what the original rendered at from+t. Everything below exists to hold that
// property -- the source ranges, the three separate keyframe tracks (transform,
// effect, component), and the audio fades all measure from a clip's start, so
// trimming a clip's head has to move them with it.
//
// Two things it deliberately does NOT preserve, because the model cannot say
// them:
//   - a fade that the range cuts INTO restarts from silence rather than
//     resuming partway, since a fade has a length but no offset;
//   - a transition the range starts inside restarts, since transition progress
//     is measured from the incoming clip's start and that start has just moved.
// Both are only reachable by putting a range boundary inside a fade or a
// transition, and both are pinned by timelineslice_test so they stay known
// rather than becoming a surprise.
//
// Header-only and dependency-free beyond the model itself, so the widget, the
// window, the exporter and the tests all share one copy.

#include "TimelineModel.hpp"

#include <algorithm>
#include <cmath>

namespace harpia {

// The span a set of clips occupies on the output timeline: the earliest start
// and the latest end. Returns {-1, -1} when `pairs` names nothing.
//
// Free-standing because both the widget (what is selected) and the window (what
// to offer in the export dialog) ask it, and a union of half-open ranges is not
// a thing worth writing twice.
struct TlSpan {
	qint64 fromMs = -1;
	qint64 toMs = -1;

	bool isValid() const { return fromMs >= 0 && toMs > fromMs; }
	qint64 durationMs() const { return isValid() ? toMs - fromMs : 0; }
	bool operator==(const TlSpan &o) const { return fromMs == o.fromMs && toMs == o.toMs; }
};

// Slice `src` down to the output-time window [fromMs, toMs), rebased to 0.
//
// Tracks are all kept, empty or not. Not for the render's sake -- an empty lane
// composites to nothing wherever it sits, and the negative control for this in
// timelineslice_test leaves every frame comparison passing -- but because the
// result is a TimelineModel like any other, and its lanes are part of what it
// is. A slice with lanes silently missing would be a different edit.
inline TimelineModel sliceTimeline(const TimelineModel &src, qint64 fromMs, qint64 toMs)
{
	TimelineModel out;
	fromMs = std::max<qint64>(0, fromMs);
	if (toMs <= fromMs)
		return out; // an empty window is empty, not the whole project

	out.tracks.reserve(src.tracks.size());
	for (const TlTrack &t : src.tracks) {
		TlTrack nt = t;
		nt.clips.clear();
		for (const TlClip &c : t.clips) {
			const qint64 s = std::max(c.outStartMs, fromMs);
			const qint64 e = std::min(c.outEndMs(), toMs);
			if (e <= s)
				continue; // entirely outside the window
			const qint64 head = s - c.outStartMs; // output ms cut off the front
			const qint64 tail = c.outEndMs() - e; // ...and off the back

			TlClip nc = c;
			nc.outStartMs = s - fromMs;
			if (c.freeDuration()) {
				// A still, a caption or an effect has no source clock --
				// its src range only ever encodes a length. Same rewrite
				// splitAtPlayhead does to the halves it makes.
				const double sp = c.speed > 0.01 ? c.speed : 1.0;
				nc.srcStartMs = 0;
				nc.srcEndMs = std::max<qint64>(1, qint64(std::llround(double(e - s) * sp)));
			} else {
				// srcAtOutput already folds in speed and clamps to the
				// clip, so the two ends cannot drift apart from the way
				// the compositor reads the same clip.
				nc.srcStartMs = c.srcAtOutput(s);
				nc.srcEndMs = c.srcAtOutput(e);
			}

			if (head > 0) {
				// All three keyframe tracks are measured from the clip's
				// own start, so cutting the head off has to slide them
				// back by the same amount or every animation jumps.
				//
				// Keys that fall outside the surviving range are KEPT
				// rather than dropped: a channel holds its first/last key
				// outside the keyed span, so the key just before the cut
				// is what makes the pose at the cut correct.
				for (TlKeyframe &k : nc.keys)
					k.tMs -= head;
				for (FxKey &k : nc.fx.keys)
					k.tMs -= head;
				for (auto it = nc.components.begin(); it != nc.components.end(); ++it)
					for (auto kit = it->keys.begin(); kit != it->keys.end(); ++kit)
						for (PropKey &pk : kit.value())
							pk.tMs -= head;

				// A fade the cut lands past is gone entirely (exact); one
				// the cut lands inside keeps what is left of it, which
				// restarts from silence instead of resuming partway --
				// the model has no offset to say otherwise.
				nc.fadeInMs = int(std::max<qint64>(0, nc.fadeInMs - head));
			}
			if (tail > 0)
				nc.fadeOutMs = int(std::max<qint64>(0, nc.fadeOutMs - tail));
			nc.clampFades();
			nt.clips.append(nc);
		}
		out.tracks.append(nt);
	}

	for (qint64 m : src.markers)
		if (m >= fromMs && m < toMs)
			out.markers.append(m - fromMs);

	return out;
}

} // namespace harpia
