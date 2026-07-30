// Every component property can be keyframed, including the ones that are not
// numbers you can slide between.
//
// The machinery was already universal -- PropDef::keyframeable defaults to true
// and propAt() consults the keys for any key at all -- but three kinds of
// property were shut out or would have produced nonsense:
//
//   - Bool had no key button at all, and would have been WRONG if it had one:
//     lerping 0 to 1 and rounding at 0.5 flips the switch halfway between two
//     keys, at an instant the user never chose and cannot see anywhere.
//   - Colour resolved through a plain double. Sliding between two packed
//     0xAARRGGBB numbers runs through whatever bit patterns lie between them --
//     red to blue passes through green and spills into the alpha byte -- and
//     keying one at all wrote 0, because QVariant(QColor).toDouble() is 0.
//   - script, shader and transform-script parameters were declared keyframeable
//     only when they were Floats, so a plugin's integer or switch could not be
//     animated even though the resolver would have handled it.
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/Component.hpp"
#include "editor/component/ComponentRegistry.hpp"

#include <QColor>
#include <QGuiApplication>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// A type with one property of each kind, so the resolver is exercised on all
// four rather than on whichever ones the built-ins happen to use.
static ComponentType allKinds()
{
	ComponentType t;
	t.id = QStringLiteral("test.allkinds");
	t.displayName = QStringLiteral("All kinds");
	t.stage = Stage::Pixel;
	PropDef f;
	f.key = QStringLiteral("amount");
	f.type = PropType::Float;
	f.min = 0;
	f.max = 10;
	f.def = 1;
	PropDef i;
	i.key = QStringLiteral("count");
	i.type = PropType::Int;
	i.min = 0;
	i.max = 100;
	i.def = 4;
	PropDef b;
	b.key = QStringLiteral("invert");
	b.type = PropType::Bool;
	b.def = 0;
	PropDef c;
	c.key = QStringLiteral("tint");
	c.type = PropType::Color;
	c.def = double(quint32(QColor(Qt::black).rgba()));
	t.props = {f, i, b, c};
	return t;
}

static ComponentInstance keyed(const QString &key, QVector<PropKey> ks)
{
	ComponentInstance ci;
	ci.typeId = QStringLiteral("test.allkinds");
	ci.instanceId = QStringLiteral("x");
	ci.keys.insert(key, ks);
	return ci;
}
static PropKey k(qint64 t, double v)
{
	PropKey p;
	p.tMs = t;
	p.v = v;
	p.ease = TlEase::Linear;
	return p;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);
	registerBuiltinComponents(ComponentRegistry::instance());
	const ComponentType t = allKinds();

	std::printf("\n-- every kind of property declares itself animatable --\n");
	{
		for (const PropDef &d : t.props)
			ok(d.keyframeable,
			   QByteArray("a ")
				   .append(d.type == PropType::Bool	 ? "Bool"
					   : d.type == PropType::Int	 ? "Int"
					   : d.type == PropType::Color	 ? "Color"
								       : "Float")
				   .append(" property is keyframeable by default")
				   .constData());
	}

	std::printf("\n-- a Float slides, as it always did --\n");
	{
		const ComponentInstance ci = keyed(QStringLiteral("amount"), {k(0, 0.0), k(1000, 10.0)});
		std::printf("     0ms %.2f   500ms %.2f   1000ms %.2f\n",
			    propAt(t, ci, QStringLiteral("amount"), 0).toDouble(),
			    propAt(t, ci, QStringLiteral("amount"), 500).toDouble(),
			    propAt(t, ci, QStringLiteral("amount"), 1000).toDouble());
		ok(std::abs(propAt(t, ci, QStringLiteral("amount"), 500).toDouble() - 5.0) < 0.01,
		   "halfway between the keys is halfway between the values");
	}

	std::printf("\n-- an Int lands on whole numbers --\n");
	{
		const ComponentInstance ci = keyed(QStringLiteral("count"), {k(0, 0.0), k(1000, 3.0)});
		const QVariant mid = propAt(t, ci, QStringLiteral("count"), 500);
		std::printf("     500ms -> %s (type %s)\n", qPrintable(mid.toString()),
			    mid.typeName());
		// Not 1.5. A control that shows a count cannot show one and a half, and
		// a component reading it with toInt() would silently truncate.
		ok(mid.userType() == QMetaType::Int, "an animated Int stays an Int");
		ok(mid.toInt() == 2, "rounded, not truncated");
	}

	std::printf("\n-- a Bool HOLDS between keys, it does not fade across --\n");
	{
		// off at 0, on at 1000. The switch must flip AT 1000.
		const ComponentInstance ci = keyed(QStringLiteral("invert"), {k(0, 0.0), k(1000, 1.0)});
		const auto at = [&](qint64 ms) {
			return propAt(t, ci, QStringLiteral("invert"), ms).toBool();
		};
		std::printf("     0ms %d  499ms %d  500ms %d  999ms %d  1000ms %d\n", at(0), at(499),
			    at(500), at(999), at(1000));
		ok(!at(0), "off at the first key");
		// The whole point. Interpolating and rounding would turn this on at
		// 500ms -- a moment nothing in the UI mentions.
		ok(!at(500), "still off halfway between, where a lerp would have flipped it");
		ok(!at(999), "still off just before the second key");
		ok(at(1000), "and on exactly at the second key");
		ok(at(5000), "and stays on afterwards");

		ok(propAt(t, ci, QStringLiteral("invert"), 0).userType() == QMetaType::Bool,
		   "and it is a real bool, not a number");
	}

	std::printf("\n-- a Colour moves channel by channel --\n");
	{
		const double red = double(quint32(QColor(255, 0, 0).rgba()));
		const double blue = double(quint32(QColor(0, 0, 255).rgba()));
		const ComponentInstance ci = keyed(QStringLiteral("tint"), {k(0, red), k(1000, blue)});
		const QVariant midV = propAt(t, ci, QStringLiteral("tint"), 500);
		ok(midV.canConvert<QColor>(), "an animated Colour resolves to a colour");
		const QColor mid = midV.value<QColor>();
		std::printf("     0ms %s   500ms rgb(%d,%d,%d)   1000ms %s\n",
			    qPrintable(propAt(t, ci, QStringLiteral("tint"), 0).value<QColor>().name()),
			    mid.red(), mid.green(), mid.blue(),
			    qPrintable(propAt(t, ci, QStringLiteral("tint"), 1000).value<QColor>().name()));
		ok(propAt(t, ci, QStringLiteral("tint"), 0).value<QColor>() == QColor(255, 0, 0),
		   "it starts on the first key's colour");
		ok(propAt(t, ci, QStringLiteral("tint"), 1000).value<QColor>() == QColor(0, 0, 255),
		   "and ends on the second's");
		// Sliding the PACKED number instead would run red -> blue straight
		// through green and out the top of the blue byte into alpha.
		ok(std::abs(mid.red() - 128) <= 2 && std::abs(mid.blue() - 128) <= 2,
		   "halfway is half of each channel");
		ok(mid.green() == 0, "and a channel that never changes never moves");
		ok(mid.alpha() == 255, "the alpha byte is not trampled by the interpolation");
	}

	std::printf("\n-- keying a value round-trips, whatever its type --\n");
	{
		// propKeyValue is the inverse of what propAt hands out. Without it,
		// keying a colour writes QVariant(QColor).toDouble(), which is 0 --
		// black -- and the key looks like it did not take.
		for (const PropDef &d : t.props) {
			QVariant v;
			switch (d.type) {
			case PropType::Float: v = 3.25; break;
			case PropType::Int: v = 7; break;
			case PropType::Bool: v = true; break;
			case PropType::Color: v = QColor(12, 200, 90, 128); break;
			}
			ComponentInstance ci;
			ci.typeId = t.id;
			ci.keys.insert(d.key, {k(0, propKeyValue(d, v))});
			const QVariant back = propAt(t, ci, d.key, 0);
			std::printf("     %-7s %-22s -> %s\n", qPrintable(d.key),
				    qPrintable(v.toString().isEmpty() ? v.value<QColor>().name()
								      : v.toString()),
				    qPrintable(back.toString().isEmpty() ? back.value<QColor>().name()
									 : back.toString()));
			if (d.type == PropType::Color)
				ok(back.value<QColor>() == v.value<QColor>(),
				   "a keyed Colour comes back unchanged, alpha included");
			else
				ok(back == v, "a keyed value comes back unchanged");
		}
	}

	std::printf("\n-- and the built-ins agree --\n");
	{
		// The Mask carries a Bool (invert) and an Int (shape) alongside its
		// floats. Every one of them has to be offerable, or "everything is
		// keyframeable" is only true of the ones that were already easy.
		const ComponentType *m = ComponentRegistry::instance().find(QStringLiteral("harpia.mask"));
		ok(m != nullptr, "the Mask component is registered");
		if (m) {
			int nonKeyable = 0;
			for (const PropDef &d : m->props)
				if (!d.keyframeable)
					++nonKeyable;
			std::printf("     %d properties, %d of them not keyframeable\n",
				    int(m->props.size()), nonKeyable);
			ok(nonKeyable == 0, "every one of its properties can be keyed");

			// And its bool really holds, through the registered type rather
			// than the hand-made one above.
			ComponentInstance ci;
			ci.typeId = m->id;
			ci.keys.insert(QStringLiteral("invert"), {k(0, 0.0), k(1000, 1.0)});
			ok(!propAt(*m, ci, QStringLiteral("invert"), 600).toBool(),
			   "and its invert switch holds between keys too");
		}
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
