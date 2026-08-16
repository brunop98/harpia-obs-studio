#include "Component.hpp"

#include <QColor>

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

const PropDef *defFor(const ComponentType &type, const QString &key)
{
	for (const PropDef &d : type.props)
		if (d.key == key)
			return &d;
	return nullptr;
}

namespace {

// Keyframes hold doubles whatever the property's type is, so an animated Int or
// Bool lands back on its own type here rather than leaking a fractional value
// into a control that cannot show one.
QVariant coerce(const PropDef &d, double v)
{
	switch (d.type) {
	case PropType::Int: return int(std::lround(v));
	case PropType::Bool: return v >= 0.5;
	// A colour travels as a packed 0xAARRGGBB in the double. 32 bits is exact
	// in a double, so nothing is lost, and it means one PropKey type covers
	// every property rather than a variant per kind.
	case PropType::Color: return QColor::fromRgba(QRgb(quint32(std::llround(v))));
	case PropType::Float: break;
	}
	return v;
}

// Where we are between two keys, 0..1, after easing. -1 when the answer is
// simply "the key at index `i`" (before the first, after the last, or only one).
struct Span {
	int i = 0;
	double u = -1.0;
};
Span spanAt(const QVector<PropKey> &keys, qint64 tMs)
{
	if (keys.size() == 1 || tMs <= keys.front().tMs)
		return {0, -1.0};
	if (tMs >= keys.back().tMs)
		return {int(keys.size()) - 1, -1.0};
	int i = 0;
	while (i + 1 < keys.size() && keys[i + 1].tMs <= tMs)
		++i;
	const PropKey &a = keys[i];
	const qint64 span = std::max<qint64>(1, keys[i + 1].tMs - a.tMs);
	return {i, tlEaseAt(a.ease, double(tMs - a.tMs) / double(span), a.bez1, a.bez2)};
}

double interpolate(const QVector<PropKey> &keys, qint64 tMs)
{
	if (keys.isEmpty())
		return 0.0;
	const Span s = spanAt(keys, tMs);
	if (s.u < 0.0)
		return keys[s.i].v;
	return keys[s.i].v + (keys[s.i + 1].v - keys[s.i].v) * s.u;
}
} // namespace


// One property's value at one instant, from its keys.
//
// Not every kind of value can be slid between two numbers, which is why this
// exists instead of interpolate() feeding coerce():
//
//   - a Bool HOLDS. Lerping it and rounding at 0.5 would flip it halfway
//     between two keys, at a moment the user never chose and cannot see in the
//     Inspector -- "on at 0s, on at 2s" is unambiguous, but "off at 0s, on at
//     2s" must switch AT 2s, not at 1s.
//   - a Colour has to move channel by channel. The packed 0xAARRGGBB is a
//     number, but sliding between two of them runs through whatever bit
//     patterns lie in between: red to blue would pass through green and
//     overflow into the alpha byte.
QVariant valueFromKeys(const PropDef &d, const QVector<PropKey> &keys, qint64 tMs)
{
	if (keys.isEmpty())
		return coerce(d, d.def);
	const Span s = spanAt(keys, tMs);

	if (d.type == PropType::Bool) {
		// The key at or before now, held. u is ignored entirely.
		return keys[s.i].v >= 0.5;
	}
	if (d.type == PropType::Color) {
		const QColor a = QColor::fromRgba(QRgb(quint32(std::llround(keys[s.i].v))));
		if (s.u < 0.0)
			return a;
		const QColor b = QColor::fromRgba(QRgb(quint32(std::llround(keys[s.i + 1].v))));
		const auto mix = [u = s.u](int x, int y) {
			return std::clamp(int(std::lround(x + (y - x) * u)), 0, 255);
		};
		return QColor(mix(a.red(), b.red()), mix(a.green(), b.green()),
			      mix(a.blue(), b.blue()), mix(a.alpha(), b.alpha()));
	}
	return coerce(d, interpolate(keys, tMs));
}

namespace {

// One property's value, with its definition already in hand.
//
// Split out of propAt so resolveProps can skip the defFor() lookup: it walks
// type.props, so it is holding each PropDef when it asks, and looking the same
// one back up BY NAME made resolving a component's properties quadratic in the
// number of properties. That happens per component per clip per frame, and a
// twelve-parameter shader was paying about seventy string comparisons a frame
// to rediscover twelve things it already had.
QVariant propFor(const PropDef &d, const ComponentInstance &inst, qint64 tMs)
{
	// Keyframes win over the static value: they are what the user sees moving,
	// and a static field that silently overrode them is how an Inspector comes
	// to look broken (the Spotlight pose fields did exactly that).
	const auto k = inst.keys.find(d.key);
	if (k != inst.keys.end() && !k->isEmpty())
		return valueFromKeys(d, *k, tMs);

	const auto p = inst.props.find(d.key);
	if (p != inst.props.end() && p->isValid())
		return *p;
	return coerce(d, d.def);
}

} // namespace

double propKeyValue(const PropDef &def, const QVariant &v)
{
	if (def.type == PropType::Color) {
		// Round-trips with coerce(): the same packed 0xAARRGGBB, in a double
		// that holds 32 bits exactly.
		const QColor c = v.canConvert<QColor>() ? v.value<QColor>()
							: QColor::fromRgba(QRgb(v.toUInt()));
		return double(quint32(c.rgba()));
	}
	return v.toDouble(); // bool gives 0 or 1, which is what a held key wants
}

QVariant propAt(const ComponentType &type, const ComponentInstance &inst, const QString &key,
		qint64 tMs)
{
	const PropDef *d = defFor(type, key);
	return d ? propFor(*d, inst, tMs) : QVariant();
}

PropBag resolveProps(const ComponentType &type, const ComponentInstance &inst, qint64 tMs)
{
	PropBag out;
	for (const PropDef &d : type.props)
		out.insert(d.key, propFor(d, inst, tMs)); // the def is in hand; don't re-find it
	return out;
}

} // namespace harpia
