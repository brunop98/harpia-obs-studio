// Effects, as components.
//
// Every FxType is now a Pixel component, generated from the table Effects
// already keeps rather than fifteen hand-written registrations — so a new
// effect becomes a component by existing, and the names and ranges cannot drift
// from the renderer's.
//
// Two things need proving. An effect component must produce the SAME picture
// the old fx path did, because a port that quietly changes what Brightness
// looks like has broken every project that used it. And a project written
// before the port has to open with its grade intact.
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ComponentStack.hpp"
#include "editor/timeline/EffectClip.hpp"
#include "editor/timeline/TimelineCompositor.hpp"
#include "editor/timeline/TimelineJson.hpp"

#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QPainter>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static QImage scene()
{
	QImage im(160, 120, QImage::Format_RGBA8888);
	for (int y = 0; y < 120; ++y)
		for (int x = 0; x < 160; ++x)
			im.setPixelColor(x, y, QColor((x * 37) % 256, (y * 53) % 256, (x * y) % 256));
	return im;
}

static int worstDiff(const QImage &a, const QImage &b)
{
	int w = 0;
	for (int y = 0; y < a.height(); ++y)
		for (int x = 0; x < a.width(); ++x) {
			const QColor ca = a.pixelColor(x, y), cb = b.pixelColor(x, y);
			w = std::max({w, std::abs(ca.red() - cb.red()),
				      std::abs(ca.green() - cb.green()),
				      std::abs(ca.blue() - cb.blue())});
		}
	return w;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);
	ComponentRegistry reg;
	registerBuiltinComponents(reg);

	std::printf("\n-- every effect became a component --\n");
	{
		int missing = 0;
		for (int i = 0; i < kFxTypeCount; ++i) {
			const FxType ft = FxType(i);
			if (ft == FxType::InverseSelection)
				continue; // ported separately: it edits shapes, not numbers
			const ComponentType *t = reg.find(effectComponentId(ft));
			if (!t) {
				std::printf("     MISSING %s -> %s\n", fxTypeName(ft),
					    qPrintable(effectComponentId(ft)));
				++missing;
				continue;
			}
			if (t->props.size() != fxParams(ft).size()) {
				std::printf("     %s has %d props, the effect has %d\n",
					    fxTypeName(ft), int(t->props.size()),
					    int(fxParams(ft).size()));
				++missing;
			}
		}
		ok(missing == 0, "all of them, with the same parameters the effect declares");
		ok(reg.find(effectComponentId(FxType::HueShift)) != nullptr,
		   "and a two-word name becomes a sensible id (harpia.fx.hueShift)");
	}

	std::printf("\n-- and renders exactly what the old path rendered --\n");
	{
		// The check that makes this a port rather than a rewrite. Every effect,
		// at a non-default parameter value, both ways.
		int mismatches = 0;
		for (int i = 0; i < kFxTypeCount; ++i) {
			const FxType ft = FxType(i);
			if (ft == FxType::InverseSelection)
				continue;
			const QVector<FxParamDef> defs = fxParams(ft);

			FxSpec spec;
			spec.type = ft;
			spec.enabled = true;
			spec.params = fxDefaults(ft);
			// Push one parameter off its default so a no-op cannot pass.
			if (!defs.isEmpty())
				spec.params[QString::fromLatin1(defs[0].key)] =
					defs[0].def + (defs[0].hi - defs[0].def) * 0.5;

			QImage viaFx = scene();
			Effects::apply(viaFx, spec, 0);

			ComponentInstance ci;
			ci.typeId = effectComponentId(ft);
			ci.instanceId = QStringLiteral("c");
			for (auto it = spec.params.cbegin(); it != spec.params.cend(); ++it)
				ci.props.insert(it.key(), it.value());
			QImage viaComponent = scene();
			EvalContext ctx;
			ctx.canvas = QSize(160, 120);
			ComponentStack(QVector<ComponentInstance>{ci}, reg)
				.evaluatePixels(ctx, viaComponent);

			const int d = worstDiff(viaFx, viaComponent);
			if (d != 0) {
				std::printf("     %-22s worst channel difference %d\n", fxTypeName(ft), d);
				++mismatches;
			}
		}
		ok(mismatches == 0, "pixel for pixel, all of them");
	}

	std::printf("\n-- an effect component's keyframes animate it --\n");
	{
		ComponentInstance ci;
		ci.typeId = effectComponentId(FxType::Brightness);
		ci.instanceId = QStringLiteral("c");
		ci.keys.insert(QStringLiteral("amount"),
			       {PropKey{0, 0.0, TlEase::Linear, 0.42, 0.58},
				PropKey{1000, 1.0, TlEase::Linear, 0.42, 0.58}});
		ComponentStack st(QVector<ComponentInstance>{ci}, reg);
		EvalContext a;
		a.canvas = QSize(160, 120);
		a.tMs = 0;
		a.durMs = 1000;
		EvalContext b = a;
		b.tMs = 1000;
		QImage at0 = scene(), at1 = scene();
		st.evaluatePixels(a, at0);
		st.evaluatePixels(b, at1);
		ok(worstDiff(at0, scene()) == 0, "amount 0 at the start leaves the frame alone");
		ok(worstDiff(at1, at0) > 20, "and it is much brighter by the end");
	}

	std::printf("\n-- a project from before the port still opens with its grade --\n");
	{
		// The migration that "replace outright, migrate on open" actually means.
		ComponentRegistry::instance().clear();
		registerBuiltinComponents(ComponentRegistry::instance());

		const char *raw = R"({"type":"effect","srcStart":0,"srcEnd":3000,"outStart":0,
		    "fx":{"kind":4,"enabled":true,"params":{"degrees":45.0},
		          "keys":[{"t":0,"params":{"degrees":0.0}},
		                  {"t":2000,"params":{"degrees":180.0}}]}})";
		const QJsonObject o = QJsonDocument::fromJson(raw).object();
		const TlClip c = clipFromJson(o);
		if (!c.components.isEmpty())
			std::printf("     it is %s\n", qPrintable(c.components[0].typeId));

		std::printf("     migrated to %d component(s)\n", int(c.components.size()));
		ok(c.components.size() == 1, "the fx became a component");
		if (c.components.isEmpty())
			return failures ? 1 : 0;
		ok(c.components[0].typeId == effectComponentId(FxType::HueShift),
		   "of the right kind");
		ok(c.components[0].keys.value(QStringLiteral("degrees")).size() == 2,
		   "and its keyframes came across");

		// Re-reading what we just wrote must not migrate a second time and end
		// up applying the hue shift twice.
		const TlClip again = clipFromJson(clipToJson(c));
		std::printf("     after a save and reload: %d component(s)\n",
			    int(again.components.size()));
		ok(again.components.size() == 1, "a second open does not duplicate it");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
