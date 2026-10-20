#pragma once

// Composition guides: the lines a camera draws over its viewfinder to help
// frame a shot -- the rule of thirds, the golden ratio, a cross on the middle.
//
// GuidesOverlay draws them over the part of the desktop being recorded, in a
// click-through window kept out of the capture: on your screen, never in the
// video. Several can be on at once (thirds with a centre mark is the usual
// pairing), so a preset stores them as a set of GuideKind bits.
//
// The geometry lives here, pure, so every line can be checked without a
// screen. It is worked out inside a frame rectangle in whatever units the
// caller paints in -- the overlay uses device pixels, so the lines land on the
// recording's own pixel grid.

#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace harpia {

enum GuideKind : int {
	GuideThirds = 1 << 0,
	GuideGoldenRatio = 1 << 1,
	GuideGoldenSpiral = 1 << 2,
	GuideCrosshair = 1 << 3,
	GuideDiagonals = 1 << 4,
	GuideSafeAreas = 1 << 5,
};
constexpr int kGuideKindCount = 6;
constexpr int kGuideKindsAll = (1 << kGuideKindCount) - 1;

struct GuideKindInfo {
	GuideKind kind;
	const char *label;
	const char *tip;
};

// In the order the settings page lists them.
inline const GuideKindInfo *guideKindInfo()
{
	static const GuideKindInfo k[kGuideKindCount] = {
		{GuideThirds, "Rule of thirds",
		 "Two lines each way, splitting the frame into nine. Put the subject on a "
		 "line, and what matters most where two cross."},
		{GuideGoldenRatio, "Golden ratio",
		 "Like thirds, but at 38% and 62% of the frame (1 : 1.618), a little closer "
		 "to the middle."},
		{GuideGoldenSpiral, "Golden spiral",
		 "A spiral of golden-ratio rectangles. Put the subject where it curls in, "
		 "and let the rest of the shot follow the curve."},
		{GuideCrosshair, "Center crosshair", "A small cross on the exact middle of the frame."},
		{GuideDiagonals, "Diagonals",
		 "Corner to corner, plus a 45° line from each corner: for leading lines and "
		 "movement across the frame."},
		{GuideSafeAreas, "Safe areas",
		 "Boxes at 90% (action safe) and 80% (title safe, dashed). Keep text and key "
		 "action inside, so crops and screen edges don't cut them."},
	};
	return k;
}

// "Rule of thirds + Center crosshair"; past two, "Rule of thirds + 2 more".
inline QString guideKindsSummary(int kinds)
{
	QStringList names;
	for (int i = 0; i < kGuideKindCount; ++i)
		if (kinds & guideKindInfo()[i].kind)
			names << QString::fromUtf8(guideKindInfo()[i].label);
	if (names.isEmpty())
		return QStringLiteral("None");
	if (names.size() <= 2)
		return names.join(QStringLiteral(" + "));
	return QStringLiteral("%1 + %2 more").arg(names.first()).arg(names.size() - 1);
}

// 1/φ: the golden ratio's short part over its long part, 0.618...
constexpr double kGoldenInv = 0.6180339887498949;

// Everything to stroke for a set of guides inside `f`.
struct GuidePaths {
	QPainterPath solid;
	// The title-safe box, dashed so the two safe boxes read apart.
	QPainterPath dashed;
};

namespace guidedetail {

// A quarter of an ellipse around `c`, from c+a to c+b (a and b perpendicular
// and axis-aligned), as a run of short lines.
inline void quarterArc(QPainterPath &path, const QPointF &c, const QPointF &a, const QPointF &b)
{
	constexpr int kSteps = 24;
	constexpr double kHalfPi = 1.5707963267948966; // M_PI needs _USE_MATH_DEFINES on MSVC
	path.moveTo(c + a);
	for (int i = 1; i <= kSteps; ++i) {
		const double t = kHalfPi * double(i) / kSteps;
		path.lineTo(c + a * std::cos(t) + b * std::sin(t));
	}
}

} // namespace guidedetail

// `crisp` is added to the position of every straight vertical or horizontal
// line after flooring it, so a pen an odd number of pixels wide lands on whole
// pixels instead of smearing across two: pass 0.5 for an odd pen width, 0 for
// an even one, and a negative value to keep exact positions (the tests).
inline GuidePaths guidePaths(int kinds, const QRectF &f, double crisp = -1.0)
{
	GuidePaths out;
	if (f.width() < 4 || f.height() < 4)
		return out;
	const auto snap = [crisp](double v) { return crisp < 0 ? v : std::floor(v) + crisp; };
	QPainterPath &p = out.solid;
	const auto vline = [&](double x, double y0, double y1) {
		p.moveTo(snap(x), y0);
		p.lineTo(snap(x), y1);
	};
	const auto hline = [&](double y, double x0, double x1) {
		p.moveTo(x0, snap(y));
		p.lineTo(x1, snap(y));
	};

	if (kinds & GuideThirds) {
		for (int i = 1; i <= 2; ++i) {
			vline(f.left() + f.width() * i / 3.0, f.top(), f.bottom());
			hline(f.top() + f.height() * i / 3.0, f.left(), f.right());
		}
	}
	if (kinds & GuideGoldenRatio) {
		for (const double k : {1.0 - kGoldenInv, kGoldenInv}) {
			vline(f.left() + f.width() * k, f.top(), f.bottom());
			hline(f.top() + f.height() * k, f.left(), f.right());
		}
	}
	if (kinds & GuideDiagonals) {
		p.moveTo(f.topLeft());
		p.lineTo(f.bottomRight());
		p.moveTo(f.topRight());
		p.lineTo(f.bottomLeft());
		// 45° from each corner, until it meets the far side.
		const double m = std::min(f.width(), f.height());
		p.moveTo(f.topLeft());
		p.lineTo(f.left() + m, f.top() + m);
		p.moveTo(f.topRight());
		p.lineTo(f.right() - m, f.top() + m);
		p.moveTo(f.bottomLeft());
		p.lineTo(f.left() + m, f.bottom() - m);
		p.moveTo(f.bottomRight());
		p.lineTo(f.right() - m, f.bottom() - m);
	}
	if (kinds & GuideSafeAreas) {
		const auto box = [&](QPainterPath &into, double inset) {
			const double l = snap(f.left() + f.width() * inset), r = snap(f.right() - f.width() * inset);
			const double t = snap(f.top() + f.height() * inset), b = snap(f.bottom() - f.height() * inset);
			into.addRect(QRectF(QPointF(l, t), QPointF(r, b)));
		};
		box(p, 0.05);           // action safe: 90%
		box(out.dashed, 0.10);  // title safe: 80%
	}
	if (kinds & GuideGoldenSpiral) {
		// Golden rectangles, each cut into a "square" (φ⁻¹ of the long way)
		// and the rest, turning a quarter each time -- left, top, right,
		// bottom -- with a quarter arc through every square. Cut by fraction
		// rather than by true squares, so it fits any frame the way an
		// editor's crop overlay does: a 16:9 frame gets a slightly wide spiral.
		QRectF r = f;
		for (int i = 0; i < 16 && r.width() >= 2 && r.height() >= 2; ++i) {
			switch (i % 4) {
			case 0: { // square on the left: bottom-left to top-right
				const double s = r.width() * kGoldenInv;
				guidedetail::quarterArc(p, QPointF(r.left() + s, r.bottom()), QPointF(-s, 0),
							QPointF(0, -r.height()));
				vline(r.left() + s, r.top(), r.bottom());
				r.setLeft(r.left() + s);
				break;
			}
			case 1: { // on top: top-left to bottom-right
				const double t = r.height() * kGoldenInv;
				guidedetail::quarterArc(p, QPointF(r.left(), r.top() + t), QPointF(0, -t),
							QPointF(r.width(), 0));
				hline(r.top() + t, r.left(), r.right());
				r.setTop(r.top() + t);
				break;
			}
			case 2: { // on the right: top-right to bottom-left
				const double u = r.width() * kGoldenInv;
				const double x = r.right() - u;
				guidedetail::quarterArc(p, QPointF(x, r.top()), QPointF(u, 0), QPointF(0, r.height()));
				vline(x, r.top(), r.bottom());
				r.setRight(x);
				break;
			}
			default: { // at the bottom: bottom-right to top-left
				const double v = r.height() * kGoldenInv;
				const double y = r.bottom() - v;
				guidedetail::quarterArc(p, QPointF(r.right(), y), QPointF(0, v), QPointF(-r.width(), 0));
				hline(y, r.left(), r.right());
				r.setBottom(y);
				break;
			}
			}
		}
	}
	if (kinds & GuideCrosshair) {
		const QPointF c = f.center();
		const double arm = std::clamp(std::min(f.width(), f.height()) * 0.035, 6.0, 48.0);
		hline(c.y(), c.x() - arm, c.x() + arm);
		vline(c.x(), c.y() - arm, c.y() + arm);
	}
	return out;
}

// Whether the guides are on screen.
//
// `hiddenFromCapture`: the system can keep the overlay out of the recording
// (Windows 10 version 2004 and newer). Where it cannot, the guides go away for
// the length of every recording -- lines burned into the video are the one
// thing this feature must never produce.
//
// `areaShown`: the recorded area's frame is on screen. A Custom Region has one
// (RegionTool), and the guides are drawn inside it, so they come and go WITH it
// -- guides floating over the desktop after the frame they divide has gone
// read as a leftover, not as a tool. Entire Monitor has no frame: pass true.
//
// Otherwise they stay up whenever they are switched on, Harpia in front or not:
// lining up a shot happens with the game in front. The exceptions, with nothing
// being recorded: the editor, a window you work inside -- the same rule the
// region frame follows -- and Harpia minimized (`appMinimized`), put away along
// with everything it draws on the desktop.
inline bool guidesVisible(bool on, int kinds, bool hasVideo, bool recording, bool hiddenFromCapture,
			  bool editorOpen, bool arrangingAreas, bool areaShown = true, bool appMinimized = false)
{
	if (!on || (kinds & kGuideKindsAll) == 0 || !hasVideo || arrangingAreas || !areaShown)
		return false;
	if (recording)
		return hiddenFromCapture;
	return !editorOpen && !appMinimized;
}

} // namespace harpia
