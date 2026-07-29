#include "ComponentStack.hpp"

#include <QImage>

namespace harpia {

ComponentStack::ComponentStack(const QVector<ComponentInstance> &list, const ComponentRegistry &reg)
	: owned_(list)
{
	const QVector<int> order = resolveOrder(owned_, reg, &warnings_);
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
		// Properties are resolved per component per frame, not shared: two
		// instances of the same type on one clip must see their own settings.
		EvalContext ctx = base;
		ctx.p = resolveProps(*e.type, *e.inst, base.tMs);
		e.impl->evaluate(ctx, io);
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
