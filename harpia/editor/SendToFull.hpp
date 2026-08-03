#pragma once

// Handing a Trim or Multi-Cut result to Full editing.
//
// Multi-Cut is the fast way to get the shape of a video right; Full editing is
// where text, overlays, transitions and effects live. Without a bridge between
// them the only way to combine the two was to export from one and re-import
// into the other -- losing every cut boundary in the process, because an
// exported file is one flat clip.
//
// The conversion is pure and lives here so it can be checked without a window:
// the part that goes quietly wrong is the OUTPUT TIME arithmetic, where a
// mis-accumulated offset leaves gaps or overlaps that look like a decoding bug
// rather than an arithmetic one.
//
// What carries over, and why:
//   * ONE CLIP PER CUT, not one merged clip. The cut boundaries are the work
//     Multi-Cut just did; collapsing them would mean re-finding every edit
//     point by hand, and transitions need two adjacent clips to sit between.
//   * Each clip keeps its own source range, speed and source id, so a
//     multi-source Multi-Cut arrives intact.
//   * A Trim result is the same thing with one cut -- its in/out points and
//     its crop.
//   * Clips are APPENDED at `startAtMs`, never dropped on top of existing
//     work: sending twice piles up, which is visible and undoable, whereas
//     sending over something is not.

#include "TrackEditor.hpp"          // CutSegment
#include "timeline/TimelineModel.hpp" // TlClip

#include <QRect>
#include <QVector>

namespace harpia {

// Multi-Cut's segments as timeline clips, laid end to end starting at
// `startAtMs`. Empty in, empty out. Zero/negative-length cuts are skipped
// rather than emitted as clips no one can grab.
inline QVector<TlClip> clipsFromSegments(const QVector<CutSegment> &segs, qint64 startAtMs)
{
	QVector<TlClip> out;
	out.reserve(segs.size());
	qint64 at = std::max<qint64>(0, startAtMs);
	for (const CutSegment &s : segs) {
		if (s.srcEndMs <= s.srcStartMs)
			continue; // an empty cut is not a clip
		TlClip c;
		c.type = TlClip::Type::Video;
		c.sourceId = s.sourceId;
		c.srcStartMs = s.srcStartMs;
		c.srcEndMs = s.srcEndMs;
		c.speed = (s.speed > 0.01) ? s.speed : 1.0;
		c.outStartMs = at;
		// Butt-jointed: the next clip starts exactly where this one ends, so
		// the timeline plays back as the Multi-Cut output did, with no gap to
		// hunt for and no overlap that would read as an accidental transition.
		at += c.outDurationMs();
		out.append(c);
	}
	return out;
}

// A Trim result as a single clip: the in/out points, the speed, and the crop
// rectangle if one was set. The crop is a source-pixel rect in both models, so
// it transfers verbatim.
inline QVector<TlClip> clipsFromTrim(int sourceId, qint64 trimStartMs, qint64 trimEndMs, double speed,
				     bool cropEnabled, const QRect &cropRect, qint64 startAtMs)
{
	CutSegment s;
	s.sourceId = sourceId;
	s.srcStartMs = trimStartMs;
	s.srcEndMs = trimEndMs;
	s.speed = speed;
	QVector<TlClip> out = clipsFromSegments({s}, startAtMs);
	if (!out.isEmpty() && cropEnabled && !cropRect.isEmpty())
		out[0].crop = cropRect;
	return out;
}

// How long the sent material runs, for the "added N clips (M s)" report. Zero
// for an empty send.
inline qint64 sentDurationMs(const QVector<TlClip> &clips)
{
	qint64 total = 0;
	for (const TlClip &c : clips)
		total += c.outDurationMs();
	return total;
}

} // namespace harpia
