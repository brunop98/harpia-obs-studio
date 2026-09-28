// Slide to Position: arrive from beyond an edge, settle at the clip's own
// position, optionally leave at the end. Pinned here:
//
//   - every curve starts at 0 and ends at 1; Back and Elastic overshoot,
//     Bounce never does, Ease out is ahead of linear half-way;
//   - the first frame puts the clip fully outside the picture on the chosen
//     edge (its box does not touch the canvas), using its real size, and a
//     corner moves both axes;
//   - after the slide the pose is exactly the clip's own, untouched;
//   - Slide out leaves by the same edge or the chosen one and is outside on
//     the last frame; a clip too short for both halves splits them;
//   - with no known size it still ends up fully outside;
//   - the component is registered, a Transform-stage Motion component, not
//     offered on audio/effect clips, and composes with the clip's pose.
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ComponentStack.hpp"
#include "editor/component/SlideMotion.hpp"

#include <QGuiApplication>
#include <QRectF>

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
static void near(double got, double want, const char *w, double tol = 1e-6)
{
	const bool good = std::abs(got - want) <= tol;
	std::printf("  %s %s (got %.6f, want %.6f)\n", good ? "PASS" : "FAIL", w, got, want);
	if (!good)
		++failures;
}

static bool operator==(const TlTransform &a, const TlTransform &b)
{
	return a.posX == b.posX && a.posY == b.posY && a.scale == b.scale && a.rotation == b.rotation &&
	       a.opacity == b.opacity;
}

// The clip's box on the canvas at this pose, like the compositor draws it.
static QRectF boxOf(const TlTransform &xf, QSize canvas, QSize src)
{
	const QSizeF e = slideClipExtent(xf, canvas, src);
	const double cx = xf.posX * canvas.width(), cy = xf.posY * canvas.height();
	return QRectF(cx - e.width() / 2, cy - e.height() / 2, e.width(), e.height());
}
static bool outside(const TlTransform &xf, QSize canvas, QSize src)
{
	return !boxOf(xf, canvas, src).intersects(QRectF(0, 0, canvas.width(), canvas.height()));
}

int main(int argc, char **argv)
{
	QGuiApplication app(argc, argv);
	const QSize canvas(1920, 1080);
	const QSize logo(400, 200); // fitted: 1920 wide -> scale 0.5 of that = 960x480

	std::printf("\n-- the curves --\n");
	{
		for (int i = 0; i < kSlideCurveCount; ++i) {
			const SlideCurve c = SlideCurve(i);
			near(slideCurveAt(c, 0.0), 0.0, qPrintable(slideCurveNames()[i] + QStringLiteral(" starts at 0")), 1e-9);
			near(slideCurveAt(c, 1.0), 1.0, qPrintable(slideCurveNames()[i] + QStringLiteral(" ends at 1")), 1e-9);
		}
		ok(slideCurveNames().size() == kSlideCurveCount, "one name per curve");
		double backMax = 0, elMax = 0, bounceMax = 0;
		for (int k = 0; k <= 200; ++k) {
			const double u = k / 200.0;
			backMax = std::max(backMax, slideCurveAt(SlideCurve::Back, u));
			elMax = std::max(elMax, slideCurveAt(SlideCurve::Elastic, u));
			bounceMax = std::max(bounceMax, slideCurveAt(SlideCurve::Bounce, u));
		}
		ok(backMax > 1.05, "Back overshoots");
		ok(elMax > 1.05, "Elastic overshoots");
		ok(bounceMax <= 1.0 + 1e-9, "Bounce never passes the target");
		ok(slideCurveAt(SlideCurve::EaseOut, 0.5) > 0.5, "Ease out is ahead of linear half-way");
		ok(slideCurveAt(SlideCurve::EaseIn, 0.5) < 0.5, "Ease in is behind");
	}

	std::printf("\n-- sliding in --\n");
	{
		TlTransform target;
		target.posX = 0.7;
		target.posY = 0.4;
		target.scale = 0.5;
		SlideSettings s;
		s.durationMs = 600;
		s.curve = SlideCurve::Linear;
		for (int e = 0; e < kSlideEdgeCount; ++e) {
			s.from = SlideEdge(e);
			const TlTransform first = slidePoseAt(s, target, 0, 5000, canvas, logo);
			ok(outside(first, canvas, logo),
			   qPrintable(QStringLiteral("from %1: fully outside on the first frame").arg(slideEdgeNames()[e])));
		}
		s.from = SlideEdge::Left;
		const TlTransform f0 = slidePoseAt(s, target, 0, 5000, canvas, logo);
		near(f0.posY, target.posY, "from the left the height stays");
		const QRectF b0 = boxOf(f0, canvas, logo);
		ok(b0.right() < 0 && b0.right() > -10, "and it starts just past the edge, not a screen away");
		s.from = SlideEdge::BottomRight;
		const TlTransform c0 = slidePoseAt(s, target, 0, 5000, canvas, logo);
		ok(c0.posX > 1.0 && c0.posY > 1.0, "a corner moves both axes");

		s.from = SlideEdge::Left;
		const TlTransform mid = slidePoseAt(s, target, 300, 5000, canvas, logo);
		near(mid.posX, (f0.posX + target.posX) / 2, "linear: half-way at half the time");
		const TlTransform done = slidePoseAt(s, target, 600, 5000, canvas, logo);
		ok(done == target, "arrived exactly on the clip's own pose");
		const TlTransform later = slidePoseAt(s, target, 2500, 5000, canvas, logo);
		ok(later == target, "and holds it");
		ok(later.scale == target.scale && later.rotation == target.rotation && later.opacity == target.opacity,
		   "scale, rotation and opacity are never touched");
		const TlTransform end = slidePoseAt(s, target, 5000, 5000, canvas, logo);
		ok(end == target, "without Slide out it is still there on the last frame");

		SlideSettings z = s;
		z.durationMs = 0;
		ok(slidePoseAt(z, target, 0, 5000, canvas, logo) == target, "a zero duration does nothing");
	}

	std::printf("\n-- sliding out --\n");
	{
		TlTransform target;
		target.posX = 0.5;
		target.posY = 0.5;
		target.scale = 0.4;
		SlideSettings s;
		s.from = SlideEdge::Top;
		s.durationMs = 500;
		s.curve = SlideCurve::EaseOut;
		s.slideOut = true;
		ok(slidePoseAt(s, target, 2000, 5000, canvas, logo) == target, "in the middle it sits still");
		const TlTransform last = slidePoseAt(s, target, 5000, 5000, canvas, logo);
		ok(outside(last, canvas, logo) && last.posY < 0, "leaves the way it came: outside at the top on the last frame");
		s.outEdge = int(SlideEdge::Right);
		const TlTransform lastR = slidePoseAt(s, target, 5000, 5000, canvas, logo);
		ok(outside(lastR, canvas, logo) && lastR.posX > 1.0, "or by the edge chosen");
		const TlTransform leaving = slidePoseAt(s, target, 4600, 5000, canvas, logo);
		ok(leaving.posX > target.posX && leaving.posX < lastR.posX, "part-way out part-way through");
		// Mirrored ease: an Ease out arrival starts its departure gently.
		const TlTransform early = slidePoseAt(s, target, 4550, 5000, canvas, logo);
		const double linearFrac = 50.0 / 500.0;
		ok((early.posX - target.posX) / (lastR.posX - target.posX) < linearFrac,
		   "the departure starts slower than linear (the arrival's curve, backwards)");

		// Too short for both: each gets half.
		SlideSettings sh = s;
		sh.durationMs = 800;
		const TlTransform shortMid = slidePoseAt(sh, target, 600, 1000, canvas, logo);
		ok(shortMid.posX > target.posX, "a 1 s clip with 800 ms slides: out is under way just after the half");
		const TlTransform shortIn = slidePoseAt(sh, target, 499, 1000, canvas, logo);
		ok(std::abs(shortIn.posY - target.posY) < 0.01, "and in has finished by then");
	}

	std::printf("\n-- size unknown, rotated --\n");
	{
		TlTransform target;
		target.posX = 0.2;
		target.scale = 1.0;
		SlideSettings s;
		s.from = SlideEdge::Left;
		ok(outside(slidePoseAt(s, target, 0, 3000, canvas, QSize()), canvas, logo),
		   "with no size known it still starts outside (a whole-canvas box is never smaller)");
		target.rotation = 45;
		ok(outside(slidePoseAt(s, target, 0, 3000, canvas, logo), canvas, logo),
		   "a rotated clip's corners are outside too");
	}

	std::printf("\n-- the component --\n");
	{
		ComponentRegistry reg;
		registerBuiltinComponents(reg);
		const ComponentType *t = reg.find(QStringLiteral("harpia.slide"));
		ok(t != nullptr, "Slide to Position is registered");
		ok(t && t->stage == Stage::Transform && t->category == QLatin1String("Motion"),
		   "a Transform-stage component in the Motion menu");
		ok(t && (t->clipKinds & ClipKindEffect) == 0 && (t->clipKinds & ClipKindText),
		   "offered on captions and pictures, not on effect clips");
		ok(t && t->props.size() == 5, "five settings: from, duration, curve, slide out, slide out to");

		QVector<ComponentInstance> list;
		ComponentInstance ci;
		ci.typeId = QStringLiteral("harpia.slide");
		ci.instanceId = QStringLiteral("s1");
		ci.props.insert(QStringLiteral("from"), int(SlideEdge::Right));
		ci.props.insert(QStringLiteral("durationMs"), 400);
		ci.props.insert(QStringLiteral("slideOut"), true);
		list.append(ci);
		ComponentStack st(list, reg);
		EvalContext ctx;
		ctx.durMs = 3000;
		ctx.canvas = canvas;
		ctx.srcSize = logo;
		TlTransform seed;
		seed.posX = 0.3;
		seed.posY = 0.6;
		seed.scale = 0.5;
		ctx.tMs = 0;
		const TlTransform a = st.evaluatePose(ctx, seed).xf;
		ok(a.posX > 1.0 && std::abs(a.posY - 0.6) < 1e-9, "frame one: beyond the right edge, same height");
		ctx.tMs = 1500;
		ok(st.evaluatePose(ctx, seed).xf == seed, "the middle: the clip's own pose");
		ctx.tMs = 3000;
		ok(st.evaluatePose(ctx, seed).xf.posX > 1.0, "last frame: gone again, back to the right");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
