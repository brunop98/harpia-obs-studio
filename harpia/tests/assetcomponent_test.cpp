// Shaders and transform scripts, as components.
//
// Both used to be panels of their own with their own list, their own ordering
// and their own parameter controls. As components they have to earn the same
// things the built-ins get, and the ways that can go wrong are all invisible
// from a screenshot:
//
//   - a file in the folder that registers no component, so "Add Component"
//     simply does not offer it;
//   - a shader's //@param lines not becoming properties, leaving a component
//     with nothing to tune;
//   - a script component that overwrites the clip's pose instead of composing
//     with it, making the Transform row a dead control;
//   - an old project's `scripts` list not becoming components, so reopening it
//     loses the animation;
//   - or becoming components TWICE, so every script runs twice.
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ComponentStack.hpp"
#include "editor/component/ShaderComponent.hpp"
#include "editor/component/TransformScriptComponent.hpp"
#include "editor/timeline/TimelineJson.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static void eq(double got, double want, const char *w, double tol = 1e-6)
{
	const bool g = std::abs(got - want) <= tol;
	std::printf("  %s %s (got %g, want %g)\n", g ? "PASS" : "FAIL", w, got, want);
	if (!g)
		++failures;
}
static void write(const QString &path, const QByteArray &body)
{
	QFile f(path);
	f.open(QIODevice::WriteOnly);
	f.write(body);
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	QTemporaryDir dir;
	const QString shaders = dir.path() + QStringLiteral("/shaders");
	const QString scripts = dir.path() + QStringLiteral("/scripts");
	QDir().mkpath(shaders);
	QDir().mkpath(scripts);

	write(shaders + QStringLiteral("/tint.frag"),
	      "//@param strength float 0 1 0.4 Strength\n"
	      "//@param warm bool 1 Warm\n"
	      "void mainImage(out vec4 o, in vec2 p){ o = vec4(strength); }\n");
	// Not a .frag: it must not become a component just for being in the folder.
	write(shaders + QStringLiteral("/notes.txt"), "hello\n");

	write(scripts + QStringLiteral("/half.js"),
	      "//@param amount float 0 1 0.5 Amount\n"
	      "function scale(t, u, dur, ctx) { return ctx.base.scale * amount; }\n");
	write(scripts + QStringLiteral("/spin.js"),
	      "function rotation(t, u, dur, ctx) { return ctx.base.rotation + 90; }\n");

	ComponentRegistry reg;
	QStringList errs;
	const int nsh = ShaderComponents::loadFolder(shaders, reg, &errs);
	const int nsc = TransformScriptComponents::loadFolder(scripts, reg, &errs);

	std::printf("\n-- a file in the folder is a component you can add --\n");
	{
		ok(nsh == 1, "the one .frag registered, and the .txt did not");
		ok(nsc == 2, "both .js files registered");
		const ComponentType *sh = reg.find(shaderComponentId(QStringLiteral("tint")));
		const ComponentType *sc =
			reg.find(transformScriptComponentId(QStringLiteral("half")));
		ok(sh && sc, "under ids derived from the file names");
		if (!sh || !sc)
			return 1;
		ok(sh->addable && sc->addable,
		   "addable -- which is the whole point, they were panels before");
		ok(sh->category == QStringLiteral("Shader") &&
			   sc->category == QStringLiteral("Script"),
		   "in categories of their own in the Add Component menu");
		ok(sh->stage == Stage::Pixel, "a shader is Pixel work");
		ok(sc->stage == Stage::Transform, "a script poses the clip");
	}

	std::printf("\n-- a //@param line becomes a property --\n");
	{
		const ComponentType *sh = reg.find(shaderComponentId(QStringLiteral("tint")));
		ok(sh->props.size() == 2, "both of the shader's params");
		bool strength = false, warm = false;
		for (const PropDef &d : sh->props) {
			if (d.key == QStringLiteral("strength"))
				strength = d.type == PropType::Float && d.def == 0.4 &&
					   d.label == QStringLiteral("Strength");
			if (d.key == QStringLiteral("warm"))
				warm = d.type == PropType::Bool;
		}
		ok(strength, "the float one with its range, default and label");
		ok(warm, "and the bool one as a checkbox rather than a slider");
		const ComponentType *sc =
			reg.find(transformScriptComponentId(QStringLiteral("half")));
		ok(sc->props.size() == 1 && sc->props[0].key == QStringLiteral("amount"),
		   "a script's params come through the same way");
	}

	std::printf("\n-- a script component composes with the pose, not over it --\n");
	{
		QVector<ComponentInstance> comps;
		ComponentInstance ci;
		ci.typeId = transformScriptComponentId(QStringLiteral("half"));
		ci.instanceId = QStringLiteral("a");
		ci.props.insert(QStringLiteral("amount"), 0.5);
		comps.append(ci);

		ComponentStack stack(comps, reg);
		EvalContext ctx;
		ctx.tMs = 0;
		ctx.durMs = 1000;
		ctx.fps = 30;
		ctx.canvas = QSize(640, 360);
		TlTransform seed;
		seed.scale = 2.0;   // what the clip is set to by hand
		seed.rotation = 10; // a channel this script does not define
		const ClipState st = stack.evaluatePose(ctx, seed);

		// 2.0 * 0.5. Had the script ignored ctx.base it would read 0.5, and the
		// Transform row would be a control that does nothing.
		eq(st.xf.scale, 1.0, "it multiplied the clip's own scale");
		eq(st.xf.rotation, 10.0, "and left the channel it does not drive alone");
	}

	std::printf("\n-- two scripts stack in list order --\n");
	{
		QVector<ComponentInstance> comps;
		for (const char *n : {"half", "spin"}) {
			ComponentInstance ci;
			ci.typeId = transformScriptComponentId(QString::fromLatin1(n));
			ci.instanceId = QString::fromLatin1(n);
			ci.props.insert(QStringLiteral("amount"), 0.5);
			comps.append(ci);
		}
		ComponentStack stack(comps, reg);
		EvalContext ctx;
		ctx.tMs = 0;
		ctx.durMs = 1000;
		ctx.fps = 30;
		ctx.canvas = QSize(640, 360);
		TlTransform seed;
		seed.scale = 2.0;
		seed.rotation = 0;
		const ClipState st = stack.evaluatePose(ctx, seed);
		eq(st.xf.scale, 1.0, "the first one still scaled");
		eq(st.xf.rotation, 90.0, "and the second one still rotated");
	}

	std::printf("\n-- a project from before the port opens with its scripts --\n");
	{
		ComponentRegistry::instance().clear();
		TransformScriptComponents::loadFolder(scripts, ComponentRegistry::instance());

		const QByteArray json = R"({
			"type": "video", "sourceId": 1, "srcStart": 0, "srcEnd": 1000,
			"outStart": 0,
			"scripts": [ {"name":"half","params":{"amount":0.25}},
				     {"name":"spin","params":{}} ]
		})";
		const QJsonObject co = QJsonDocument::fromJson(json).object();
		TlClip c = clipFromJson(co);
		ok(c.components.size() == 2, "both scripts became components");
		if (c.components.size() == 2) {
			ok(c.components[0].typeId ==
				   transformScriptComponentId(QStringLiteral("half")),
			   "in the order they ran in");
			eq(c.components[0].props.value(QStringLiteral("amount")).toDouble(), 0.25,
			   "carrying the value the clip had set");
		}

		// Save and reopen: the clip still carries `scripts` for older readers,
		// so a second pass must NOT convert them again.
		const QJsonObject saved = clipToJson(c);
		const TlClip again = clipFromJson(saved);
		std::printf("     after a save and reload: %d component(s)\n",
			    int(again.components.size()));
		ok(again.components.size() == 2, "a second open does not double them");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
