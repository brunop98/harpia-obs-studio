#pragma once

// Inverse Selection (Spotlight): dim the whole frame except for one or more
// chosen areas — the highlight every screen tutorial and short-form video uses.
//
// It runs on the COMPOSITED frame, so it covers every visible track at once,
// and it runs inside TimelineCompositor — the one description shared by the
// preview and the exporter — so what is on screen is what gets encoded, to the
// pixel. There is no separate export path to drift out of sync.
//
// Deliberately CPU (QPainter + a separable box blur) rather than a GL pass:
// the compositor is QImage on both the preview and the export worker, and a GL
// path would only exist on one of them. The dim/blur work is bounded by the
// render size, which the preview-quality control already scales.
//
// Shapes are an enum with a single switch behind them (maskPath), so a custom
// path type is one case and one editor, not a rework.

#include "Ease.hpp"

#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

namespace harpia {

enum class SpotShape { Rect, RoundRect, Circle, Ellipse };

inline constexpr int kSpotShapeCount = 4;

inline const char *spotShapeName(SpotShape s)
{
	switch (s) {
	case SpotShape::Rect: return "Rectangle";
	case SpotShape::RoundRect: return "Rounded rectangle";
	case SpotShape::Circle: return "Circle";
	case SpotShape::Ellipse: return "Ellipse";
	}
	return "Rectangle";
}

inline SpotShape spotShapeFromInt(int v)
{
	return (v >= 0 && v < kSpotShapeCount) ? SpotShape(v) : SpotShape::Rect;
}

// Everything about one area that can move over time. Normalised to the canvas
// (0..1) so a project renders the same at any output size.
struct SpotPose {
	double cx = 0.5, cy = 0.5; // centre
	double w = 0.35, h = 0.35; // size (a Circle uses w for both)
	double rotation = 0.0;     // degrees clockwise about the centre
	double radius = 0.08;      // RoundRect corner radius, as a fraction of the
				   // SHORTER side, so it scales with the shape
	double visible = 1.0;      // 0..1; below 0.5 the area is not cut out at all

	bool operator==(const SpotPose &o) const
	{
		return cx == o.cx && cy == o.cy && w == o.w && h == o.h &&
		       rotation == o.rotation && radius == o.radius && visible == o.visible;
	}
};

// One keyframe on a mask. Unlike a clip's keys these pin the whole pose: a mask
// has far fewer channels than a clip and moving one usually means moving it.
struct SpotKey {
	qint64 tMs = 0; // OUTPUT time
	SpotPose pose;
	TlEase ease = TlEase::EaseInOut;
	double bez1 = 0.42, bez2 = 0.58;

	bool operator==(const SpotKey &o) const
	{
		return tMs == o.tMs && pose == o.pose && ease == o.ease && bez1 == o.bez1 &&
		       bez2 == o.bez2;
	}
};

struct SpotMask {
	QString name;
	SpotShape shape = SpotShape::RoundRect;
	bool enabled = true;
	SpotPose pose;              // used when there are no keys
	QVector<SpotKey> keys;      // sorted by tMs

	SpotPose poseAt(qint64 outMs) const
	{
		if (keys.isEmpty())
			return pose;
		if (keys.size() == 1 || outMs <= keys.front().tMs)
			return keys.front().pose;
		if (outMs >= keys.back().tMs)
			return keys.back().pose;
		int i = 0;
		while (i + 1 < keys.size() && keys[i + 1].tMs <= outMs)
			++i;
		const SpotKey &a = keys[i];
		const SpotKey &b = keys[i + 1];
		const qint64 span = std::max<qint64>(1, b.tMs - a.tMs);
		const double u = tlEaseAt(a.ease, double(outMs - a.tMs) / double(span), a.bez1, a.bez2);
		auto mix = [u](double p, double q) { return p + (q - p) * u; };
		SpotPose r;
		r.cx = mix(a.pose.cx, b.pose.cx);
		r.cy = mix(a.pose.cy, b.pose.cy);
		r.w = mix(a.pose.w, b.pose.w);
		r.h = mix(a.pose.h, b.pose.h);
		r.rotation = mix(a.pose.rotation, b.pose.rotation);
		r.radius = mix(a.pose.radius, b.pose.radius);
		r.visible = mix(a.pose.visible, b.pose.visible);
		return r;
	}

	bool operator==(const SpotMask &o) const
	{
		return name == o.name && shape == o.shape && enabled == o.enabled &&
		       pose == o.pose && keys == o.keys;
	}
};

// The whole effect: the areas, and what happens to everything else.
struct SpotlightSpec {
	bool enabled = false;
	bool invert = false;          // true = dim INSIDE the areas instead
	double dimOpacity = 0.65;     // 0..1 over the dimmed part
	QColor dimColor = QColor(0, 0, 0);
	double blur = 0.0;            // 0..1, mapped to a radius in canvas terms
	QVector<SpotMask> masks;

	bool active() const
	{
		if (!enabled || masks.isEmpty())
			return false;
		for (const SpotMask &m : masks)
			if (m.enabled)
				return true;
		return false;
	}

	bool operator==(const SpotlightSpec &o) const
	{
		return enabled == o.enabled && invert == o.invert && dimOpacity == o.dimOpacity &&
		       dimColor == o.dimColor && blur == o.blur && masks == o.masks;
	}
	bool operator!=(const SpotlightSpec &o) const { return !(*this == o); }
};

class Spotlight {
public:
	// The outline of one mask on a `canvas`-sized frame, rotation included.
	// Exposed so the preview can draw handles exactly where the shape is.
	static QPainterPath maskPath(const SpotMask &m, const SpotPose &pose, QSize canvas);

	// Dim `frame` in place. `outMs` drives the masks' keyframes. Does nothing
	// when the spec is inactive, so the cost of "off" is one branch.
	static void apply(QImage &frame, const SpotlightSpec &spec, qint64 outMs);

	// Separable box blur, run three times to approximate a Gaussian. Exposed for
	// testing; `radius` is in pixels.
	static void blurInPlace(QImage &img, int radius);

	// Built-in starting points.
	static QVector<SpotMask> presets(); // one per name below
	static SpotMask preset(const QString &name);
	static QStringList presetNames();
};

} // namespace harpia
