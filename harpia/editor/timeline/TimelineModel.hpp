#pragma once

// Data model for the "Full editing" multi-track timeline (an NLE-style stacked
// set of video + audio tracks). Unlike the sequential Multi-Cut model
// (CutSegment), every clip here carries an EXPLICIT output position
// (outStartMs), so clips can sit anywhere with gaps, and video tracks stack and
// composite (top track over lower ones).
//
// GL-free and libav-free so the widget, the compositor, the window, and the
// exporter can all share it.

#include <QRect>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace harpia {

// One clip placed on a track. Video clips use the transform fields; audio clips
// use volume/fades (+ cached peaks for the waveform). `sourceId` refers to an
// EditorSource in the window's media pool.
struct TlClip {
	int sourceId = 0;
	qint64 srcStartMs = 0;
	qint64 srcEndMs = 0;
	double speed = 1.0;
	qint64 outStartMs = 0; // position on the output timeline

	// Video-only transform, relative to the output canvas.
	double posX = 0.5;   // centre X (0..1 across the canvas)
	double posY = 0.5;   // centre Y (0..1 down the canvas)
	double scale = 1.0;  // 1 = letterbox-fit to the canvas; <1 = picture-in-picture
	double opacity = 1.0;
	QRect crop;          // source-pixel crop (null/empty = whole frame)

	// Audio-only.
	double volume = 1.0;
	int fadeInMs = 15;
	int fadeOutMs = 15;
	QVector<float> peaks; // whole-source amplitude buckets (for the waveform)

	qint64 srcLenMs() const { return std::max<qint64>(0, srcEndMs - srcStartMs); }
	qint64 outDurationMs() const
	{
		const double sp = speed > 0.01 ? speed : 1.0;
		return std::max<qint64>(1, qint64(std::llround(double(srcLenMs()) / sp)));
	}
	qint64 outEndMs() const { return outStartMs + outDurationMs(); }
	bool coversOutput(qint64 ms) const { return ms >= outStartMs && ms < outEndMs(); }
	// Source-time under an output-time position inside this clip.
	qint64 srcAtOutput(qint64 outMs) const
	{
		const qint64 off = std::clamp<qint64>(outMs - outStartMs, 0, outDurationMs());
		return srcStartMs + qint64(std::llround(double(off) * (speed > 0.01 ? speed : 1.0)));
	}

	bool operator==(const TlClip &o) const
	{
		return sourceId == o.sourceId && srcStartMs == o.srcStartMs && srcEndMs == o.srcEndMs &&
		       speed == o.speed && outStartMs == o.outStartMs && posX == o.posX && posY == o.posY &&
		       scale == o.scale && opacity == o.opacity && crop == o.crop && volume == o.volume &&
		       fadeInMs == o.fadeInMs && fadeOutMs == o.fadeOutMs;
	}
};

struct TlTrack {
	enum class Kind { Video, Audio };
	Kind kind = Kind::Video;
	QString name;
	bool muted = false;
	QVector<TlClip> clips; // unordered; painting/compositing sorts by outStartMs

	// The clip covering an output-time position (topmost = last added wins on
	// overlap). Returns -1 when none.
	int clipAt(qint64 outMs) const
	{
		for (int i = clips.size() - 1; i >= 0; --i)
			if (clips[i].coversOutput(outMs))
				return i;
		return -1;
	}

	bool operator==(const TlTrack &o) const
	{
		return kind == o.kind && name == o.name && muted == o.muted && clips == o.clips;
	}
};

// The whole timeline. Video tracks come first (index = compositing z-order; a
// later index is drawn on top), then audio tracks.
struct TimelineModel {
	QVector<TlTrack> tracks;

	qint64 durationMs() const
	{
		qint64 d = 0;
		for (const TlTrack &t : tracks)
			for (const TlClip &c : t.clips)
				d = std::max(d, c.outEndMs());
		return d;
	}

	bool isEmpty() const
	{
		for (const TlTrack &t : tracks)
			if (!t.clips.isEmpty())
				return false;
		return true;
	}

	bool operator==(const TimelineModel &o) const { return tracks == o.tracks; }
	bool operator!=(const TimelineModel &o) const { return !(*this == o); }
};

} // namespace harpia
