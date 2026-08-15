#include "ComponentStack.hpp"

#include "ComponentEnvelope.hpp"

#include <QImage>
#include <QPainter>

namespace harpia {

ComponentStack::ComponentStack(const QVector<ComponentInstance> &list, const ComponentRegistry &reg,
			       bool conflicts)
	: owned_(list)
{
	const QVector<int> order = resolveOrder(owned_, reg, &warnings_);
	if (conflicts)
		warnings_ += findConflicts(owned_, reg);
	pure_ = allPure(owned_, reg);

	entries_.reserve(order.size());
	for (int idx : order) {
		const ComponentInstance &inst = owned_[idx];
		const ComponentType *type = reg.find(inst.typeId);
		if (!type)
			continue; // already reported by resolveOrder; nothing to run
		Entry e;
		e.index = idx;
		e.stage = type->stage;
		e.type = type;
		e.inst = &owned_[idx];
		e.impl = type->make ? std::shared_ptr<IComponent>(type->make().release()) : nullptr;
		if (e.impl)
			entries_.append(e);
	}
}

bool ComponentStack::pixelStageIsPointOp() const
{
	for (const Entry &e : entries_) {
		if (e.stage != Stage::Pixel || !e.inst->enabled)
			continue;
		if (!e.type->pointOp)
			return false;
	}
	return true;
}

void ComponentStack::run(Stage stage, const EvalContext &base, ClipState &io) const
{
	for (const Entry &e : entries_) {
		if (e.stage != stage || !e.inst->enabled)
			continue;

		// In / Out ramp. Zero on both (the default, and every project written
		// before this) yields exactly 1 and takes none of the paths below --
		// so the old behaviour is not merely preserved, it is the same code.
		const double w = componentWeight(base.tMs, base.durMs, e.inst->inMs, e.inst->outMs);
		if (envelopeIsEmpty(w))
			continue; // none of this component is present yet: skip it whole

		// Properties are resolved per component per frame, not shared: two
		// instances of the same type on one clip must see their own settings.
		EvalContext ctx = base;
		ctx.p = resolveProps(*e.type, *e.inst, base.tMs);

		if (envelopeIsFull(w)) {
			e.impl->evaluate(ctx, io); // the hold: no ramp work at all
			continue;
		}

		// Part-way through a ramp. Rather than asking every component what its
		// "no effect" value would be -- which nothing declares, and which a
		// third-party component would get wrong -- run it normally and
		// interpolate its EFFECT ON THE STATE back toward what the state was
		// before it. That is a true parameter ramp for poses and a cross-fade
		// for pixels, from one rule, with no per-component knowledge.
		if (stage == Stage::Transform || stage == Stage::Time) {
			const TlTransform before = io.xf;
			const double beforeScale = io.timeScale;
			e.impl->evaluate(ctx, io);
			// Lerping the resulting POSE (not the image) is what makes a
			// Transform component fade in as movement instead of as a double
			// exposure.
			const auto mix = [w](double a, double b) { return a + (b - a) * w; };
			io.xf.posX = mix(before.posX, io.xf.posX);
			io.xf.posY = mix(before.posY, io.xf.posY);
			io.xf.scale = mix(before.scale, io.xf.scale);
			io.xf.rotation = mix(before.rotation, io.xf.rotation);
			io.xf.opacity = mix(before.opacity, io.xf.opacity);
			io.timeScale = mix(beforeScale, io.timeScale);
			continue;
		}

		if (io.frame && !io.frame->isNull()) {
			// Pixel / Composite: cross-fade the component's output with its
			// input. For effects that are linear in the image -- saturation,
			// brightness, tint -- this is exactly a parameter ramp; for the
			// rest it is the sensible reading of "half applied". The copy
			// costs only during the ramp, never on the hold.
			const QImage input = io.frame->copy();
			e.impl->evaluate(ctx, io);
			QPainter p(io.frame);
			p.setOpacity(1.0 - w);
			p.setCompositionMode(QPainter::CompositionMode_SourceOver);
			p.drawImage(0, 0, input); // paint the ORIGINAL back over, at 1-w
			continue;
		}

		e.impl->evaluate(ctx, io); // Audio and anything with no frame yet
	}
}

ClipState ComponentStack::evaluatePose(const EvalContext &base, const TlTransform &seed) const
{
	ClipState st;
	st.xf = seed;
	st.frame = nullptr; // pixels do not exist yet, and the type says so
	run(Stage::Time, base, st);
	run(Stage::Source, base, st);
	run(Stage::Transform, base, st);
	return st;
}

void ComponentStack::evaluatePixels(const EvalContext &base, QImage &frame) const
{
	ClipState st;
	st.frame = &frame;
	run(Stage::Pixel, base, st);
	run(Stage::Composite, base, st);
}

} // namespace harpia
