// A component written by a user, in JavaScript.
//
// The point of the whole design is that a scripted component is not a
// second-class citizen: same registry, same ordering, same keyframes, same
// purity contract. So this checks it the same way the built-ins were checked —
// including that a script evaluated cold at frame 900 agrees with one walked
// there, and that two threads rendering at once get the same answer, since each
// has to compile the source into its own QuickJS runtime.
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ComponentStack.hpp"
#include "editor/component/ScriptComponent.hpp"

#include <QGuiApplication>
#include <QThread>

#include <atomic>
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
	const bool good = std::abs(got - want) <= tol;
	std::printf("  %s %s (got %g, want %g)\n", good ? "PASS" : "FAIL", w, got, want);
	if (!good)
		++failures;
}

static EvalContext at(qint64 tMs, qint64 durMs = 10000)
{
	EvalContext c;
	c.tMs = tMs;
	c.outMs = tMs;
	c.durMs = durMs;
	c.fps = 30.0;
	c.canvas = QSize(1920, 1080);
	return c;
}

static ComponentInstance inst(const QString &type)
{
	ComponentInstance c;
	c.typeId = type;
	c.instanceId = QStringLiteral("i1");
	return c;
}

// The example from the header: a component nobody at Harpia wrote.
static const char *kPulse = R"JS(
//@component acme.pulse
//@name Pulse
//@category Motion
//@stage Transform
//@param bpm float 30 240 120 Beats per minute
//@param depth float 0 1 0.5 Depth

function evaluate(ctx, io) {
    const beat = Math.sin(ctx.t * Math.PI * ctx.p.bpm / 30) * 0.5 + 0.5;
    io.scale *= 1 + ctx.p.depth * beat;
}
)JS";

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);

	if (!ScriptComponents::available()) {
		std::printf("\nSKIPPED: this build has no scripting engine.\n");
		return 0;
	}

	ComponentRegistry reg;

	std::printf("\n-- loading a user's component --\n");
	{
		QString err;
		ok(ScriptComponents::loadSource(QString::fromUtf8(kPulse), reg, &err),
		   "it loads");
		if (!err.isEmpty())
			std::printf("     error: %s\n", qPrintable(err));
		const ComponentType *t = reg.find(QStringLiteral("acme.pulse"));
		ok(t != nullptr, "and registers under its declared id");
		if (!t)
			return 1;
		ok(t->displayName == QStringLiteral("Pulse"), "with its name");
		ok(t->category == QStringLiteral("Motion"), "and category");
		ok(t->stage == Stage::Transform, "and stage");
		ok(t->props.size() == 2, "its //@param lines became properties");
		ok(t->props[0].key == QStringLiteral("bpm") && t->props[0].max == 240.0,
		   "with their ranges, so the Inspector can build a slider");
		ok(t->props[0].keyframeable, "and they are keyframeable like any built-in's");
	}

	std::printf("\n-- it actually drives the render --\n");
	{
		QVector<ComponentInstance> list{inst(QStringLiteral("acme.pulse"))};
		list[0].props.insert(QStringLiteral("bpm"), 120.0);
		list[0].props.insert(QStringLiteral("depth"), 0.5);
		ComponentStack st(list, reg);
		ok(st.warnings().isEmpty(), "no warnings");

		TlTransform seed; // scale 1.0
		// At t=0 the sine is 0, so beat = 0.5 and scale = 1 + 0.5*0.5.
		eq(st.evaluatePose(at(0), seed).xf.scale, 1.25, "at t=0");
		// bpm 120 -> the argument is t*pi*4; at t=0.125 s that is pi/2, sine 1,
		// beat 1, scale 1.5.
		eq(st.evaluatePose(at(125), seed).xf.scale, 1.5, "at the peak");
	}

	std::printf("\n-- it composes rather than overwriting --\n");
	{
		QVector<ComponentInstance> list{inst(QStringLiteral("acme.pulse"))};
		list[0].props.insert(QStringLiteral("depth"), 0.0); // no pulse at all
		ComponentStack st(list, reg);
		TlTransform seed;
		seed.scale = 3.0; // the clip's own hand-set zoom
		eq(st.evaluatePose(at(0), seed).xf.scale, 3.0,
		   "a script multiplying io.scale leaves hand-set values intact");
	}

	std::printf("\n-- keyframes work on a scripted component's properties --\n");
	{
		QVector<ComponentInstance> list{inst(QStringLiteral("acme.pulse"))};
		list[0].props.insert(QStringLiteral("bpm"), 120.0);
		list[0].keys.insert(QStringLiteral("depth"),
				    {PropKey{0, 0.0, TlEase::Linear, 0.42, 0.58},
				     PropKey{1000, 1.0, TlEase::Linear, 0.42, 0.58}});
		ComponentStack st(list, reg);
		// depth animates 0..1; at t=0 it is 0 whatever the beat, so scale is 1.
		eq(st.evaluatePose(at(0), TlTransform{}).xf.scale, 1.0, "depth 0 at the start");
		ok(st.evaluatePose(at(500), TlTransform{}).xf.scale != 1.0,
		   "and something else halfway through");
	}

	std::printf("\n-- purity: cold and walked agree --\n");
	{
		QVector<ComponentInstance> list{inst(QStringLiteral("acme.pulse"))};
		list[0].props.insert(QStringLiteral("bpm"), 97.0); // not a round number
		ComponentStack st(list, reg);
		const double cold = st.evaluatePose(at(9000), TlTransform{}).xf.scale;
		for (qint64 t = 0; t <= 9000; t += 33)
			st.evaluatePose(at(t), TlTransform{});
		const double walked = st.evaluatePose(at(9000), TlTransform{}).xf.scale;
		eq(walked, cold, "scrubbing to 9 s equals playing to 9 s");
	}

	std::printf("\n-- and two threads agree, each with its own runtime --\n");
	{
		// The preview renders on the GUI thread and the exporter on a worker; a
		// QuickJS runtime is not shared between them. If the per-thread
		// compilation were wrong, this is where it would show.
		QVector<ComponentInstance> list{inst(QStringLiteral("acme.pulse"))};
		list[0].props.insert(QStringLiteral("bpm"), 143.0);
		ComponentStack st(list, reg);
		const double here = st.evaluatePose(at(3210), TlTransform{}).xf.scale;

		std::atomic<double> there{-1.0};
		QThread *w = QThread::create([&] {
			there = st.evaluatePose(at(3210), TlTransform{}).xf.scale;
		});
		w->start();
		w->wait();
		delete w;
		std::printf("     GUI thread %.9f   worker %.9f\n", here, there.load());
		eq(there.load(), here, "the same frame on another thread");
	}

	std::printf("\n-- bad scripts fail at load, with a reason --\n");
	{
		QString err;
		ok(!ScriptComponents::loadSource(QStringLiteral("function evaluate(){}"), reg, &err),
		   "no //@component line is refused");
		ok(err.contains(QStringLiteral("//@component")), "and says so");

		err.clear();
		ok(!ScriptComponents::loadSource(
			   QStringLiteral("//@component bare\nfunction evaluate(){}"), reg, &err),
		   "an un-namespaced id is refused");
		ok(err.contains(QStringLiteral("namespace")), "and says why");

		err.clear();
		ok(!ScriptComponents::loadSource(
			   QStringLiteral("//@component a.b\n//@stage Pixel\nfunction evaluate(){}"),
			   reg, &err),
		   "a Pixel-stage script is refused");
		ok(err.contains(QStringLiteral("shader")),
		   "pointing at shaders, since per-pixel JS would be unusably slow");

		err.clear();
		ok(!ScriptComponents::loadSource(QStringLiteral("//@component a.c\nthis is not js"),
						 reg, &err),
		   "a syntax error is caught at load, not on every frame");

		err.clear();
		ok(!ScriptComponents::loadSource(
			   QStringLiteral("//@component a.d\nvar x = 1;"), reg, &err),
		   "a script with no evaluate() is refused");
	}

	std::printf("\n-- a throwing component is a no-op, not a broken render --\n");
	{
		QString err;
		ok(ScriptComponents::loadSource(
			   QStringLiteral("//@component a.throws\n//@stage Transform\n"
					  "function evaluate(ctx, io){ throw new Error('boom'); }"),
			   reg, &err),
		   "it loads (it compiles fine; it only throws when run)");
		QVector<ComponentInstance> list{inst(QStringLiteral("a.throws"))};
		ComponentStack st(list, reg);
		TlTransform seed;
		seed.scale = 2.0;
		eq(st.evaluatePose(at(0), seed).xf.scale, 2.0,
		   "the clip renders unchanged rather than the frame failing");
	}

	std::printf("\n-- a runaway script cannot hang the editor --\n");
	{
		QString err;
		ScriptComponents::loadSource(
			QStringLiteral("//@component a.hang\n//@stage Transform\n"
				       "function evaluate(ctx, io){ while(true){} }"),
			reg, &err);
		QVector<ComponentInstance> list{inst(QStringLiteral("a.hang"))};
		ComponentStack st(list, reg);
		QElapsedTimer timer;
		timer.start();
		st.evaluatePose(at(0), TlTransform{});
		const qint64 ms = timer.elapsed();
		std::printf("     infinite loop returned after %lld ms\n", (long long)ms);
		ok(ms < 1000, "the watchdog cut it off instead of freezing");
	}

	std::printf("\n-- reloading replaces, so hot reload works --\n");
	{
		const char *v2 = R"JS(
//@component acme.pulse
//@name Pulse Mk2
//@stage Transform
function evaluate(ctx, io) { io.scale *= 2; }
)JS";
		QString err;
		ok(ScriptComponents::loadSource(QString::fromUtf8(v2), reg, &err),
		   "the same id loads again");
		ok(reg.find(QStringLiteral("acme.pulse"))->displayName == QStringLiteral("Pulse Mk2"),
		   "and the registry holds the new one");
		QVector<ComponentInstance> list{inst(QStringLiteral("acme.pulse"))};
		ComponentStack st(list, reg);
		eq(st.evaluatePose(at(0), TlTransform{}).xf.scale, 2.0,
		   "the NEW body runs, not a cached copy of the old one");
	}

	std::printf("\n-- the impurity sniff test --\n");
	{
		ok(ScriptComponents::looksImpure(
			   QStringLiteral("var total = 0;\nfunction evaluate(c,io){ total++; }")),
		   "top-level mutable state is flagged");
		ok(!ScriptComponents::looksImpure(
			   QStringLiteral("const K = 3;\nfunction evaluate(c,io){ io.scale *= K; }")),
		   "a const is not");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
