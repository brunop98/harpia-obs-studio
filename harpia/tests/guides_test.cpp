// Composition guides: where every line goes, when they are on screen, and what
// the paint actually puts in the pixels.
//
// The rule the whole feature hangs on is in "when they show": a guide that ends
// up in the recording is the one outcome that must never happen, so the
// recording rows of that table are pinned one by one.
#include "core/CompositionGuides.hpp"
#include "model/Preset.hpp"
#include "ui/GuidesOverlay.hpp"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <cmath>
#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static bool near(double a, double b, double tol = 0.01)
{
	return std::abs(a - b) <= tol;
}

// The straight segments of a path as (start, end) pairs: every subpath that
// is a single moveTo + lineTo.
static QVector<QLineF> segments(const QPainterPath &p)
{
	QVector<QLineF> out;
	for (int i = 0; i + 1 < p.elementCount(); ++i) {
		const QPainterPath::Element a = p.elementAt(i), b = p.elementAt(i + 1);
		if (a.isMoveTo() && b.isLineTo() && (i + 2 >= p.elementCount() || p.elementAt(i + 2).isMoveTo()))
			out.append(QLineF(a.x, a.y, b.x, b.y));
	}
	return out;
}

static bool hasVertical(const QPainterPath &p, double x, double y0, double y1)
{
	for (const QLineF &l : segments(p))
		if (near(l.x1(), x) && near(l.x2(), x) && near(std::min(l.y1(), l.y2()), y0) &&
		    near(std::max(l.y1(), l.y2()), y1))
			return true;
	return false;
}

static bool hasHorizontal(const QPainterPath &p, double y, double x0, double x1)
{
	for (const QLineF &l : segments(p))
		if (near(l.y1(), y) && near(l.y2(), y) && near(std::min(l.x1(), l.x2()), x0) &&
		    near(std::max(l.x1(), l.x2()), x1))
			return true;
	return false;
}

static bool hasLine(const QPainterPath &p, QPointF a, QPointF b)
{
	for (const QLineF &l : segments(p))
		if ((near(l.x1(), a.x()) && near(l.y1(), a.y()) && near(l.x2(), b.x()) && near(l.y2(), b.y())) ||
		    (near(l.x1(), b.x()) && near(l.y1(), b.y()) && near(l.x2(), a.x()) && near(l.y2(), a.y())))
			return true;
	return false;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	const QRectF f(0, 0, 1920, 1080);

	std::printf("\n-- where the lines go --\n");
	{
		const GuidePaths g = guidePaths(GuideThirds, f);
		ok(hasVertical(g.solid, 640, 0, 1080) && hasVertical(g.solid, 1280, 0, 1080),
		   "thirds: two verticals at 1/3 and 2/3, full height");
		ok(hasHorizontal(g.solid, 360, 0, 1920) && hasHorizontal(g.solid, 720, 0, 1920),
		   "and two horizontals, full width");
		ok(segments(g.solid).size() == 4 && g.dashed.isEmpty(), "and nothing else");
	}
	{
		const GuidePaths g = guidePaths(GuideGoldenRatio, f);
		ok(hasVertical(g.solid, 1920 * 0.381966, 0, 1080) && hasVertical(g.solid, 1920 * 0.618034, 0, 1080),
		   "golden ratio: verticals at 38.2% and 61.8%");
		ok(hasHorizontal(g.solid, 1080 * 0.381966, 0, 1920) &&
			   hasHorizontal(g.solid, 1080 * 0.618034, 0, 1920),
		   "and horizontals too");
	}
	{
		const GuidePaths g = guidePaths(GuideCrosshair, f);
		const QRectF b = g.solid.boundingRect();
		ok(near(b.center().x(), 960) && near(b.center().y(), 540), "crosshair: on the exact middle");
		ok(b.width() < 120 && b.height() < 120 && b.width() > 10, "and small: a mark, not a grid");
	}
	{
		const GuidePaths g = guidePaths(GuideDiagonals, f);
		ok(hasLine(g.solid, {0, 0}, {1920, 1080}) && hasLine(g.solid, {1920, 0}, {0, 1080}),
		   "diagonals: corner to corner, both ways");
		ok(hasLine(g.solid, {0, 0}, {1080, 1080}) && hasLine(g.solid, {1920, 0}, {840, 1080}) &&
			   hasLine(g.solid, {0, 1080}, {1080, 0}) && hasLine(g.solid, {1920, 1080}, {840, 0}),
		   "and 45 degrees from every corner, stopping at the far side");
	}
	{
		const GuidePaths g = guidePaths(GuideSafeAreas, f);
		const QRectF action = g.solid.boundingRect(), title = g.dashed.boundingRect();
		ok(near(action.left(), 96) && near(action.top(), 54) && near(action.right(), 1824) &&
			   near(action.bottom(), 1026),
		   "safe areas: action safe at 90%");
		ok(near(title.left(), 192) && near(title.top(), 108) && near(title.right(), 1728) &&
			   near(title.bottom(), 972),
		   "title safe at 80%, on the dashed pen");
	}
	{
		const GuidePaths g = guidePaths(GuideGoldenSpiral, f);
		const QRectF b = g.solid.boundingRect();
		ok(b.left() >= -0.01 && b.top() >= -0.01 && b.right() <= 1920.01 && b.bottom() <= 1080.01,
		   "golden spiral: stays inside the frame");
		// The arcs, each a run of lineTos after a moveTo: one continuous curve
		// means every arc starts exactly where the previous one ended.
		QVector<QPointF> starts, ends;
		for (int i = 0; i < g.solid.elementCount(); ++i) {
			const QPainterPath::Element e = g.solid.elementAt(i);
			const bool arcStart = e.isMoveTo() && i + 3 < g.solid.elementCount() &&
					      g.solid.elementAt(i + 2).isLineTo() && g.solid.elementAt(i + 3).isLineTo();
			if (!arcStart)
				continue;
			starts.append(QPointF(e.x, e.y));
			int j = i + 1;
			while (j + 1 < g.solid.elementCount() && g.solid.elementAt(j + 1).isLineTo())
				++j;
			ends.append(QPointF(g.solid.elementAt(j).x, g.solid.elementAt(j).y));
		}
		bool joined = starts.size() >= 8;
		for (int i = 1; i < starts.size(); ++i)
			joined = joined && near(starts[i].x(), ends[i - 1].x(), 0.05) &&
				 near(starts[i].y(), ends[i - 1].y(), 0.05);
		ok(joined, "and is one unbroken curve, arc after arc");
		ok(near(starts.first().x(), 0) && near(starts.first().y(), 1080),
		   "starting in the bottom-left corner");
		const QPointF eye = ends.last();
		ok(eye.x() > 1920 * 0.6 && eye.x() < 1920 * 0.85 && eye.y() > 1080 * 0.55 && eye.y() < 1080 * 0.8,
		   "and curling in near the lower-right golden point");
		ok(hasVertical(g.solid, 1920 * kGoldenInv, 0, 1080),
		   "with the first golden cut drawn as a divider");
	}
	{
		const GuidePaths g = guidePaths(GuideThirds | GuideCrosshair, f, 0.5);
		ok(hasVertical(g.solid, 640.5, 0, 1080), "crisp: an odd pen's lines sit on pixel centres");
		const GuidePaths e = guidePaths(GuideThirds, QRectF(0, 0, 1000, 600), 0.0);
		ok(hasVertical(e.solid, 333, 0, 600), "and an even pen's on whole pixels (333.3 -> 333)");
	}
	{
		ok(guidePaths(kGuideKindsAll, QRectF(0, 0, 2, 2)).solid.isEmpty(), "nothing in a frame too small to frame");
		ok(guidePaths(0, f).solid.isEmpty() && guidePaths(0, f).dashed.isEmpty(), "and nothing when none is chosen");
	}

	std::printf("\n-- what they are called --\n");
	{
		ok(guideKindsSummary(GuideThirds) == QStringLiteral("Rule of thirds"), "one by name");
		ok(guideKindsSummary(GuideThirds | GuideCrosshair) ==
			   QStringLiteral("Rule of thirds + Center crosshair"),
		   "two joined");
		ok(guideKindsSummary(GuideThirds | GuideCrosshair | GuideDiagonals) ==
			   QStringLiteral("Rule of thirds + 2 more"),
		   "more than two, counted");
		ok(guideKindsSummary(0) == QStringLiteral("None"), "none");
		int all = 0;
		for (int i = 0; i < kGuideKindCount; ++i)
			all |= guideKindInfo()[i].kind;
		ok(all == kGuideKindsAll, "every kind is listed once on the settings page");
	}

	std::printf("\n-- when they show --\n");
	{
		// on, kinds, video, recording, hidden-from-capture, editor, arranging
		ok(guidesVisible(true, GuideThirds, true, false, false, false, false),
		   "switched on and idle: shown, Harpia in front or not");
		ok(!guidesVisible(false, GuideThirds, true, false, true, false, false), "switched off: never");
		ok(!guidesVisible(true, 0, true, false, true, false, false), "on with nothing chosen: nothing to draw");
		ok(!guidesVisible(true, GuideThirds, false, false, true, false, false), "Audio Only: no picture to frame");
		ok(guidesVisible(true, GuideThirds, true, true, true, false, false),
		   "recording, kept out of the capture: shown");
		ok(!guidesVisible(true, GuideThirds, true, true, false, false, false),
		   "recording where the system cannot keep them out: HIDDEN, never in the video");
		ok(!guidesVisible(true, GuideThirds, true, true, false, true, false),
		   "even with the editor open");
		ok(!guidesVisible(true, GuideThirds, true, false, true, true, false),
		   "the editor open, not recording: out of the way");
		ok(guidesVisible(true, GuideThirds, true, true, true, true, false),
		   "the editor open while recording (and kept out): still marking the take");
		ok(!guidesVisible(true, GuideThirds, true, false, true, false, true),
		   "arranging Multi-Area: the arrange overlay has the screen");
		ok(!guidesVisible(true, GuideThirds, true, false, true, false, false, false),
		   "a region whose frame is hidden: the guides go with it");
		ok(!guidesVisible(true, GuideThirds, true, true, true, false, false, false),
		   "even recording");
		ok(guidesVisible(true, GuideThirds, true, false, true, false, false, true),
		   "and come back when it does");
	}

	std::printf("\n-- the preset --\n");
	{
		const Preset p = Preset::makeDefault("/tmp/out");
		ok(!p.showGuides, "off by default: nothing appears over anyone's desktop after an update");
		ok(p.guideKinds == GuideThirds, "the rule of thirds once switched on");
		ok(p.guideColor == "#ffffff" && p.guideOpacityPct == 60 && p.guideThickness == 1,
		   "white, 60%, 1 px");
		ok(p.guidesShortcut == "Ctrl+Shift+G", "Ctrl+Shift+G, next to Zoom's and Follow's");
		ok(p.guidesShortcut != p.zoomShortcut && p.guidesShortcut != p.spotlightShortcut &&
			   p.guidesShortcut != p.followShortcut,
		   "and on a key of its own");
	}

	std::printf("\n-- the paint --\n");
	{
		QImage img(300, 200, QImage::Format_ARGB32_Premultiplied);
		img.fill(Qt::transparent);
		GuideStyle s;
		s.kinds = GuideThirds;
		s.color = QColor(255, 0, 0);
		s.opacityPct = 100;
		s.thickness = 1;
		{
			QPainter p(&img);
			paintGuides(p, QRectF(0, 0, 300, 200), s);
		}
		const QColor on = img.pixelColor(100, 50);
		ok(on.red() == 255 && on.green() == 0 && on.alpha() == 255, "a 1 px line lands on one whole pixel column, in the colour chosen");
		const QColor edge = img.pixelColor(99, 50);
		ok(edge.alpha() > 0 && edge.alpha() < 140 && edge.red() < 40,
		   "with a faint dark edge beside it, so a light line reads on a light scene");
		ok(img.pixelColor(50, 50).alpha() == 0, "and the rest is see-through");

		img.fill(Qt::transparent);
		s.opacityPct = 50;
		s.color = QColor(0, 0, 0);
		{
			QPainter p(&img);
			paintGuides(p, QRectF(0, 0, 300, 200), s);
		}
		const QColor half = img.pixelColor(100, 50);
		ok(std::abs(half.alpha() - 128) <= 2 && half.red() < 10,
		   "opacity is the line's own alpha -- its edge does not darken it");
		ok(img.pixelColor(99, 50).red() > 200, "and a dark line gets a light edge");

		img.fill(Qt::transparent);
		s.opacityPct = 100;
		s.color = QColor(255, 255, 255);
		s.thickness = 3;
		{
			QPainter p(&img);
			paintGuides(p, QRectF(0, 0, 300, 200), s);
		}
		ok(img.pixelColor(99, 50).alpha() == 255 && img.pixelColor(101, 50).alpha() == 255 &&
			   img.pixelColor(100, 50).alpha() == 255,
		   "3 px is three whole pixels");

		// A 150% display: 1 px at 100% is rounded to 2 device pixels.
		QImage hi(450, 300, QImage::Format_ARGB32_Premultiplied);
		hi.fill(Qt::transparent);
		s.thickness = 1;
		{
			QPainter p(&hi);
			p.scale(1.5, 1.5);
			paintGuides(p, QRectF(0, 0, 300, 200), s, 1.5);
		}
		int solid = 0;
		for (int x = 140; x < 160; ++x)
			if (hi.pixelColor(x, 60).alpha() == 255)
				++solid;
		ok(solid == 2, "on a 150% display a 1 px line is 2 sharp device pixels, not a 1.5 px smear");
	}

	std::printf("\n-- the overlay window --\n");
	{
		GuidesOverlay o;
		const Qt::WindowFlags fl = o.windowFlags();
		ok(fl.testFlag(Qt::WindowTransparentForInput) && o.testAttribute(Qt::WA_TransparentForMouseEvents),
		   "clicks go straight through to the game underneath");
		ok(fl.testFlag(Qt::WindowStaysOnTopHint) && fl.testFlag(Qt::FramelessWindowHint),
		   "frameless and on top");
		ok(fl.testFlag(Qt::WindowDoesNotAcceptFocus) && o.testAttribute(Qt::WA_ShowWithoutActivating),
		   "and never takes the focus from it");
		GuideStyle s;
		o.showGuides(QRect(100, 50, 800, 450), s);
		ok(o.isVisible() && o.geometry() == QRect(100, 50, 800, 450), "covers exactly the recorded area");
		o.showGuides(QRect(120, 50, 800, 450), s);
		ok(o.geometry() == QRect(120, 50, 800, 450), "and follows it when it moves");
		o.showGuides(QRect(120, 50, 4, 4), s);
		ok(!o.isVisible(), "an area too small to frame hides it");
		o.showGuides(QRect(0, 0, 640, 360), s);
		o.hideGuides();
		ok(!o.isVisible(), "hideGuides hides it");
#if !defined(_WIN32)
		ok(!GuidesOverlay::systemCanHideFromCapture() && !o.hiddenFromCapture(),
		   "off Windows nothing keeps it out of a capture, so it says so (and hides while recording)");
#endif
	}
	{
		GuidesPreview pv;
		GuideStyle s;
		s.kinds = 0;
		pv.setStyle(s);
		const QImage none = pv.grab().toImage();
		s.kinds = GuideThirds;
		s.opacityPct = 100;
		pv.setStyle(s);
		const QImage thirds = pv.grab().toImage();
		ok(none.pixelColor(64, 30) != thirds.pixelColor(64, 30), "the settings preview draws the chosen guides");
		ok(none.pixelColor(30, 20) == thirds.pixelColor(30, 20), "over the stand-in picture, which stays put");
	}

	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
