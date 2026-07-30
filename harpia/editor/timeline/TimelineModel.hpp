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
#include "../component/Component.hpp"

#include <functional>
#include "Ease.hpp"
#include "TlTransform.hpp"
#include "EffectClip.hpp"
#include "Transitions.hpp"
#include "Spotlight.hpp"

#include <QColor>
#include <QHash>
#include <QMap>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

namespace harpia {

// One animation keyframe: a pose pinned to a time inside the clip. `tMs` is an
// offset from the clip's outStartMs, in OUTPUT time. `ease` shapes the curve
// from this keyframe to the next.
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
	// Background "sticker" behind the text, the Instagram/TikTok kind: one
	// continuous rounded shape wrapping the whole block, however many lines it
	// has, sized from the text and re-measured whenever the text changes.
	//
	// Padding and radius are in the SAME reference units as fontPx (relative to
	// a 1080-tall canvas) and are scaled by the same factor when rendering, so
	// the corners and the breathing room track the text size instead of
	// shrinking away on a big canvas.
	bool boxEnabled = false;
	QColor boxColor = QColor(0, 0, 0);
	double boxOpacity = 0.6;  // independent of boxColor, so the colour picker
				  // and the transparency don't fight each other
	int boxPadX = 22;         // horizontal breathing room
	int boxPadY = 12;         // vertical
	int boxRadius = 16;       // corner radius; clamped to half the shorter side,
				  // so a large value gives a clean pill
	int align = 1; // 0 = left, 1 = centre, 2 = right (multi-line blocks)

	bool operator==(const TlText &o) const
	{
		return text == o.text && fontFamily == o.fontFamily && fontPx == o.fontPx &&
		       bold == o.bold && italic == o.italic && color == o.color &&
		       outlineWidth == o.outlineWidth && outlineColor == o.outlineColor &&
		       boxEnabled == o.boxEnabled && boxColor == o.boxColor &&
		       boxOpacity == o.boxOpacity && boxPadX == o.boxPadX && boxPadY == o.boxPadY &&
		       boxRadius == o.boxRadius && align == o.align;
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
	// still whose duration is free; Text = a rendered caption; Effect = a grade
	// applied to everything composited below its track, for its own duration.
	enum class Type { Video, Text, Image, Effect };
	Type type = Type::Video;

	// Stills, captions and effects have no source timeline, so their length is
	// whatever the user drags it to rather than being capped by a decoder.
	bool freeDuration() const
	{
		return type == Type::Text || type == Type::Image || type == Type::Effect;
	}

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

	// Effect clips only: which effect, its parameters and its keyframes.
	FxSpec fx;

	// Components. The direction this editor is moving: every clip feature is a
	// component in an ordered list, built-in or user-written alike. They run
	// alongside the fields above for now — the pose, fx and scripts are being
	// ported to components one at a time, and until that is finished both
	// describe part of the clip.
	QVector<ComponentInstance> components;

	// What to call an effect clip on the timeline. Its components decide, when
	// it has any: swapping Brightness for Blur must not leave the strip still
	// saying "Brightness". Falls back to the fx field for a project that has not
	// been migrated yet.
	QString effectLabel(const std::function<QString(const QString &)> &nameOf) const
	{
		if (!fx.name.isEmpty())
			return fx.name; // a name the user typed always wins
		if (components.isEmpty())
			return QString::fromLatin1(fxTypeName(fx.type));
		QStringList parts;
		for (const ComponentInstance &ci : components)
			parts << nameOf(ci.typeId);
		return parts.join(QStringLiteral(" + "));
	}

	// A fresh effect clip, as a component. Both places that make one call this,
	// so "what a new Brightness looks like" is written once.
	static ComponentInstance newEffectComponent(const QString &typeId,
						    const QMap<QString, double> &defaults)
	{
		ComponentInstance ci;
		ci.typeId = typeId;
		ci.instanceId = QStringLiteral("fx");
		for (auto it = defaults.cbegin(); it != defaults.cend(); ++it)
			ci.props.insert(it.key(), it.value());
		return ci;
	}

	// How this clip ARRIVES when it overlaps the one before it on its track.
	// The overlap itself is the transition's duration, so there is nothing here
	// to keep in sync with the clips' positions.
	TlTransition transition;

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
		       scripts == o.scripts && fx == o.fx && components == o.components &&
		       transition == o.transition &&
		       volume == o.volume && fadeInMs == o.fadeInMs &&
		       fadeOutMs == o.fadeOutMs && fadeInCurve == o.fadeInCurve &&
		       fadeOutCurve == o.fadeOutCurve;
	}
};

// The keyframe the Prev/Next buttons should jump to from `relMs` (a clip-time
// position), or -1 when there is nowhere to go.
//
// Wraps: past the last key, Next returns the first. Without that, pressing Next
// on the final key of three does nothing at all, which reads as a broken button
// rather than as having reached the end.
//
// The +/-1 ms deadband stops "next" from re-selecting the key you are already
// sitting on, which is why this is not just a lower_bound.
inline int stepKeyIndex(const QVector<TlKeyframe> &keys, qint64 relMs, int dir)
{
	if (keys.isEmpty())
		return -1;
	if (dir > 0) {
		for (int i = 0; i < keys.size(); ++i)
			if (keys[i].tMs > relMs + 1)
				return i;
		return keys.size() > 1 ? 0 : -1; // wrap; a lone key has nowhere to go
	}
	for (int i = keys.size() - 1; i >= 0; --i)
		if (keys[i].tMs < relMs - 1)
			return i;
	return keys.size() > 1 ? keys.size() - 1 : -1;
}

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
	// Effect is its own kind so the timeline can draw it apart, only accept
	// effect clips onto it, and know where in the composite it belongs.
	enum class Kind { Video, Audio, Effect };
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

	// The two clips in an overlap at `outMs`, if there is one: `*outgoing` is
	// the earlier, `*incoming` the later. Returns false when at most one clip
	// covers this instant. Only ever two -- a three-way pile-up is not a
	// transition anyone means, so the latest pair wins.
	bool overlapAt(qint64 outMs, int *outgoing, int *incoming) const
	{
		int a = -1, b = -1;
		for (int i = 0; i < clips.size(); ++i) {
			if (!clips[i].coversOutput(outMs))
				continue;
			if (a < 0 || clips[i].outStartMs < clips[a].outStartMs) {
				b = (a >= 0 && (b < 0 || clips[a].outStartMs > clips[b].outStartMs)) ? a : b;
				a = i;
			} else if (b < 0 || clips[i].outStartMs > clips[b].outStartMs) {
				b = i;
			}
		}
		if (a < 0 || b < 0 || a == b)
			return false;
		if (outgoing)
			*outgoing = a;
		if (incoming)
			*incoming = b;
		return true;
	}

	// Milliseconds the clip at `index` overlaps the clip that ends inside it —
	// i.e. the length of its incoming transition. 0 when it has none.
	qint64 overlapBefore(int index) const
	{
		if (index < 0 || index >= clips.size())
			return 0;
		const TlClip &me = clips[index];
		qint64 best = 0;
		for (int i = 0; i < clips.size(); ++i) {
			if (i == index)
				continue;
			const TlClip &o = clips[i];
			// Only a clip that STARTS earlier can be the outgoing one.
			if (o.outStartMs >= me.outStartMs)
				continue;
			const qint64 ov = std::min(o.outEndMs(), me.outEndMs()) - me.outStartMs;
			best = std::max(best, std::max<qint64>(0, ov));
		}
		return best;
	}

	// overlapBefore() for EVERY clip at once, in one pass.
	//
	// Painting the track and hit-testing its transitions both need the answer
	// for every clip, and calling overlapBefore() in a loop is O(clips²) — on a
	// busy track that showed up directly in the paint and in every mouse-move.
	// Sorted by start, the answer is just "the furthest end reached by anything
	// that starts strictly earlier", which one sweep carries along.
	QVector<qint64> overlapsBefore() const
	{
		QVector<qint64> out(clips.size(), 0);
		QVector<int> order(clips.size());
		for (int i = 0; i < clips.size(); ++i)
			order[i] = i;
		std::sort(order.begin(), order.end(), [this](int a, int b) {
			return clips[a].outStartMs < clips[b].outStartMs;
		});
		qint64 reach = std::numeric_limits<qint64>::min(); // furthest end so far
		int i = 0;
		while (i < order.size()) {
			// Clips sharing a start must all see the same `reach`: none of
			// them is "strictly earlier" than another.
			int j = i;
			const qint64 start = clips[order[i]].outStartMs;
			while (j < order.size() && clips[order[j]].outStartMs == start)
				++j;
			for (int k = i; k < j; ++k) {
				const TlClip &me = clips[order[k]];
				if (reach > std::numeric_limits<qint64>::min())
					out[order[k]] = std::max<qint64>(
						0, std::min(reach, me.outEndMs()) - me.outStartMs);
			}
			for (int k = i; k < j; ++k)
				reach = std::max(reach, clips[order[k]].outEndMs());
			i = j;
		}
		return out;
	}

	// Are these two clips, in this order, the two halves of one split?
	//
	// Not merely "they touch". A split leaves the source time running straight
	// through the join, so deleting the seam would put the original clip back --
	// which is the thing worth telling apart on screen. Two unrelated pieces of
	// footage that happen to abut are a different edit and read differently.
	//
	// Stills, captions and effects have no source clock (splitAtPlayhead
	// rewrites their src range outright), so for those the test is same source
	// and, for a caption, the same text: continuity has to be shown some other
	// way or not claimed at all.
	static bool isSplitPair(const TlClip &a, const TlClip &b)
	{
		if (a.type != b.type || a.outEndMs() != b.outStartMs)
			return false;
		if (a.type == TlClip::Type::Text)
			return a.text == b.text;
		if (a.type == TlClip::Type::Effect)
			return false; // an effect has neither a source nor a source time
		if (a.sourceId != b.sourceId || a.sourceId == 0)
			return false;
		if (a.freeDuration()) // a still: one file, no clock to be continuous in
			return true;
		return !(qAbs(a.speed - b.speed) > 1e-9) && a.srcEndMs == b.srcStartMs;
	}

	// Where on the output timeline this track's splits are, in ms.
	//
	// Two passes over the clips rather than a pair of nested loops, for the same
	// reason overlapsBefore() has one sweep: this is wanted on every repaint and
	// every mouse-move, and O(clips^2) there is felt on a busy track. Indexing
	// by end time also gets the answer right when clips are not in start order
	// and when a third clip starts at the same instant, which a
	// compare-with-your-neighbour sweep would miss.
	//
	// A MULTI-hash, and that is the whole subtlety. Several clips can end at the
	// same instant -- routine on a track carrying transitions, where clips
	// overlap by design -- and QHash::insert REPLACES on a duplicate key, so a
	// plain QHash remembered only the last of them. When the one it kept was not
	// the split's other half, the seam was simply not drawn: a real split with
	// no valley on it, which is the one thing this is for.
	QVector<qint64> splitSeams() const
	{
		QMultiHash<qint64, int> endsAt; // output ms -> every clip ending there
		endsAt.reserve(clips.size());
		for (int i = 0; i < clips.size(); ++i)
			endsAt.insert(clips[i].outEndMs(), i);
		QVector<qint64> out;
		for (const TlClip &c : clips) {
			for (auto it = endsAt.constFind(c.outStartMs);
			     it != endsAt.constEnd() && it.key() == c.outStartMs; ++it) {
				if (isSplitPair(clips[it.value()], c)) {
					out.append(c.outStartMs);
					break; // one seam per join, however many clips end here
				}
			}
		}
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
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
	// There is no project-level Inverse Selection here any more. A spotlight is
	// a CLIP on an effect track, carrying its own areas -- so it has a start and
	// an end you can see and drag, it dims only the tracks below it, and a
	// project can hold several. The whole-project version was the one case where
	// the clip spans the timeline, which the clip form covers by being dragged
	// that wide.

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

	int audioTrackCount() const
	{
		int n = 0;
		for (const TlTrack &t : tracks)
			if (t.kind == TlTrack::Kind::Audio)
				++n;
		return n;
	}

	// Tracks that live in the picture half of the stack: video, and the effect
	// tracks interleaved among them (an effect track has to be able to sit
	// between two video tracks — that is what decides what it grades).
	static bool isPictureKind(TlTrack::Kind k) { return k != TlTrack::Kind::Audio; }
	int pictureTrackCount() const
	{
		int n = 0;
		for (const TlTrack &t : tracks)
			if (isPictureKind(t.kind))
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
