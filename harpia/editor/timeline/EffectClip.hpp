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

// True when an effect's output pixel depends only on the input pixel at the
// SAME position -- a curve, a matrix, a hue rotation. Those give the same
// answer on a sub-rect as on the whole frame, which is what lets an
// area-bounded effect clip grade only the region it covers.
//
// Listed by what is NOT one, because that is the shorter and the more dangerous
// list: anything reading its neighbours would sample the cut edge instead of
// the pixels really there and leave a seam at the area's border. A new FxType
// therefore defaults to "not a point op" by falling through, which is the safe
// way round -- a wrong `true` here is a rendering bug, a wrong `false` is only
// slower.
// One line saying what an effect does, for the component's tooltip. Effects had
// none: the Inspector showed a name and some sliders and left you to find out
// what "Chromatic aberration" or "Exposure" meant by moving them.
inline const char *fxHelp(FxType t)
{
	switch (t) {
	case FxType::Brightness: return "Lift or lower every pixel evenly.";
	case FxType::Contrast: return "Push light and dark apart, or pull them together.";
	case FxType::Saturation: return "How strong the colours are. 0 is greyscale.";
	case FxType::Exposure: return "Brightness in stops, the way a camera means it — "
				      "gentler in the highlights than Brightness.";
	case FxType::HueShift: return "Rotate every colour round the wheel.";
	case FxType::ColorBalance: return "Warm or cool the picture by moving the "
					  "red/green/blue mix.";
	case FxType::Blur: return "Soften the picture. A fast box blur.";
	case FxType::GaussianBlur: return "Soften the picture more smoothly than Blur, "
					  "and more slowly.";
	case FxType::Sharpen: return "Bring out edges. Too much looks crunchy.";
	case FxType::Vignette: return "Darken towards the corners, to draw the eye "
				      "to the middle.";
	case FxType::Glow: return "Bloom the bright parts into what is next to them.";
	case FxType::Pixelate: return "Drop the resolution into visible blocks.";
	case FxType::Noise: return "Add film-like grain.";
	case FxType::ChromaticAberration: return "Split the colour channels apart at the "
						 "edges, like a cheap lens.";
	case FxType::InverseSelection: return "Dim everything except the chosen areas.";
	case FxType::Count: break;
	}
	return "";
}

inline bool fxIsPointOp(FxType t)
{
	switch (t) {
	case FxType::Blur:
	case FxType::GaussianBlur:
	case FxType::Sharpen:
	case FxType::Glow:
	case FxType::Pixelate:
	case FxType::ChromaticAberration:
	case FxType::Vignette:          // depends on the pixel's POSITION in the frame
	case FxType::Noise:             // ditto: the pattern is placed by coordinate
	case FxType::InverseSelection:  // a mask over the whole composition
	case FxType::Count:
		return false;
	case FxType::Brightness:
	case FxType::Contrast:
	case FxType::Saturation:
	case FxType::Exposure:
	case FxType::HueShift:
	case FxType::ColorBalance:
		return true;
	}
	return false;
}

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

	// Record `values` as a key at `tMs` (CLIP time), replacing one already
	// there. Same rule as a mask: one key is a static setting, not animation,
	// so removing down to one folds it back into `params`.
	void setKeyAt(qint64 tMs, const QMap<QString, double> &values);
	void removeKeyAt(qint64 tMs);

	bool operator==(const FxSpec &o) const
	{
		return type == o.type && name == o.name && enabled == o.enabled &&
		       params == o.params && keys == o.keys && spot == o.spot;
	}
};

class Effects {
public:
	// Force the number of row bands the pixel passes use, for tests. 0 restores
	// the default (one per core, and serial below a size threshold).
	//
	// This exists because the threshold makes threading UNTESTABLE by accident:
	// the pixel-equality test runs at 160x120, which is far below it, so it
	// exercised only the serial path and would have passed however wrong the
	// banded one was. With this a test can render the same frame at 1 band and
	// at N and demand they match exactly.
	static void setPixelBandsForTest(int bands);

	// Grade `img` in place. `tMs` is the position INSIDE the effect clip, so a
	// keyframe at 0 is the clip's start — everything time-varying here, the
	// parameters and the Spotlight's masks alike, is keyed against it, and so
	// moving a clip carries its animation along.
	static void apply(QImage &img, const FxSpec &fx, qint64 tMs);

	// True when this effect at these parameters would not change a single pixel
	// — the compositor skips it rather than paying for a no-op pass.
	static bool isNoOp(const FxSpec &fx, const QMap<QString, double> &p);
};

} // namespace harpia
