#include "Component.hpp"

#include <algorithm>

namespace harpia {

const char *stageName(Stage s)
{
	switch (s) {
	case Stage::Time: return "Time";
	case Stage::Source: return "Source";
	case Stage::Transform: return "Transform";
	case Stage::Pixel: return "Pixel";
	case Stage::Composite: return "Composite";
	case Stage::Audio: return "Audio";
	}
	return "Pixel";
}

namespace {

const PropDef *defFor(const ComponentType &type, const QString &key)
{
	for (const PropDef &d : type.props)
		if (d.key == key)
			return &d;
	return nullptr;
}

// Keyframes hold doubles whatever the property's type is, so an animated Int or
// Bool lands back on its own type here rather than leaking a fractional value
// into a control that cannot show one.
QVariant coerce(const PropDef &d, double v)
{
	switch (d.type) {
	case PropType::Int: return int(std::lround(v));
	case PropType::Bool: return v >= 0.5;
	case PropType::Float:
	case PropType::Color: break;
	}
	return v;
}

double interpolate(const QVector<PropKey> &keys, qint64 tMs)
{
	if (keys.isEmpty())
		return 0.0;
	if (keys.size() == 1 || tMs <= keys.front().tMs)
		return keys.front().v;
	if (tMs >= keys.back().tMs)
		return keys.back().v;
	int i = 0;
	while (i + 1 < keys.size() && keys[i + 1].tMs <= tMs)
		++i;
	const PropKey &a = keys[i];
	const PropKey &b = keys[i + 1];
	const qint64 span = std::max<qint64>(1, b.tMs - a.tMs);
	const double u = tlEaseAt(a.ease, double(tMs - a.tMs) / double(span), a.bez1, a.bez2);
	return a.v + (b.v - a.v) * u;
}

} // namespace

QVariant propAt(const ComponentType &type, const ComponentInstance &inst, const QString &key,
		qint64 tMs)
{
	const PropDef *d = defFor(type, key);
	if (!d)
		return {};

	// Keyframes win over the static value: they are what the user sees moving,
	// and a static field that silently overrode them is how an Inspector comes
	// to look broken (the Spotlight pose fields did exactly that).
	const auto k = inst.keys.find(key);
	if (k != inst.keys.end() && !k->isEmpty())
		return coerce(*d, interpolate(*k, tMs));

	const auto p = inst.props.find(key);
	if (p != inst.props.end() && p->isValid())
		return *p;
	return coerce(*d, d->def);
}

PropBag resolveProps(const ComponentType &type, const ComponentInstance &inst, qint64 tMs)
{
	PropBag out;
	for (const PropDef &d : type.props)
		out.insert(d.key, propAt(type, inst, d.key, tMs));
	return out;
}

} // namespace harpia
