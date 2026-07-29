#include "BuiltinComponents.hpp"

#include "../timeline/Spotlight.hpp" // blurInPlace -- one blur in the program, not two
#include "ComponentRegistry.hpp"

#include <QImage>

#include <algorithm>
#include <cmath>

namespace harpia {

namespace {

// ---- Speed ---------------------------------------------------------------
// A time-domain component, not an effect: it changes which source instant this
// output frame samples. That is why Stage::Time exists and why Speed cannot be
// dragged below a Blur — time is settled before there are pixels to blur.
class SpeedComponent : public IComponent {
public:
	void evaluate(const EvalContext &ctx, ClipState &io) const override
	{
		const double f = ctx.f("factor", 1.0);
		// Multiplied, so two time components compose rather than the last one
		// winning. Clamped away from zero: a factor of 0 is a still frame, and
		// dividing by it later is a crash rather than a look.
		io.timeScale *= std::max(0.01, f);
	}
};

// ---- Always Rotate -------------------------------------------------------
// The component that makes the purity rule concrete. Written the Unity way —
// `rotation += speed * dt` — this is correct during playback and wrong the
// moment anyone drags the playhead, because the accumulated value depends on
// how many frames happened to be rendered. As a function of t it is right
// everywhere, including on an export worker rendering frames out of order.
class AlwaysRotateComponent : public IComponent {
public:
	void evaluate(const EvalContext &ctx, ClipState &io) const override
	{
		const double dps = ctx.f("degreesPerSecond", 90.0);
		// += not =, so it composes with the clip's own rotation and with any
		// other Transform component instead of overwriting their work.
		io.xf.rotation += dps * ctx.tSec();
	}
};

// ---- Blur ----------------------------------------------------------------
class BlurComponent : public IComponent {
public:
	void evaluate(const EvalContext &ctx, ClipState &io) const override
	{
		if (!io.frame || io.frame->isNull())
			return;
		const double amount = std::clamp(ctx.f("radius", 0.15), 0.0, 1.0);
		// Radius as a fraction of the shorter side, so a clip looks the same at
		// any render size — including the reduced-resolution preview. Same rule
		// the existing effects use.
		const int px = int(std::lround(amount * 0.08 *
					       std::min(io.frame->width(), io.frame->height())));
		if (px > 0)
			Spotlight::blurInPlace(*io.frame, px);
	}
};

} // namespace

void registerBuiltinComponents(ComponentRegistry &reg)
{
	{
		ComponentType t;
		t.id = QStringLiteral("harpia.speed");
		t.displayName = QStringLiteral("Speed");
		t.category = QStringLiteral("Time");
		t.stage = Stage::Time;
		t.help = QStringLiteral("Play the clip faster or slower. 2 is double speed.");
		t.props = {{QStringLiteral("factor"), QStringLiteral("Factor"), PropType::Float, 0.1,
			    10.0, 1.0, true,
			    QStringLiteral("Above 1 is faster, below 1 is slower.")}};
		t.make = [] { return std::unique_ptr<IComponent>(new SpeedComponent); };
		reg.add(t);
	}
	{
		ComponentType t;
		t.id = QStringLiteral("harpia.alwaysRotate");
		t.displayName = QStringLiteral("Always Rotate");
		t.category = QStringLiteral("Transform");
		t.stage = Stage::Transform;
		t.help = QStringLiteral("Spin the clip continuously. Adds to whatever rotation the "
					"clip already has.");
		t.props = {{QStringLiteral("degreesPerSecond"), QStringLiteral("Degrees / second"),
			    PropType::Float, -720.0, 720.0, 90.0, true,
			    QStringLiteral("Negative turns anticlockwise.")}};
		t.make = [] { return std::unique_ptr<IComponent>(new AlwaysRotateComponent); };
		reg.add(t);
	}
	{
		ComponentType t;
		t.id = QStringLiteral("harpia.blur");
		t.displayName = QStringLiteral("Blur");
		t.category = QStringLiteral("Pixel");
		t.stage = Stage::Pixel;
		t.help = QStringLiteral("Soften the picture.");
		t.props = {{QStringLiteral("radius"), QStringLiteral("Radius"), PropType::Float, 0.0,
			    1.0, 0.15, true,
			    QStringLiteral("As a fraction of the frame's shorter side, so it "
					   "looks the same at any render size.")}};
		t.make = [] { return std::unique_ptr<IComponent>(new BlurComponent); };
		reg.add(t);
	}
}

} // namespace harpia
