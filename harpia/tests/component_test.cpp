// The component runtime.
//
// The claims worth testing are the ones the whole design rests on:
//
//  - A component is a pure function of time. Evaluating frame 900 without
//    having evaluated 0..899 gives the same answer as walking there. Get this
//    wrong and scrubbing disagrees with playback and export disagrees with
//    both, which is unfixable without redoing everything.
//  - Stage order is structural, not advisory: Speed cannot run after Blur
//    however the list is arranged.
//  - Nothing is lost on the way to disk and back, INCLUDING components this
//    build has never heard of.
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentJson.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ComponentStack.hpp"
#include "editor/shader/ShaderEffect.hpp"

#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}
static void eq(double got, double want, const char *w, double tol = 1e-9)
{
	const bool good = std::abs(got - want) <= tol;
	std::printf("  %s %s (got %g, want %g)\n", good ? "PASS" : "FAIL", w, got, want);
	if (!good)
		++failures;
}

static ComponentInstance inst(const char *type, const char *id)
{
	ComponentInstance c;
	c.typeId = QString::fromLatin1(type);
	c.instanceId = QString::fromLatin1(id);
	return c;
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

// A component that CHEATS: it accumulates across calls, the Unity way. Used to
// prove the purity test can actually tell the difference — without it, a test
// that only ever checks well-behaved components proves nothing.
class CheatingRotate : public IComponent {
public:
	void evaluate(const EvalContext &, ClipState &io) const override
	{
		angle_ += 3.0; // depends on how many times it has been called
		io.xf.rotation += angle_;
	}
	mutable double angle_ = 0.0;
};

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);

	ComponentRegistry reg;
	registerBuiltinComponents(reg);

	std::printf("\n-- the registry --\n");
	ok(reg.find(QStringLiteral("harpia.speed")) != nullptr, "Speed registered");
	ok(reg.find(QStringLiteral("harpia.blur")) != nullptr, "Blur registered");
	ok(reg.find(QStringLiteral("harpia.alwaysRotate")) != nullptr, "Always Rotate registered");
	ok(reg.find(QStringLiteral("nope.missing")) == nullptr, "and an unknown id is not");
	ok(reg.make(QStringLiteral("harpia.blur")) != nullptr, "a known type builds");
	ok(reg.make(QStringLiteral("nope.missing")) == nullptr, "an unknown one returns null");

	std::printf("\n-- purity: any frame, in any order, is the same frame --\n");
	{
		QVector<ComponentInstance> list{inst("harpia.alwaysRotate", "a")};
		list[0].props.insert(QStringLiteral("degreesPerSecond"), 90.0);
		ComponentStack st(list, reg);

		// Straight to 10 s, having rendered nothing before it — a scrub.
		const double jumped = st.evaluatePose(at(10000), TlTransform{}).xf.rotation;
		// The same instant, reached by playing every frame up to it.
		ClipState walked;
		for (qint64 t = 0; t <= 10000; t += 33)
			walked = st.evaluatePose(at(t), TlTransform{});
		const double played = st.evaluatePose(at(10000), TlTransform{}).xf.rotation;

		eq(jumped, 900.0, "90 deg/s for 10 s is 900 degrees");
		eq(played, jumped, "and playing there gives exactly the scrubbed answer");
		// Backwards too: an export worker may render frames in any order.
		eq(st.evaluatePose(at(2000), TlTransform{}).xf.rotation, 180.0,
		   "going back to 2 s still gives 2 s");
		ok(st.pure(), "the stack reports itself pure");
	}

	std::printf("\n-- and the test can tell when a component cheats --\n");
	{
		// Registered by hand: this is what a Unity-style `rotation += speed*dt`
		// component looks like from outside, and it must NOT pass the check
		// above.
		ComponentType t;
		t.id = QStringLiteral("test.cheat");
		t.displayName = QStringLiteral("Cheating Rotate");
		t.stage = Stage::Transform;
		t.pure = false;
		t.make = [] { return std::unique_ptr<IComponent>(new CheatingRotate); };
		ComponentRegistry r2;
		r2.add(t);

		QVector<ComponentInstance> list{inst("test.cheat", "c")};
		ComponentStack st(list, r2);
		const double first = st.evaluatePose(at(10000), TlTransform{}).xf.rotation;
		const double again = st.evaluatePose(at(10000), TlTransform{}).xf.rotation;
		std::printf("     same instant twice: %g then %g\n", first, again);
		ok(first != again, "an accumulating component gives a different answer each call");
		ok(!st.pure(), "and the stack reports itself impure, so the caller can serialise it");
	}

	std::printf("\n-- components compose instead of overwriting --\n");
	{
		QVector<ComponentInstance> list{inst("harpia.alwaysRotate", "a"),
						inst("harpia.alwaysRotate", "b")};
		list[0].props.insert(QStringLiteral("degreesPerSecond"), 10.0);
		list[1].props.insert(QStringLiteral("degreesPerSecond"), 5.0);
		ComponentStack st(list, reg);
		TlTransform seed;
		seed.rotation = 45.0; // the clip's own hand-set rotation
		eq(st.evaluatePose(at(1000), seed).xf.rotation, 45.0 + 10.0 + 5.0,
		   "two rotates and the clip's own value all add up");
	}

	std::printf("\n-- disabled components do nothing --\n");
	{
		QVector<ComponentInstance> list{inst("harpia.alwaysRotate", "a")};
		list[0].props.insert(QStringLiteral("degreesPerSecond"), 90.0);
		list[0].enabled = false;
		ComponentStack st(list, reg);
		eq(st.evaluatePose(at(10000), TlTransform{}).xf.rotation, 0.0, "and contribute nothing");
	}

	std::printf("\n-- stage order beats list order --\n");
	{
		// Deliberately arranged wrong: Blur first, Speed last. Time must still
		// resolve before pixels, because that is the pipeline's real shape and
		// not a preference.
		QVector<ComponentInstance> list{inst("harpia.blur", "b"), inst("harpia.speed", "s")};
		list[1].props.insert(QStringLiteral("factor"), 2.0);
		ComponentStack st(list, reg);
		const ClipState pose = st.evaluatePose(at(1000), TlTransform{});
		eq(pose.timeScale, 2.0, "Speed still ran, despite being listed after Blur");
		// And Blur did not run in the pose pass at all: there were no pixels.
		ok(pose.frame == nullptr, "the pose pass has no frame for a pixel component to touch");
	}

	std::printf("\n-- Speed is multiplied, so two of them compose --\n");
	{
		QVector<ComponentInstance> list{inst("harpia.speed", "s1"), inst("harpia.speed", "s2")};
		list[0].props.insert(QStringLiteral("factor"), 2.0);
		list[1].props.insert(QStringLiteral("factor"), 3.0);
		ComponentStack st(list, reg);
		eq(st.evaluatePose(at(0), TlTransform{}).timeScale, 6.0, "2x then 3x is 6x");
	}

	std::printf("\n-- a pixel component actually changes pixels --\n");
	{
		QVector<ComponentInstance> list{inst("harpia.blur", "b")};
		list[0].props.insert(QStringLiteral("radius"), 1.0);
		ComponentStack st(list, reg);

		QImage img(200, 200, QImage::Format_RGBA8888);
		for (int y = 0; y < 200; ++y)
			for (int x = 0; x < 200; ++x)
				img.setPixelColor(x, y, (x / 10) % 2 ? QColor(0, 0, 0)
								    : QColor(255, 255, 255));
		const QImage before = img.copy();
		st.evaluatePixels(at(0), img);
		ok(img != before, "the frame came back different");
		// A blur evens out a stripe pattern: the middle should no longer be
		// black or white.
		const int mid = QColor(img.pixel(100, 100)).lightness();
		std::printf("     centre lightness after blur: %d\n", mid);
		ok(mid > 40 && mid < 215, "and it is a blur, not a fill");
	}

	std::printf("\n-- keyframes drive properties, and beat the static value --\n");
	{
		QVector<ComponentInstance> list{inst("harpia.alwaysRotate", "a")};
		list[0].props.insert(QStringLiteral("degreesPerSecond"), 999.0); // must be ignored
		list[0].keys.insert(QStringLiteral("degreesPerSecond"),
				    {PropKey{0, 0.0, TlEase::Linear, 0.42, 0.58},
				     PropKey{1000, 100.0, TlEase::Linear, 0.42, 0.58}});
		ComponentStack st(list, reg);
		// At 500 ms the rate is halfway (50), and it has run for 0.5 s.
		eq(st.evaluatePose(at(500), TlTransform{}).xf.rotation, 25.0,
		   "the animated rate is used, not the static 999");
	}

	std::printf("\n-- missing components are reported, not fatal --\n");
	{
		QVector<ComponentInstance> list{inst("acme.notinstalled", "x"),
						inst("harpia.alwaysRotate", "a")};
		list[1].props.insert(QStringLiteral("degreesPerSecond"), 90.0);
		ComponentStack st(list, reg);
		ok(!st.warnings().isEmpty(), "a warning is raised");
		ok(st.warnings().first().contains(QStringLiteral("acme.notinstalled")),
		   "naming the component that is missing");
		eq(st.evaluatePose(at(1000), TlTransform{}).xf.rotation, 90.0,
		   "and the rest of the clip still renders");
	}

	std::printf("\n-- dependencies order within a stage --\n");
	{
		// Two transform components, B requiring A, listed B-first.
		ComponentRegistry r2;
		QVector<QString> ran;
		auto mk = [&ran](const char *tag) {
			struct Rec : IComponent {
				QVector<QString> *log;
				QString tag;
				void evaluate(const EvalContext &, ClipState &) const override
				{
					log->append(tag);
				}
			};
			auto *r = new Rec;
			r->log = &ran;
			r->tag = QString::fromLatin1(tag);
			return std::unique_ptr<IComponent>(r);
		};
		ComponentType a;
		a.id = QStringLiteral("t.a");
		a.stage = Stage::Transform;
		a.make = [&] { return mk("a"); };
		ComponentType b;
		b.id = QStringLiteral("t.b");
		b.stage = Stage::Transform;
		b.requiresIds = {QStringLiteral("t.a")};
		b.make = [&] { return mk("b"); };
		r2.add(a);
		r2.add(b);

		QVector<ComponentInstance> list{inst("t.b", "1"), inst("t.a", "2")};
		ComponentStack st(list, r2);
		st.evaluatePose(at(0), TlTransform{});
		std::printf("     ran: %s\n", qPrintable(ran.join(QStringLiteral(", "))));
		ok(ran.size() == 2 && ran[0] == QStringLiteral("a"),
		   "the required one runs first, whatever order the list is in");
	}

	std::printf("\n-- a cycle still renders, and says so --\n");
	{
		ComponentRegistry r2;
		ComponentType a;
		a.id = QStringLiteral("c.a");
		a.stage = Stage::Transform;
		a.requiresIds = {QStringLiteral("c.b")};
		a.make = [] { return std::unique_ptr<IComponent>(nullptr); };
		ComponentType b = a;
		b.id = QStringLiteral("c.b");
		b.requiresIds = {QStringLiteral("c.a")};
		// A real factory, so the entries survive construction.
		struct Nop : IComponent {
			void evaluate(const EvalContext &, ClipState &) const override {}
		};
		a.make = [] { return std::unique_ptr<IComponent>(new Nop); };
		b.make = [] { return std::unique_ptr<IComponent>(new Nop); };
		r2.add(a);
		r2.add(b);
		QVector<ComponentInstance> list{inst("c.a", "1"), inst("c.b", "2")};
		ComponentStack st(list, r2);
		bool mentioned = false;
		for (const QString &w : st.warnings())
			if (w.contains(QStringLiteral("Circular")))
				mentioned = true;
		ok(mentioned, "the cycle is reported rather than hanging or dropping the clip");
	}

	std::printf("\n-- to disk and back --\n");
	{
		ComponentInstance c = inst("harpia.blur", "abc");
		c.enabled = false;
		c.props.insert(QStringLiteral("radius"), 0.42);
		c.keys.insert(QStringLiteral("radius"),
			      {PropKey{0, 0.0, TlEase::Linear, 0.42, 0.58},
			       PropKey{2000, 1.0, TlEase::EaseInOut, 0.1, 0.9}});
		const ComponentInstance back = componentFromJson(componentToJson(c, reg), reg);
		ok(back.typeId == c.typeId && back.instanceId == c.instanceId, "type and id survive");
		ok(back.enabled == false, "the disabled flag survives");
		eq(back.props.value(QStringLiteral("radius")).toDouble(), 0.42, "the value survives");
		ok(back.keys.value(QStringLiteral("radius")).size() == 2, "both keyframes survive");
		ok(back.keys.value(QStringLiteral("radius"))[1].ease == TlEase::EaseInOut &&
			   std::abs(back.keys.value(QStringLiteral("radius"))[1].bez1 - 0.1) < 1e-9,
		   "including their easing and its handles");
	}

	std::printf("\n-- and a component this build has never heard of --\n");
	{
		// The case that decides whether someone can safely open a colleague's
		// project without every plugin installed.
		const char *raw = R"({"type":"acme.glitch","id":"g1","version":3,
		                      "props":{"amount":0.7},
		                      "secretSauce":{"lut":[1,2,3]},"futureField":"keep me"})";
		const QJsonObject in = QJsonDocument::fromJson(raw).object();
		const ComponentInstance c = componentFromJson(in, reg); // reg has no acme.glitch
		ok(c.typeId == QStringLiteral("acme.glitch"), "it still parses");
		eq(c.props.value(QStringLiteral("amount")).toDouble(), 0.7, "its known props are read");
		ok(c.unknown.contains(QStringLiteral("secretSauce")) &&
			   c.unknown.contains(QStringLiteral("futureField")),
		   "and everything unrecognised is kept");

		const QJsonObject out = componentToJson(c, reg);
		ok(out.value(QStringLiteral("secretSauce")).toObject().value(QStringLiteral("lut"))
				   .toArray()
				   .size() == 3,
		   "a save writes it back intact, so the plugin's work is not destroyed");
		ok(!c.unknown.contains(QStringLiteral("props")),
		   "and the fields we DID understand are not duplicated into the unknown bag");
	}

	std::printf("\n-- a colour is one parameter, not three sliders --\n");
	{
		// A colour rides in the same double every other parameter uses. That is
		// what lets one keyframe track, one props map and one persistence path
		// carry it — but only if the packing is EXACT. A double that rounds a
		// channel by one is a colour that drifts every save.
		const QString glsl = QStringLiteral(
			"//@param uColor   color #FF8040 Tint\n"
			"//@param uGhost   color #80102030 Ghost\n"
			"//@param uBroken  color\n"
			"//@param uAmount  float 0.0 1.0 0.5 Amount\n");
		const QVector<ShaderParam> ps = parseShaderParams(glsl);
		ok(ps.size() == 3, "a colour line with no value is dropped, the rest survive");
		ok(ps[0].uniform == QStringLiteral("uColor") &&
			   ps[0].type == ShaderParam::Type::Color,
		   "#RRGGBB parses as a colour");
		ok(ps[0].label == QStringLiteral("Tint"), "and keeps its label");

		const QColor got = QColor::fromRgba(quint32(ps[0].def));
		ok(got.red() == 0xFF && got.green() == 0x80 && got.blue() == 0x40,
		   "the channels come back exactly as written");
		ok(got.alpha() == 0xFF, "an #RRGGBB colour is fully opaque");
		ok(QColor::fromRgba(quint32(ps[1].def)).alpha() == 0x80,
		   "and #AARRGGBB carries its alpha");

		// The round trip that decides whether a saved project reopens the same
		// colour: every channel at both extremes, through the double and back.
		bool lossless = true;
		for (int a = 0; a <= 255; a += 51)
			for (int r = 0; r <= 255; r += 51) {
				const QColor c(r, 255 - r, a, a);
				if (QColor::fromRgba(quint32(double(quint32(c.rgba())))) != c)
					lossless = false;
			}
		ok(lossless, "packed 0xAARRGGBB survives the double round trip on every channel");

		const QString wrapped = wrapShaderToy(QStringLiteral("void mainImage(out vec4 o, in vec2 p){}\n"), ps);
		ok(wrapped.contains(QStringLiteral("uniform vec4 uColor;")),
		   "and reaches the shader as a vec4, not a float");
		ok(wrapped.contains(QStringLiteral("uniform float uAmount;")),
		   "while a float parameter is still a float");
	}

	std::printf("\n-- pasting a component onto a clip --\n");
	{
		// Copy is easy; what "paste" MEANS is the part with a decision in it.
		// Onto a clip without one, it is an add. Onto a clip that already has
		// one, it is an update of that one -- not a second copy, which under
		// stage ordering would silently apply the effect twice and not even
		// look like a duplicate.
		ComponentInstance src = inst("harpia.blur", "src");
		src.props.insert(QStringLiteral("radius"), 0.8);
		src.keys.insert(QStringLiteral("radius"), {PropKey{500, 0.8, TlEase::Linear, 0, 0}});
		src.inMs = 200;
		src.enabled = false;

		QVector<ComponentInstance> fresh;
		ok(pasteComponentInto(fresh, src, QStringLiteral("new1")), "onto an empty clip: added");
		eq(fresh.size(), 1, "one component");
		ok(fresh[0].typeId == src.typeId, "of the copied type");
		ok(fresh[0].instanceId == QStringLiteral("new1"),
		   "under its OWN id — two clips sharing one instance id are two rows the "
		   "Inspector cannot tell apart");
		eq(fresh[0].props.value(QStringLiteral("radius")).toDouble(), 0.8, "with the values");
		ok(fresh[0].keys.contains(QStringLiteral("radius")), "and the keyframes");
		eq(fresh[0].inMs, 200, "and the ramp");
		ok(!fresh[0].enabled, "and even switched off, because that is how it was copied");

		// Now onto a clip that already has one, plus something else to prove
		// the rest of the stack is left alone.
		QVector<ComponentInstance> had{inst("harpia.speed", "s1"), inst("harpia.blur", "b1")};
		had[1].props.insert(QStringLiteral("radius"), 0.1);
		had[0].props.insert(QStringLiteral("factor"), 2.0);
		ok(pasteComponentInto(had, src, QStringLiteral("new2")), "onto a clip that has one");
		eq(had.size(), 2, "no second Blur was added");
		ok(had[1].instanceId == QStringLiteral("b1"),
		   "the one it had keeps its identity: a paste edits this component, it does "
		   "not replace it");
		eq(had[1].props.value(QStringLiteral("radius")).toDouble(), 0.8, "its values came over");
		ok(had[1].keys.contains(QStringLiteral("radius")), "with its keyframes");
		eq(had[0].props.value(QStringLiteral("factor")).toDouble(), 2.0,
		   "and the OTHER component on the clip was not touched");
		ok(had[0].typeId == QStringLiteral("harpia.speed") &&
			   had[1].typeId == QStringLiteral("harpia.blur"),
		   "stack order unchanged");

		// The first of two, not the last: the Inspector's rows are ordinal 0
		// and 1, and "paste" from the panel means the one you would see first.
		QVector<ComponentInstance> two{inst("harpia.blur", "b1"), inst("harpia.blur", "b2")};
		pasteComponentInto(two, src, QStringLiteral("new3"));
		eq(two.size(), 2, "still two");
		eq(two[0].props.value(QStringLiteral("radius")).toDouble(), 0.8, "the first took it");
		ok(!two[1].props.contains(QStringLiteral("radius")), "and the second did not");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
