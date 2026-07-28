#pragma once

// Effect clips: a clip on an Effect track that grades everything composited
// BELOW it, for exactly as long as the clip lasts.
//
// The compositor walks tracks back-to-front, so by the time it reaches an
// effect track every track under it has already been drawn — applying the
// effect right there is both the correct semantics and the cheap way to do it.
// Tracks above are drawn afterwards and are untouched, and two overlapping
// effect clips stack in track order, top last. That order is a property of the
// track list, so repeated exports are identical by construction.
//
// Like the rest of the timeline this lives in TimelineCompositor, the single
// description the preview and the exporter share, so an effect cannot look one
// way on screen and another in the file.
//
// Adding an effect is: one enum value, one entry in the parameter table, and
// one case in apply(). Nothing else changes.

#include "Ease.hpp"
#include "Spotlight.hpp"

#include <QImage>
#include <QMap>
#include <QString>
#include <QVector>

namespace harpia {

enum class FxType {
	Brightness = 0,
	Contrast,
	Saturation,
	Exposure,
	HueShift,
	ColorBalance,
	Blur,
	GaussianBlur,
	Sharpen,
	Vignette,
	Glow,
	Pixelate,
	Noise,
	ChromaticAberration,
	InverseSelection, // the Spotlight, as a timed clip
	Count
};

inline constexpr int kFxTypeCount = int(FxType::Count);

// One tunable on an effect. `lo`/`hi` bound it, `def` is where it starts (and
// what "Reset" returns to).
struct FxParamDef {
	const char *key;
	const char *label;
	double lo, hi, def;
};

const char *fxTypeName(FxType t);
FxType fxTypeFromInt(int v);
// The parameters this effect takes, in the order the Inspector should show them.
QVector<FxParamDef> fxParams(FxType t);
// Every parameter at its default — what a fresh effect (and "Reset") holds.
QMap<QString, double> fxDefaults(FxType t);

// A keyframe on an effect's parameters. Holds the whole set, because effects
// have few parameters and animating one usually means animating the look.
struct FxKey {
	qint64 tMs = 0; // offset INSIDE the clip
	QMap<QString, double> params;
	TlEase ease = TlEase::EaseInOut;
	double bez1 = 0.42, bez2 = 0.58;

	bool operator==(const FxKey &o) const
	{
		return tMs == o.tMs && params == o.params && ease == o.ease && bez1 == o.bez1 &&
		       bez2 == o.bez2;
	}
};

struct FxSpec {
	FxType type = FxType::Brightness;
	QString name;      // user-renamable; empty = use the type's name
	bool enabled = true;
	QMap<QString, double> params;
	QVector<FxKey> keys; // sorted by tMs
	SpotlightSpec spot;  // InverseSelection only

	QString label() const
	{
		return name.isEmpty() ? QString::fromLatin1(fxTypeName(type)) : name;
	}

	// The parameters at `tMs` inside the clip: the static set, or the keyframe
	// track interpolated. A key that is missing a parameter falls back to the
	// static value, so adding a parameter later cannot break an old project.
	QMap<QString, double> paramsAt(qint64 tMs) const;

	bool operator==(const FxSpec &o) const
	{
		return type == o.type && name == o.name && enabled == o.enabled &&
		       params == o.params && keys == o.keys && spot == o.spot;
	}
};

class Effects {
public:
	// Grade `img` in place. `tMs` is the position INSIDE the effect clip (so a
	// keyframe at 0 is the clip's start) and `outMs` is the timeline position,
	// which the Spotlight's own masks are keyed against.
	static void apply(QImage &img, const FxSpec &fx, qint64 tMs, qint64 outMs);

	// True when this effect at these parameters would not change a single pixel
	// — the compositor skips it rather than paying for a no-op pass.
	static bool isNoOp(const FxSpec &fx, const QMap<QString, double> &p);
};

} // namespace harpia
