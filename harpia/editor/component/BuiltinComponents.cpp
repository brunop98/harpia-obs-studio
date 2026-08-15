#include "BuiltinComponents.hpp"

#include "../timeline/EffectClip.hpp" // the effect table, and Effects::apply
#include "../timeline/Spotlight.hpp" // blurInPlace -- one blur in the program, not two
#include "ComponentRegistry.hpp"

#include <QImage>
#include <QPainter>

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

// ---- Always Grow ---------------------------------------------------------
// Always Rotate's counterpart for size, and the one place the two deliberately
// differ. Rotation has no natural end, so it is stated as a RATE. Growth does
// have one -- the clip's last frame -- so it is stated as a DESTINATION.
// "Half again as big by the end" still means that after the clip is trimmed,
// retimed or moved to a project at another frame rate; "0.2 per second" quietly
// means something different after every one of those.
//
// Pure, like everything here, and for the reason the header gives: it reads
// ctx.u(), the clip's own 0..1 progress, so scrubbing to the middle shows the
// middle size whether or not a single frame before it was ever rendered.
class AlwaysGrowComponent : public IComponent {
public:
	void evaluate(const EvalContext &ctx, ClipState &io) const override
	{
		// Clamped at zero: a negative scale is a mirror, which is a different
		// feature wearing this one's clothes.
		const double endScale = std::max(0.0, ctx.f("endScale", 1.5));
		const TlEase ease = tlEaseFromInt(int(std::lround(ctx.f("ease", 0.0))));
		// *= starting from 1, not =, so the clip's own scale -- and its scale
		// keyframes, and any other Transform component -- still mean what they
		// meant. This multiplies the size the clip already has instead of
		// replacing it, the same way Always Rotate adds to its rotation.
		// tlEaseAt clamps u itself, so a clip evaluated past its end holds at
		// the final size rather than sailing on past it.
		io.xf.scale *= 1.0 + (endScale - 1.0) * tlEaseAt(ease, ctx.u());
	}
};

// ---- Always Zoom ---------------------------------------------------------
// The RATE half of the pair Always Grow is the destination half of. Grow is
// told where to end up and works out its own pace from the clip's length; Zoom
// is told a pace and holds it for as long as the clip lasts. Both exist because
// both questions get asked -- "end up half again as big" and "drift in slowly,
// I'll decide the length later" -- and answering one with the other means doing
// arithmetic the component should be doing.
//
// Per second, like Always Rotate's degrees per second, so every "Always"
// component reads as a rate in the units of the thing it moves.
class AlwaysZoomComponent : public IComponent {
public:
	void evaluate(const EvalContext &ctx, ClipState &io) const override
	{
		const double pctPerSec = ctx.f("percentPerSecond", 10.0);
		// Linear in the clip's OWN size: 10 means "a tenth of its original size
		// every second", so ten seconds at 10 is twice the size. Deliberately
		// not compounded. 1.1^t looks the same to the eye over a second or two
		// and turns a plausible-looking number into 2.6x over ten seconds,
		// which nobody predicts from the number they typed.
		const double factor = 1.0 + (pctPerSec / 100.0) * ctx.tSec();
		// A clip cannot be zoomed away to nothing: at -10%/s, a clip past ten
		// seconds would reach zero and then go negative, and a negative scale
		// is a mirror, not a zoom.
		io.xf.scale *= std::max(0.01, factor);
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

// ---- Mask ----------------------------------------------------------------
// Cut the clip to a shape. Outside it becomes TRANSPARENT rather than dark, so
// whatever is on the tracks below shows through -- which is what makes
// picture-in-picture, shaped reveals and "show only this corner" work. The
// dimming version of the same idea is Inverse Selection, which is a clip of its
// own because it grades the whole composition rather than one layer.
//
// The shape is described in the CLIP's own space, not the canvas: a mask
// belongs to the thing it is cutting, so moving or zooming the clip carries it
// along instead of leaving the picture sliding around behind a fixed hole.
//
// The geometry comes from Spotlight::maskPath, so there is one description of
// what a rounded rectangle is in this program rather than two that can drift.
class MaskComponent : public IComponent {
public:
	void evaluate(const EvalContext &ctx, ClipState &io) const override
	{
		if (!io.frame || io.frame->isNull())
			return;
		QImage &frame = *io.frame;
		if (frame.format() != QImage::Format_RGBA8888)
			frame = frame.convertToFormat(QImage::Format_RGBA8888);

		SpotMask m;
		m.shape = spotShapeFromInt(int(std::lround(ctx.f("shape", 1.0))));
		SpotPose pose;
		pose.cx = ctx.f("centreX", 0.5);
		pose.cy = ctx.f("centreY", 0.5);
		pose.w = std::max(0.0, ctx.f("width", 0.5));
		pose.h = std::max(0.0, ctx.f("height", 0.5));
		pose.rotation = ctx.f("rotation", 0.0);
		pose.radius = std::clamp(ctx.f("corner", 0.15), 0.0, 0.5);
		const bool invert = ctx.b("invert", false);
		const double feather = std::clamp(ctx.f("feather", 0.02), 0.0, 1.0);

		// The coverage map: white where the clip survives, black where it does
		// not, at the frame's own size so the edge lands on the same pixels the
		// frame has.
		//
		// White-on-BLACK in a colour channel, not white-on-transparent in the
		// alpha one, because blurInPlace does not touch alpha at all -- it says
		// so itself, and feathering silently did nothing until that was read
		// rather than assumed. RGBA8888 for the same kind of reason: the blur
		// converts anything else to it, which would move the buffer out from
		// under the read below.
		QImage cover(frame.size(), QImage::Format_RGBA8888);
		cover.fill(Qt::black);
		{
			QPainter p(&cover);
			p.setRenderHint(QPainter::Antialiasing, true);
			p.setPen(Qt::NoPen);
			p.setBrush(Qt::white);
			p.drawPath(Spotlight::maskPath(m, pose, frame.size()));
		}
		// Feather as a fraction of the shorter side, the same rule the effects
		// use, so a mask looks the same at any render size -- including the
		// reduced-resolution preview.
		const int soft = int(std::lround(feather * 0.25 *
						 std::min(frame.width(), frame.height())));
		if (soft > 0)
			Spotlight::blurInPlace(cover, soft);

		// Multiply the frame's alpha by the coverage. Done by hand rather than
		// through QPainter's DestinationIn so the frame stays RGBA8888
		// throughout -- the composition modes want premultiplied ARGB, and
		// converting there and back costs two full passes per frame.
		const int w = frame.width(), h = frame.height();
		for (int y = 0; y < h; ++y) {
			uchar *row = frame.scanLine(y);
			const uchar *cov = cover.constScanLine(y);
			for (int x = 0; x < w; ++x) {
				// Red carries the coverage; the three colour channels are
				// identical here, since the map is only ever black or white.
				const int c = cov[x * 4 + 0];
				const int a = invert ? 255 - c : c;
				uchar &alpha = row[x * 4 + 3];
				alpha = uchar((int(alpha) * a + 127) / 255);
			}
		}
	}
};

// ---- Transform -----------------------------------------------------------
// The pose every clip has. Unlike the others this one ASSIGNS rather than
// offsets, because it is not a modifier — it is where the clip is.
//
// Its values live in the clip's own fields, not in a ComponentInstance, and the
// compositor still seeds ClipState::xf from transformAt(). This type exists so
// the Inspector has a schema to render the pinned row from: one declaration of
// the labels, ranges and help, shared with every other component's row rather
// than hand-built a second time. Moving the storage is a separate job, because
// the keyframe editor, the preview drag and the project format are all built on
// those fields and none of that has to change for the panel to be unified.
class TransformComponent : public IComponent {
public:
	void evaluate(const EvalContext &ctx, ClipState &io) const override
	{
		io.xf.posX = ctx.f("posX", io.xf.posX);
		io.xf.posY = ctx.f("posY", io.xf.posY);
		io.xf.scale = ctx.f("scale", io.xf.scale);
		io.xf.rotation = ctx.f("rotation", io.xf.rotation);
		io.xf.opacity = std::clamp(ctx.f("opacity", io.xf.opacity), 0.0, 1.0);
	}
};

// ---- The effect family ---------------------------------------------------
// One component per FxType, GENERATED from the table Effects already keeps
// rather than fifteen hand-written registrations. The names, ranges and
// defaults therefore cannot drift from the renderer's, and a new effect becomes
// a component by existing.
//
// Each one builds a one-off FxSpec and hands it to Effects::apply, so there is
// still exactly one implementation of Brightness in the program. This is a port
// of where an effect is CONFIGURED, not a reimplementation of what it does.
class FxComponent : public IComponent {
public:
	explicit FxComponent(FxType t) : type_(t) {}

	void evaluate(const EvalContext &ctx, ClipState &io) const override
	{
		if (!io.frame || io.frame->isNull())
			return;
		FxSpec spec;
		spec.type = type_;
		spec.enabled = true;
		for (const FxParamDef &d : fxParams(type_))
			spec.params.insert(QString::fromLatin1(d.key),
					   ctx.f(d.key, d.def));
		// tMs 0, not ctx.tMs: the component's own keyframes have already been
		// resolved into ctx.p, so letting FxSpec interpolate again would be a
		// second animation system fighting the first.
		Effects::apply(*io.frame, spec, 0);
	}

private:
	FxType type_;
};

// "Hue shift" -> "harpia.fx.hueShift". Derived rather than listed so a new
// effect cannot be given an id in one place and forgotten in another.
QString fxComponentId(FxType t)
{
	const QString name = QString::fromLatin1(fxTypeName(t));
	QString camel;
	bool up = false;
	for (const QChar ch : name) {
		if (ch == QChar(' ')) {
			up = true;
			continue;
		}
		camel += up ? ch.toUpper() : (camel.isEmpty() ? ch.toLower() : ch);
		up = false;
	}
	return QStringLiteral("harpia.fx.") + camel;
}

} // namespace

QString effectComponentId(FxType t)
{
	return fxComponentId(t);
}

void registerBuiltinComponents(ComponentRegistry &reg)
{
	// Every effect, as a component. Inverse Selection is skipped: its settings
	// are a list of shapes rather than a handful of numbers, so it needs an
	// editor of its own and is ported separately.
	for (int i = 0; i < kFxTypeCount; ++i) {
		const FxType ft = FxType(i);
		if (ft == FxType::InverseSelection)
			continue;
		ComponentType t;
		t.id = fxComponentId(ft);
		t.displayName = QString::fromLatin1(fxTypeName(ft));
		t.category = QStringLiteral("Effect");
		t.stage = Stage::Pixel;
		for (const FxParamDef &d : fxParams(ft)) {
			PropDef p;
			p.key = QString::fromLatin1(d.key);
			p.label = QString::fromLatin1(d.label);
			p.type = PropType::Float;
			p.min = d.lo;
			p.max = d.hi;
			p.def = d.def;
			t.props.append(p);
		}
		t.help = QString::fromLatin1(fxHelp(ft));
		t.pointOp = fxIsPointOp(ft);
		t.make = [ft] { return std::unique_ptr<IComponent>(new FxComponent(ft)); };
		reg.add(t);
	}
	{
		ComponentType t;
		t.id = QStringLiteral("harpia.transform");
		t.displayName = QStringLiteral("Transform");
		t.category = QStringLiteral("Transform");
		t.stage = Stage::Transform;
		t.addable = false; // every clip has one; a second would fight the first
		t.help = QStringLiteral("Where the clip sits on the canvas. Every clip has one.");
		t.props = {
			{QStringLiteral("scale"), QStringLiteral("Zoom"), PropType::Float, 0.05, 8.0,
			 1.0, true, QStringLiteral("1 fits the canvas; above that zooms in.")},
			{QStringLiteral("posX"), QStringLiteral("Position X"), PropType::Float, -1.0,
			 2.0, 0.5, true, QStringLiteral("0.5 is the middle of the canvas.")},
			{QStringLiteral("posY"), QStringLiteral("Position Y"), PropType::Float, -1.0,
			 2.0, 0.5, true, QStringLiteral("0.5 is the middle of the canvas.")},
			{QStringLiteral("rotation"), QStringLiteral("Rotation"), PropType::Float,
			 -360.0, 360.0, 0.0, true, QStringLiteral("Degrees clockwise.")},
			{QStringLiteral("opacity"), QStringLiteral("Opacity"), PropType::Float, 0.0,
			 1.0, 1.0, true, QString()},
		};
		// Reset used to be a button in the Inspector, sitting next to the
		// Transform section but owned by the window. It belongs to the
		// component: it knows what its own defaults are, and putting it here is
		// what proves an action can come from a component rather than from a
		// hand-placed widget.
		//
		// Writing every property explicitly rather than clearing the bag: an
		// empty bag means "no value set", which resolves to the same numbers
		// today but stops doing so the moment a property gains a keyframe.
		ComponentAction reset;
		reset.id = QStringLiteral("reset");
		reset.label = QStringLiteral("Reset transform");
		reset.help = QStringLiteral("Put the clip back to the middle at its natural size, "
					    "and clear its pose animation.");
		reset.run = [](PropBag &p) {
			p[QStringLiteral("scale")] = 1.0;
			p[QStringLiteral("posX")] = 0.5;
			p[QStringLiteral("posY")] = 0.5;
			p[QStringLiteral("rotation")] = 0.0;
			p[QStringLiteral("opacity")] = 1.0;
		};
		t.actions.append(reset);
		t.make = [] { return std::unique_ptr<IComponent>(new TransformComponent); };
		reg.add(t);
	}
	{
		ComponentType t;
		t.id = QStringLiteral("harpia.speed");
		t.displayName = QStringLiteral("Speed");
		t.category = QStringLiteral("Time");
		t.stage = Stage::Time;
		// Not addable, for the same reason Transform is not: every clip plays at
		// some rate, so it is a thing a clip IS rather than one it HAS. It is
		// pinned in the Inspector and backed by the clip's own speed field,
		// which is what makes the strip resize when you speed a clip up. A
		// second, addable Speed would be a different Speed — one that changes
		// which frame is sampled without changing the length — and two controls
		// with one name is worse than either.
		t.addable = false;
		t.help = QStringLiteral("How fast the clip plays. 2 is double speed, and its length "
					"on the timeline halves to match.");
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
		// Motion, with the rest of the family. Transform is now what it says on
		// the tin -- the static pose -- and everything that moves a clip over
		// time is in one menu, next to the Pulse script that named the category.
		t.category = QStringLiteral("Motion");
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
		t.id = QStringLiteral("harpia.alwaysGrow");
		t.displayName = QStringLiteral("Always Grow");
		// Motion, not Transform: this was asked for as "a component for motion",
		// and Motion is a category that already exists -- the bundled Pulse
		// script declares it. Filing it under Transform put it in a different
		// menu from its own family, which is exactly where it was not found.
		t.category = QStringLiteral("Motion");
		t.stage = Stage::Transform;
		t.help = QStringLiteral("Grow the clip steadily, reaching its final size on the "
					"last frame. Multiplies whatever size the clip already "
					"has, so it composes with the clip's own scale.");
		t.props = {
			{QStringLiteral("endScale"), QStringLiteral("Final size"), PropType::Float,
			 0.0, 10.0, 1.5, true,
			 QStringLiteral("A multiple of the clip's own size, reached on its last "
					"frame. 1 is unchanged, 2 is twice as big, 0.5 shrinks "
					"by half. The clip is at its own size on the first "
					"frame whatever this says.")},
			// An Int, like Mask's shape and for the same reason: it lands on
			// whole numbers between keys, so animating it steps cleanly from
			// one curve to the next rather than resolving to half an ease.
			{QStringLiteral("ease"), QStringLiteral("Ease"), PropType::Int, 0.0, 3.0,
			 0.0, true,
			 QStringLiteral("How the growth is paced: 0 linear, 1 ease in, 2 ease "
					"out, 3 ease in-out.")},
		};
		t.make = [] { return std::unique_ptr<IComponent>(new AlwaysGrowComponent); };
		reg.add(t);
	}
	{
		ComponentType t;
		t.id = QStringLiteral("harpia.alwaysZoom");
		t.displayName = QStringLiteral("Always Zoom");
		t.category = QStringLiteral("Motion");
		t.stage = Stage::Transform;
		t.help = QStringLiteral("Zoom the clip continuously, at a steady rate for as long "
					"as the clip lasts. Multiplies whatever size the clip "
					"already has, so it composes with the clip's own scale.");
		t.props = {{QStringLiteral("percentPerSecond"), QStringLiteral("Zoom % / second"),
			    PropType::Float, -50.0, 200.0, 10.0, true,
			    QStringLiteral("A percentage of the clip's own size, added every "
					   "second. 10 makes it twice its size after ten "
					   "seconds. Negative zooms out.")}};
		t.make = [] { return std::unique_ptr<IComponent>(new AlwaysZoomComponent); };
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
	{
		ComponentType t;
		t.id = QStringLiteral("harpia.mask");
		t.displayName = QStringLiteral("Mask");
		t.category = QStringLiteral("Pixel");
		t.stage = Stage::Pixel;
		t.help = QStringLiteral(
			"Cut the clip to a shape. Everything outside becomes transparent, so the "
			"tracks below show through. The shape is in the clip's own space, so it "
			"moves and zooms with the clip.");
		t.props = {
			// Keyframeable, like everything else. An Int lands on whole
			// numbers between keys, so animating this steps cleanly from one
			// shape to the next rather than resolving to half a shape.
			{QStringLiteral("shape"), QStringLiteral("Shape"), PropType::Int, 0.0, 3.0,
			 1.0, true,
			 QStringLiteral("0 rectangle, 1 rounded rectangle, 2 circle, 3 ellipse.")},
			{QStringLiteral("centreX"), QStringLiteral("Centre X"), PropType::Float, -0.5,
			 1.5, 0.5, true, QStringLiteral("0.5 is the middle of the clip.")},
			{QStringLiteral("centreY"), QStringLiteral("Centre Y"), PropType::Float, -0.5,
			 1.5, 0.5, true, QStringLiteral("0.5 is the middle of the clip.")},
			{QStringLiteral("width"), QStringLiteral("Width"), PropType::Float, 0.0, 2.0,
			 0.5, true, QStringLiteral("As a fraction of the clip's width. A circle "
						   "takes its size from this alone.")},
			{QStringLiteral("height"), QStringLiteral("Height"), PropType::Float, 0.0,
			 2.0, 0.5, true, QStringLiteral("As a fraction of the clip's height.")},
			{QStringLiteral("rotation"), QStringLiteral("Rotation"), PropType::Float,
			 -180.0, 180.0, 0.0, true, QStringLiteral("Degrees clockwise, about the "
								  "shape's own centre.")},
			{QStringLiteral("corner"), QStringLiteral("Corner radius"), PropType::Float,
			 0.0, 0.5, 0.15, true,
			 QStringLiteral("Rounded rectangle only, as a fraction of the shorter side.")},
			{QStringLiteral("feather"), QStringLiteral("Feather"), PropType::Float, 0.0,
			 1.0, 0.02, true, QStringLiteral("Soften the edge. 0 is a hard cut.")},
			// A Bool holds between its keys rather than interpolating, so
			// keying this switches the cut at the key and not halfway to it.
			{QStringLiteral("invert"), QStringLiteral("Invert"), PropType::Bool, 0.0, 1.0,
			 0.0, true,
			 QStringLiteral("Cut the shape OUT of the clip instead of keeping it.")},
		};
		// Not a point op: a feathered edge reads its neighbours through the blur,
		// and the coverage map is built from the whole frame's geometry either
		// way. Handing this a sub-rect would move the shape.
		t.make = [] { return std::unique_ptr<IComponent>(new MaskComponent); };
		reg.add(t);
	}
}

} // namespace harpia
