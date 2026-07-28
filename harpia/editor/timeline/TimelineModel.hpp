#pragma once

// Data model for the "Full editing" multi-track timeline (an NLE-style stacked
// set of video + audio tracks). Unlike the sequential Multi-Cut model
// (CutSegment), every clip here carries an EXPLICIT output position
// (outStartMs), so clips can sit anywhere with gaps, and video tracks stack and
// composite (top track over lower ones).
//
// GL-free and libav-free so the widget, the compositor, the window, and the
// exporter can all share it.

#include "../FadeCurve.hpp"

#include <QColor>
#include <QMap>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

namespace harpia {

// The animatable pose of a clip on the output canvas. Resolved per output frame
// (from the clip's base pose, or interpolated between its keyframes).
struct TlTransform {
	double posX = 0.5;    // centre X (0..1 across the canvas)
	double posY = 0.5;    // centre Y (0..1 down the canvas)
	double scale = 1.0;   // 1 = fit the canvas; >1 zooms in; <1 = picture-in-picture
	double rotation = 0.0; // degrees clockwise, about the clip's own centre
	double opacity = 1.0;
};

// One animation keyframe: a pose pinned to a time inside the clip. `tMs` is an
// offset from the clip's outStartMs, in OUTPUT time. `ease` shapes the curve
// from this keyframe to the next.
// How a keyframe hands over to the next one, per channel.
enum class TlEase { Linear, EaseIn, EaseOut, EaseInOut, Bezier };

inline constexpr int kTlEaseCount = 5;

inline const char *tlEaseName(TlEase e)
{
	switch (e) {
	case TlEase::Linear: return "Linear";
	case TlEase::EaseIn: return "Ease In";
	case TlEase::EaseOut: return "Ease Out";
	case TlEase::EaseInOut: return "Ease In-Out";
	case TlEase::Bezier: return "Bezier";
	}
	return "Linear";
}

inline TlEase tlEaseFromInt(int v)
{
	return (v >= 0 && v < kTlEaseCount) ? TlEase(v) : TlEase::Linear;
}

// Remap 0..1 progress through an ease. `p1`/`p2` are the Bezier handles (the
// x of a standard CSS-style cubic-bezier(p1, p1, p2, p2) with the control
// points on the diagonal, which is the shape a two-number handle can describe).
inline double tlEaseAt(TlEase e, double u, double p1 = 0.42, double p2 = 0.58)
{
	u = std::clamp(u, 0.0, 1.0);
	switch (e) {
	case TlEase::Linear:
		return u;
	case TlEase::EaseIn:
		return u * u;
	case TlEase::EaseOut:
		return 1.0 - (1.0 - u) * (1.0 - u);
	case TlEase::EaseInOut:
		return u * u * (3.0 - 2.0 * u); // smoothstep
	case TlEase::Bezier: {
		// Cubic Bezier with control points (p1,p1) and (p2,p2): symmetric in x
		// and y, so y(u) is just the curve evaluated at u -- no root solve.
		const double a = std::clamp(p1, 0.0, 1.0), b = std::clamp(p2, 0.0, 1.0);
		const double v = 1.0 - u;
		return 3.0 * v * v * u * a + 3.0 * v * u * u * b + u * u * u;
	}
	}
	return u;
}

// One channel's presence at a keyframe. A key does not have to pin every
// channel: the Position tab can hold a key at 1s that Scale knows nothing
// about, and each channel interpolates only across the keys that carry it.
struct TlKeyChannel {
	bool on = true;
	TlEase ease = TlEase::EaseInOut;
	double bez1 = 0.42, bez2 = 0.58; // Bezier handles (TlEase::Bezier only)

	bool operator==(const TlKeyChannel &o) const
	{
		return on == o.on && ease == o.ease && bez1 == o.bez1 && bez2 == o.bez2;
	}
};

struct TlKeyframe {
	qint64 tMs = 0;
	TlTransform tf;
	// Which channels this key pins, and how each leaves it. Defaulting all four
	// to on keeps a key created the old way (one pose, one ease) behaving
	// exactly as it did.
	TlKeyChannel pos, scale, rot, opacity;

	// The channel record for a lane index (0 = position, 1 = scale, 2 = rotation,
	// 3 = opacity), so the editor can drive all four through one code path.
	TlKeyChannel &channel(int lane)
	{
		switch (lane) {
		case 1: return scale;
		case 2: return rot;
		case 3: return opacity;
		default: return pos;
		}
	}
	const TlKeyChannel &channel(int lane) const
	{
		return const_cast<TlKeyframe *>(this)->channel(lane);
	}

	bool operator==(const TlKeyframe &o) const
	{
		return tMs == o.tMs && tf.posX == o.tf.posX && tf.posY == o.tf.posY &&
		       tf.scale == o.tf.scale && tf.rotation == o.tf.rotation &&
		       tf.opacity == o.tf.opacity && pos == o.pos && scale == o.scale &&
		       rot == o.rot && opacity == o.opacity;
	}
};

// The animatable channels, in the order the keyframe editor shows them.
enum TlLane { TlLanePos = 0, TlLaneScale = 1, TlLaneRot = 2, TlLaneOpacity = 3 };
inline constexpr int kTlLaneCount = 4;
inline const char *tlLaneName(int lane)
{
	switch (lane) {
	case TlLaneScale: return "Scale";
	case TlLaneRot: return "Rotation";
	case TlLaneOpacity: return "Opacity";
	default: return "Position";
	}
}

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

// One entry in a clip's transform-script stack: which script (file stem in the
// user's scripts folder) and the //@param values it was given.
struct TlScript {
	QString name;
	QMap<QString, double> params;

	bool operator==(const TlScript &o) const { return name == o.name && params == o.params; }
	bool operator!=(const TlScript &o) const { return !(*this == o); }
};

// Rebuild a script stack from list-widget labels of the form "<n>.  <name>",
// where <n> is the entry's ORIGINAL 1-based position. Reordering a list widget
// moves the labels with the rows, so the numbers say where each row came from.
// Returns an empty vector if the labels don't describe a permutation of
// `current` — callers must then leave the stack alone rather than scramble it.
inline QVector<TlScript> reorderScriptsByLabel(const QStringList &labels,
					       const QVector<TlScript> &current)
{
	if (labels.size() != current.size())
		return {};
	QVector<TlScript> out;
	out.reserve(current.size());
	QVector<bool> used(current.size(), false);
	for (const QString &label : labels) {
		bool ok = false;
		const int from = label.section(QLatin1Char('.'), 0, 0).trimmed().toInt(&ok) - 1;
		if (!ok || from < 0 || from >= current.size() || used[from])
			return {}; // unparseable, out of range, or a duplicate
		used[from] = true;
		out.append(current[from]);
	}
	return out;
}

// One clip placed on a track. Video and Text clips use the transform/keyframes;
// audio clips use volume/fades (+ cached peaks for the waveform). `sourceId`
// refers to an EditorSource in the window's media pool (unused for Text).
struct TlClip {
	// Video = a decoded media clip (also used for audio-track clips); Image = a
	// still whose duration is free; Text = a rendered caption.
	enum class Type { Video, Text, Image };
	Type type = Type::Video;

	// Stills and captions have no source timeline, so their length is whatever
	// the user drags it to rather than being capped by a decoder.
	bool freeDuration() const { return type == Type::Text || type == Type::Image; }

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
	double rotation = 0.0; // degrees clockwise
	double opacity = 1.0;
	QRect crop;          // source-pixel crop (null/empty = whole frame)

	// Zoom/position animation. Empty = the static base pose above. Sorted by tMs.
	QVector<TlKeyframe> keys;

	// Text clips only.
	TlText text;

	// Transform scripts, applied in order. Each one starts from what the previous
	// left behind, so a script that only defines scale() composes with one that
	// only defines position(); where two define the same channel, the later entry
	// wins. Channels no script defines fall through to the pose/keyframes above.
	QVector<TlScript> scripts;

	// Audio-only. The fades are non-destructive: nothing is written back to the
	// media, they are evaluated as a gain at play/render time.
	double volume = 1.0;
	int fadeInMs = 15;
	int fadeOutMs = 15;
	FadeCurve fadeInCurve = FadeCurve::Linear;
	FadeCurve fadeOutCurve = FadeCurve::Linear;
	QVector<float> peaks; // whole-source amplitude buckets (for the waveform)

	// A fade can never be longer than the clip it lives on — trimming a clip
	// shorter has to pull its fades in with it, or the gain would never reach
	// full and the clip would sound broken for no visible reason.
	void clampFades()
	{
		const int dur = int(std::min<qint64>(outDurationMs(), std::numeric_limits<int>::max()));
		fadeInMs = std::clamp(fadeInMs, 0, dur);
		fadeOutMs = std::clamp(fadeOutMs, 0, dur);
	}

	// Gain this clip's fades apply at `outMs`. Both fades are multiplied, so a
	// clip too short to hold them separately dips in the middle instead of
	// jumping — smooth either way, never a click.
	double fadeGainAt(qint64 outMs) const
	{
		const qint64 off = outMs - outStartMs;
		const qint64 dur = outDurationMs();
		if (off < 0 || off > dur)
			return 0.0;
		double g = 1.0;
		if (fadeInMs > 0 && off < fadeInMs)
			g *= fadeGain(fadeInCurve, double(off) / double(fadeInMs));
		if (fadeOutMs > 0 && off > dur - fadeOutMs)
			g *= fadeGain(fadeOutCurve, double(dur - off) / double(fadeOutMs));
		return std::clamp(g, 0.0, 1.0);
	}

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

	TlTransform baseTransform() const
	{
		return TlTransform{posX, posY, scale, rotation, opacity};
	}
	void setBaseTransform(const TlTransform &t)
	{
		posX = t.posX;
		posY = t.posY;
		scale = t.scale;
		rotation = t.rotation;
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
		TlTransform out = baseTransform();

		// Each channel walks only the keys that actually pin it, so a Position
		// key at 1s and a Scale key at 3s animate independently instead of one
		// dragging the other along. Outside a channel's keyed range it holds the
		// nearest key, as before.
		auto solve = [&](int lane, double TlTransform::*field) {
			int first = -1, last = -1;
			for (int i = 0; i < keys.size(); ++i) {
				if (!keys[i].channel(lane).on)
					continue;
				if (first < 0)
					first = i;
				last = i;
			}
			if (first < 0)
				return; // this channel isn't keyed: keep the base pose
			if (t <= keys[first].tMs) {
				out.*field = keys[first].tf.*field;
				return;
			}
			if (t >= keys[last].tMs) {
				out.*field = keys[last].tf.*field;
				return;
			}
			int a = first, b = -1;
			for (int i = first; i <= last; ++i) {
				if (!keys[i].channel(lane).on)
					continue;
				if (keys[i].tMs <= t)
					a = i;
				else {
					b = i;
					break;
				}
			}
			if (b < 0) {
				out.*field = keys[a].tf.*field;
				return;
			}
			const qint64 span = std::max<qint64>(1, keys[b].tMs - keys[a].tMs);
			const TlKeyChannel &ch = keys[a].channel(lane);
			const double u = tlEaseAt(ch.ease,
						  std::clamp(double(t - keys[a].tMs) / double(span),
							     0.0, 1.0),
						  ch.bez1, ch.bez2);
			const double p = keys[a].tf.*field, q = keys[b].tf.*field;
			out.*field = p + (q - p) * u;
		};
		// Position is one channel driving two fields, so X and Y can never
		// disagree about which keys they are between.
		solve(TlLanePos, &TlTransform::posX);
		solve(TlLanePos, &TlTransform::posY);
		solve(TlLaneScale, &TlTransform::scale);
		solve(TlLaneRot, &TlTransform::rotation);
		solve(TlLaneOpacity, &TlTransform::opacity);
		return out;
	}

	// Keys that pin `lane`, in time order — what one tab of the keyframe editor
	// shows.
	QVector<int> keysOnLane(int lane) const
	{
		QVector<int> out;
		for (int i = 0; i < keys.size(); ++i)
			if (keys[i].channel(lane).on)
				out.append(i);
		return out;
	}

	// Insert (or replace) a keyframe at an output-time position. The first
	// keyframe added seeds from the base pose so nothing jumps.
	// `lanes` is a mask of 1<<TlLane... — which channels the new key pins.
	// The default pins all of them, which is what the Inspector's one-button
	// "Key" does and what every existing caller means.
	void setKeyframeAt(qint64 outMs, const TlTransform &tf,
			   TlEase ease = TlEase::EaseInOut, int lanes = 0xF)
	{
		const qint64 t = std::clamp<qint64>(outMs - outStartMs, 0, outDurationMs());
		for (TlKeyframe &k : keys) {
			if (std::llabs(k.tMs - t) <= 1) { // replace the one at this time
				k.tf = tf;
				for (int l = 0; l < kTlLaneCount; ++l)
					if (lanes & (1 << l))
						k.channel(l).on = true;
				return;
			}
		}
		TlKeyframe k;
		k.tMs = t;
		k.tf = tf;
		for (int l = 0; l < kTlLaneCount; ++l) {
			k.channel(l).on = (lanes & (1 << l)) != 0;
			k.channel(l).ease = ease;
		}
		int at = 0;
		while (at < keys.size() && keys[at].tMs < t)
			++at;
		keys.insert(at, k);
	}

	// Drop a channel from a key, removing the key entirely once it pins nothing.
	void clearKeyLane(int index, int lane)
	{
		if (index < 0 || index >= keys.size())
			return;
		keys[index].channel(lane).on = false;
		for (int l = 0; l < kTlLaneCount; ++l)
			if (keys[index].channel(l).on)
				return;
		keys.remove(index);
	}

	// Put the keys back in time order (a drag can move one past its neighbour).
	void sortKeys()
	{
		std::stable_sort(keys.begin(), keys.end(),
				 [](const TlKeyframe &a, const TlKeyframe &b) { return a.tMs < b.tMs; });
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
		       posX == o.posX && posY == o.posY && scale == o.scale &&
		       rotation == o.rotation && opacity == o.opacity &&
		       crop == o.crop && keys == o.keys && text == o.text &&
		       scripts == o.scripts &&
		       volume == o.volume && fadeInMs == o.fadeInMs &&
		       fadeOutMs == o.fadeOutMs && fadeInCurve == o.fadeInCurve &&
		       fadeOutCurve == o.fadeOutCurve;
	}
};

// Hand a split clip's fades to the two halves it became: the head keeps the
// fade-in, the tail keeps the fade-out, neither inherits the other's, and both
// are pulled in to fit. Shared by the two split paths so a context-menu split
// and a playhead split can't drift apart.
inline void splitFades(TlClip &head, TlClip &tail)
{
	tail.fadeInMs = 0;
	tail.fadeInCurve = head.fadeInCurve;
	head.fadeOutMs = 0;
	head.fadeOutCurve = tail.fadeOutCurve;
	head.clampFades();
	tail.clampFades();
}

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
	// Points of interest on the output timeline, sorted, in ms. Purely for
	// navigation — nothing about the render depends on them.
	QVector<qint64> markers;

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

	bool operator==(const TimelineModel &o) const
	{
		return tracks == o.tracks && markers == o.markers;
	}
	bool operator!=(const TimelineModel &o) const { return !(*this == o); }
};

} // namespace harpia
