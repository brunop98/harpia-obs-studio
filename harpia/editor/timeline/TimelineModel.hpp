#pragma once

// Data model for the "Full editing" multi-track timeline (an NLE-style stacked
// set of video + audio tracks). Unlike the sequential Multi-Cut model
// (CutSegment), every clip here carries an EXPLICIT output position
// (outStartMs), so clips can sit anywhere with gaps, and video tracks stack and
// composite (top track over lower ones).
//
// GL-free and libav-free so the widget, the compositor, the window, and the
// exporter can all share it.

#include <QColor>
#include <QRect>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace harpia {

// The animatable pose of a clip on the output canvas. Resolved per output frame
// (from the clip's base pose, or interpolated between its keyframes).
struct TlTransform {
	double posX = 0.5;  // centre X (0..1 across the canvas)
	double posY = 0.5;  // centre Y (0..1 down the canvas)
	double scale = 1.0; // 1 = fit the canvas; >1 zooms in; <1 = picture-in-picture
	double opacity = 1.0;
};

// One animation keyframe: a pose pinned to a time inside the clip. `tMs` is an
// offset from the clip's outStartMs, in OUTPUT time. `ease` shapes the curve
// from this keyframe to the next.
struct TlKeyframe {
	qint64 tMs = 0;
	TlTransform tf;
	enum class Ease { Linear, EaseInOut };
	Ease ease = Ease::EaseInOut;

	bool operator==(const TlKeyframe &o) const
	{
		return tMs == o.tMs && tf.posX == o.tf.posX && tf.posY == o.tf.posY &&
		       tf.scale == o.tf.scale && tf.opacity == o.tf.opacity && ease == o.ease;
	}
};

// Styling for a text clip. Rendered by TimelineCompositor with QPainter, so the
// preview and the exported frames use identical text.
struct TlText {
	QString text = QStringLiteral("Text");
	QString fontFamily;          // empty = the app's default family
	int fontPx = 64;             // at scale 1.0, relative to a 1080-tall canvas
	bool bold = false;
	bool italic = false;
	QColor color = QColor(0xff, 0xff, 0xff);
	double outlineWidth = 3.0;   // 0 = no outline
	QColor outlineColor = QColor(0x00, 0x00, 0x00);
	bool boxEnabled = false;     // caption-style background box
	QColor boxColor = QColor(0, 0, 0, 150);
	int boxPadding = 14;
	int boxRadius = 6;
	int align = 1; // 0 = left, 1 = centre, 2 = right (multi-line blocks)

	bool operator==(const TlText &o) const
	{
		return text == o.text && fontFamily == o.fontFamily && fontPx == o.fontPx &&
		       bold == o.bold && italic == o.italic && color == o.color &&
		       outlineWidth == o.outlineWidth && outlineColor == o.outlineColor &&
		       boxEnabled == o.boxEnabled && boxColor == o.boxColor &&
		       boxPadding == o.boxPadding && boxRadius == o.boxRadius && align == o.align;
	}
};

// One clip placed on a track. Video and Text clips use the transform/keyframes;
// audio clips use volume/fades (+ cached peaks for the waveform). `sourceId`
// refers to an EditorSource in the window's media pool (unused for Text).
struct TlClip {
	enum class Type { Video, Text };
	Type type = Type::Video;

	int sourceId = 0;
	qint64 srcStartMs = 0;
	qint64 srcEndMs = 0;
	double speed = 1.0;
	qint64 outStartMs = 0; // position on the output timeline

	// Base pose (used when there are no keyframes, and as the seed for the first
	// keyframe). Kept as flat fields so existing code and persistence stay simple.
	double posX = 0.5;
	double posY = 0.5;
	double scale = 1.0;
	double opacity = 1.0;
	QRect crop;          // source-pixel crop (null/empty = whole frame)

	// Zoom/position animation. Empty = the static base pose above. Sorted by tMs.
	QVector<TlKeyframe> keys;

	// Text clips only.
	TlText text;

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

	TlTransform baseTransform() const { return TlTransform{posX, posY, scale, opacity}; }
	void setBaseTransform(const TlTransform &t)
	{
		posX = t.posX;
		posY = t.posY;
		scale = t.scale;
		opacity = t.opacity;
	}

	// The pose at an OUTPUT-time position: the base pose when un-keyframed, else
	// the interpolated keyframe track (clamped to the first/last key outside the
	// keyed range).
	TlTransform transformAt(qint64 outMs) const
	{
		if (keys.isEmpty())
			return baseTransform();
		const qint64 t = outMs - outStartMs;
		if (keys.size() == 1 || t <= keys.front().tMs)
			return keys.front().tf;
		if (t >= keys.back().tMs)
			return keys.back().tf;
		int i = 0;
		while (i + 1 < keys.size() && keys[i + 1].tMs <= t)
			++i;
		const TlKeyframe &a = keys[i];
		const TlKeyframe &b = keys[i + 1];
		const qint64 span = std::max<qint64>(1, b.tMs - a.tMs);
		double u = std::clamp(double(t - a.tMs) / double(span), 0.0, 1.0);
		if (a.ease == TlKeyframe::Ease::EaseInOut)
			u = u * u * (3.0 - 2.0 * u); // smoothstep
		auto mix = [u](double p, double q) { return p + (q - p) * u; };
		return TlTransform{mix(a.tf.posX, b.tf.posX), mix(a.tf.posY, b.tf.posY),
				   mix(a.tf.scale, b.tf.scale), mix(a.tf.opacity, b.tf.opacity)};
	}

	// Insert (or replace) a keyframe at an output-time position. The first
	// keyframe added seeds from the base pose so nothing jumps.
	void setKeyframeAt(qint64 outMs, const TlTransform &tf,
			   TlKeyframe::Ease ease = TlKeyframe::Ease::EaseInOut)
	{
		const qint64 t = std::clamp<qint64>(outMs - outStartMs, 0, outDurationMs());
		for (TlKeyframe &k : keys) {
			if (std::llabs(k.tMs - t) <= 1) { // replace the one at this time
				k.tf = tf;
				return;
			}
		}
		TlKeyframe k;
		k.tMs = t;
		k.tf = tf;
		k.ease = ease;
		int at = 0;
		while (at < keys.size() && keys[at].tMs < t)
			++at;
		keys.insert(at, k);
	}

	int keyframeIndexAt(qint64 outMs, qint64 tolMs = 40) const
	{
		const qint64 t = outMs - outStartMs;
		for (int i = 0; i < keys.size(); ++i)
			if (std::llabs(keys[i].tMs - t) <= tolMs)
				return i;
		return -1;
	}

	bool operator==(const TlClip &o) const
	{
		return type == o.type && sourceId == o.sourceId && srcStartMs == o.srcStartMs &&
		       srcEndMs == o.srcEndMs && speed == o.speed && outStartMs == o.outStartMs &&
		       posX == o.posX && posY == o.posY && scale == o.scale && opacity == o.opacity &&
		       crop == o.crop && keys == o.keys && text == o.text && volume == o.volume &&
		       fadeInMs == o.fadeInMs && fadeOutMs == o.fadeOutMs;
	}
};

struct TlTrack {
	enum class Kind { Video, Audio };
	Kind kind = Kind::Video;
	QString name;
	// Per-track switches. `hidden` only applies to video (keeps the audio), and
	// `muted` silences the track's audio (a video track has both). `locked`
	// blocks every edit but still previews/renders. `ripple` closes the gap when
	// a clip is deleted from this track.
	bool muted = false;
	bool hidden = false;
	bool locked = false;
	bool ripple = false;
	QColor color = QColor(0x3a, 0x6e, 0xa5); // clip tint (random pastel when created)
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
		return kind == o.kind && name == o.name && muted == o.muted && hidden == o.hidden &&
		       locked == o.locked && ripple == o.ripple && color == o.color && clips == o.clips;
	}
};

// The whole timeline. Video tracks come first, then audio tracks. Index order IS
// the display order (index 0 is the top lane) and, for video, the compositing
// order: the HIGHER lane renders in FRONT, so index 0 is drawn last/on top.
struct TimelineModel {
	QVector<TlTrack> tracks;

	int videoTrackCount() const
	{
		int n = 0;
		for (const TlTrack &t : tracks)
			if (t.kind == TlTrack::Kind::Video)
				++n;
		return n;
	}

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
