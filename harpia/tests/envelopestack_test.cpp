// In / Out ramps through the REAL component stack.
//
// componentenvelope_test pins the weight curve. This pins what the weight
// DOES, which is a different question and the one with the interesting
// failure modes:
//
//   * a Pixel component must cross-fade -- half a saturation boost is a
//     half-saturated image, not a hard on/off;
//   * a Transform component must ramp its POSE -- half a move is the clip
//     half-way there, NOT a double exposure of moved and unmoved, which is
//     what a naive "blend everything" rule would produce;
//   * the hold must be byte-identical to no envelope at all, or this feature
//     quietly re-renders every existing project;
//   * and it has to compose with keyframes rather than replace them.
#include "editor/component/Component.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ComponentStack.hpp"

#include <QImage>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

namespace {

// A Pixel component that paints the frame a flat colour whose brightness is
// its "amount" property. Flat, so a cross-fade is readable as one number.
class Flood : public IComponent {
public:
	void evaluate(const EvalContext &ctx, ClipState &io) const override
	{
		if (!io.frame)
			return;
		const int v = std::clamp(int(ctx.f("amount", 1.0) * 255.0), 0, 255);
		io.frame->fill(QColor(v, v, v));
	}
};

// A Transform component that shoves the clip to one side.
class Shove : public IComponent {
public:
	void evaluate(const EvalContext &ctx, ClipState &io) const override
	{
		io.xf.posX = ctx.f("to", 1.0);
		io.xf.scale = ctx.f("scale", 2.0);
	}
};

ComponentRegistry makeRegistry()
{
	ComponentRegistry reg;
	ComponentType flood;
	flood.id = QStringLiteral("test.flood");
	flood.displayName = QStringLiteral("Flood");
	flood.stage = Stage::Pixel;
	flood.props = {{QStringLiteral("amount"), QStringLiteral("Amount"), PropType::Float, 0.0, 1.0, 1.0}};
	flood.make = []() { return std::unique_ptr<IComponent>(new Flood); };
	reg.add(flood);

	ComponentType shove;
	shove.id = QStringLiteral("test.shove");
	shove.displayName = QStringLiteral("Shove");
	shove.stage = Stage::Transform;
	shove.props = {{QStringLiteral("to"), QStringLiteral("To"), PropType::Float, 0.0, 1.0, 1.0},
		       {QStringLiteral("scale"), QStringLiteral("Scale"), PropType::Float, 0.0, 4.0, 2.0}};
	shove.make = []() { return std::unique_ptr<IComponent>(new Shove); };
	reg.add(shove);
	return reg;
}

ComponentInstance inst(const char *type, qint64 in, qint64 out)
{
	ComponentInstance c;
	c.typeId = QString::fromLatin1(type);
	c.instanceId = QStringLiteral("a");
	c.inMs = in;
	c.outMs = out;
	return c;
}

EvalContext at(qint64 tMs, qint64 durMs)
{
	EvalContext c;
	c.tMs = tMs;
	c.durMs = durMs;
	c.canvas = QSize(16, 16);
	return c;
}

int grayAt(const QImage &im)
{
	return qGray(im.pixel(8, 8));
}

} // namespace

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const ComponentRegistry reg = makeRegistry();

	std::printf("\n-- no envelope renders exactly as before --\n");
	{
		// The compatibility check. Every project written before this feature
		// has in=out=0, and must render identically.
		ComponentStack st({inst("test.flood", 0, 0)}, reg);
		QImage a(16, 16, QImage::Format_RGBA8888);
		a.fill(Qt::black);
		st.evaluatePixels(at(0, 4000), a);
		QImage b(16, 16, QImage::Format_RGBA8888);
		b.fill(Qt::black);
		st.evaluatePixels(at(4000, 4000), b);
		std::printf("     first frame %d, last frame %d (full flood = 255)\n", grayAt(a), grayAt(b));
		ok(grayAt(a) == 255 && grayAt(b) == 255,
		   "with both ramps zero the component is full strength at both ends");
	}

	std::printf("\n-- a Pixel component cross-fades --\n");
	{
		ComponentStack st({inst("test.flood", 1000, 1000)}, reg);
		auto render = [&](qint64 t) {
			QImage im(16, 16, QImage::Format_RGBA8888);
			im.fill(Qt::black); // the input: the component floods it white
			st.evaluatePixels(at(t, 4000), im);
			return grayAt(im);
		};
		const int start = render(0), quarter = render(250), half = render(500), hold = render(2000),
			  end = render(4000);
		std::printf("     t=0 %d  t=250 %d  t=500 %d  hold %d  t=4000 %d\n", start, quarter, half,
			    hold, end);
		ok(start <= 2, "at the very start the component is absent -- the input shows through");
		ok(hold == 255, "on the hold it is fully applied");
		ok(end <= 2, "and by the end it has faded back out");
		ok(half > start && half < hold, "half-way up the ramp is genuinely half-applied");
		ok(quarter < half, "and the ramp is ordered, not jumping to full");
	}

	std::printf("\n-- a Transform component ramps its POSE, not its pixels --\n");
	{
		// The case a blend-everything rule gets wrong: half a move must be the
		// clip half-way across, not a ghost of both positions.
		ComponentStack st({inst("test.shove", 1000, 0)}, reg);
		TlTransform seed; // posX 0.5, scale 1.0
		const ClipState s0 = st.evaluatePose(at(0, 4000), seed);
		const ClipState sHalf = st.evaluatePose(at(500, 4000), seed);
		const ClipState sFull = st.evaluatePose(at(2000, 4000), seed);
		std::printf("     posX: t=0 %.3f  t=500 %.3f  hold %.3f   (seed 0.5, target 1.0)\n",
			    s0.xf.posX, sHalf.xf.posX, sFull.xf.posX);
		ok(std::abs(s0.xf.posX - 0.5) < 1e-6, "at the start the pose is still the clip's own");
		ok(std::abs(sFull.xf.posX - 1.0) < 1e-6, "on the hold it is what the component asked for");
		ok(sHalf.xf.posX > 0.5 && sHalf.xf.posX < 1.0, "and part-way is genuinely part-way THERE");
		// Every channel ramps, not just the one: scale 1.0 -> 2.0.
		ok(sHalf.xf.scale > 1.0 && sHalf.xf.scale < 2.0, "scale ramps too, from the clip's own value");
		ok(std::abs(s0.xf.scale - 1.0) < 1e-6, "starting from the seed, not from zero");
	}

	std::printf("\n-- the envelope composes with keyframes --\n");
	{
		// The keyframes say what the value is; the envelope says how much of
		// the component is present. Both, not either.
		ComponentInstance c = inst("test.flood", 1000, 0);
		c.keys[QStringLiteral("amount")] = {{0, 0.4, TlEase::Linear}, {4000, 0.4, TlEase::Linear}};
		ComponentStack st({c}, reg);
		auto render = [&](qint64 t) {
			QImage im(16, 16, QImage::Format_RGBA8888);
			im.fill(Qt::black);
			st.evaluatePixels(at(t, 4000), im);
			return grayAt(im);
		};
		const int hold = render(2000);
		const int mid = render(500);
		std::printf("     keyframed amount 0.4 -> hold %d, mid-ramp %d\n", hold, mid);
		ok(hold > 90 && hold < 115, "on the hold the KEYFRAME's value is what renders (~102)");
		ok(mid > 0 && mid < hold, "and during the ramp that keyframed value fades in");
	}

	std::printf("\n-- a disabled component is still disabled --\n");
	{
		ComponentInstance c = inst("test.flood", 500, 500);
		c.enabled = false;
		ComponentStack st({c}, reg);
		QImage im(16, 16, QImage::Format_RGBA8888);
		im.fill(Qt::black);
		st.evaluatePixels(at(2000, 4000), im);
		ok(grayAt(im) == 0, "CONTROL: the envelope does not resurrect a switched-off component");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
