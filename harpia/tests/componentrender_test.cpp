// Components, through the real compositor — the one shared by the preview and
// the exporter.
//
// The claim being tested is the whole reason the runtime is built the way it
// is: a component's output must not depend on WHEN or WHERE it was rendered.
// Two things follow, and both are checked here against the actual
// TimelineCompositor rather than against the runtime in isolation:
//
//   - the same frame composited twice is byte-identical, however many other
//     frames were rendered in between and in whatever order;
//   - a component's effect survives the trip through compose(), which is where
//     a wiring mistake (wrong stage, wrong time base, frame handed over after
//     it was drawn) would show up and the unit tests would not.
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ScriptComponent.hpp"
#include "editor/timeline/TimelineCompositor.hpp"
#include "editor/timeline/TimelineModel.hpp"

#include <QGuiApplication>
#include <QImage>
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

// A source that needs no decoder: a flat colour with a bright square off to one
// side, so a rotation or a move is visible as a change in where the bright part
// lands, and a blur as the square's edge softening.
class TestFrames : public TimelineCompositor::FrameProvider {
public:
	QImage frameFor(int, qint64 srcMs) override
	{
		QImage im(320, 180, QImage::Format_RGBA8888);
		im.fill(QColor(30, 30, 30));
		QPainter p(&im);
		p.fillRect(QRect(20, 20, 60, 60), QColor(240, 240, 240));
		// Encode the source time in a corner patch, so a Time-stage component
		// can be caught changing WHICH frame was fetched, not just how it looks.
		p.fillRect(QRect(300, 0, 20, 20), QColor(int(srcMs % 256), 0, 0));
		p.end();
		lastSrcMs = srcMs;
		return im;
	}
	qint64 lastSrcMs = -1;
};

static TimelineModel oneClip(const QVector<ComponentInstance> &comps)
{
	TimelineModel m;
	TlTrack t;
	t.kind = TlTrack::Kind::Video;
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = 1;
	c.srcStartMs = 0;
	c.srcEndMs = 5000;
	c.outStartMs = 0;
	c.components = comps;
	t.clips.append(c);
	m.tracks.append(t);
	return m;
}

static ComponentInstance comp(const char *type, const char *key, double v)
{
	ComponentInstance c;
	c.typeId = QString::fromLatin1(type);
	c.instanceId = QStringLiteral("i");
	if (key)
		c.props.insert(QString::fromLatin1(key), v);
	return c;
}

static QImage render(const TimelineModel &m, qint64 outMs, TestFrames *fp)
{
	return TimelineCompositor::compose(m, outMs, QSize(320, 180), *fp, nullptr, 30.0);
}

static double meanLuma(const QImage &f)
{
	double s = 0;
	int n = 0;
	for (int y = 0; y < f.height(); y += 2)
		for (int x = 0; x < f.width(); x += 2) {
			s += QColor(f.pixel(x, y)).lightness();
			++n;
		}
	return n ? s / n : -1;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);
	registerBuiltinComponents(ComponentRegistry::instance());

	TestFrames fp;

	std::printf("\n-- a clip with no components is untouched --\n");
	const QImage plain = render(oneClip({}), 1000, &fp);
	ok(!plain.isNull(), "it renders");

	std::printf("\n-- Always Rotate reaches the picture --\n");
	{
		const QImage r = render(oneClip({comp("harpia.alwaysRotate", "degreesPerSecond", 90.0)}),
					1000, &fp);
		ok(r != plain, "the frame differs from the un-rotated one");
		// Rotating a landscape frame inside a landscape canvas leaves corners
		// empty, so the mean drops. A rotation that never happened would not.
		std::printf("     plain %.1f   rotated %.1f\n", meanLuma(plain), meanLuma(r));
		ok(meanLuma(r) < meanLuma(plain) - 2.0, "and it is darker, as a rotation should be");
	}

	std::printf("\n-- Blur reaches the picture, and only the clip --\n");
	{
		const QImage b = render(oneClip({comp("harpia.blur", "radius", 1.0)}), 1000, &fp);
		ok(b != plain, "the frame differs");
		// The bright square's edge should have softened: sample just outside it.
		const int edgePlain = QColor(plain.pixel(85, 50)).lightness();
		const int edgeBlur = QColor(b.pixel(85, 50)).lightness();
		std::printf("     just outside the square: plain %d, blurred %d\n", edgePlain, edgeBlur);
		ok(edgeBlur > edgePlain + 5, "light has bled outside the square");
	}

	std::printf("\n-- Speed changes WHICH frame is fetched --\n");
	{
		fp.lastSrcMs = -1;
		render(oneClip({}), 1000, &fp);
		const qint64 at1x = fp.lastSrcMs;
		fp.lastSrcMs = -1;
		render(oneClip({comp("harpia.speed", "factor", 2.0)}), 1000, &fp);
		const qint64 at2x = fp.lastSrcMs;
		std::printf("     1x asked for %lldms, 2x asked for %lldms\n", (long long)at1x,
			    (long long)at2x);
		ok(at1x == 1000, "at 1x, one second in is one second of source");
		ok(at2x == 2000, "at 2x it is two — the Time stage ran before the decode");
	}

	std::printf("\n-- disabled means disabled, all the way through --\n");
	{
		ComponentInstance c = comp("harpia.alwaysRotate", "degreesPerSecond", 90.0);
		c.enabled = false;
		ok(render(oneClip({c}), 1000, &fp) == plain, "the frame is exactly the plain one");
	}

	std::printf("\n-- THE claim: the same instant renders identically, always --\n");
	{
		// Everything at once, including a Time component, so the check covers
		// the decode as well as the pose and the pixels.
		const TimelineModel m = oneClip({comp("harpia.speed", "factor", 1.7),
						 comp("harpia.alwaysRotate", "degreesPerSecond", 45.0),
						 comp("harpia.blur", "radius", 0.4)});
		const QImage cold = render(m, 2345, &fp);

		// Now render a hundred other frames, forwards and backwards, and come
		// back. If anything in the chain kept state, this is where it shows.
		for (qint64 t = 0; t < 5000; t += 97)
			render(m, t, &fp);
		for (qint64 t = 4900; t > 0; t -= 61)
			render(m, t, &fp);
		const QImage again = render(m, 2345, &fp);

		ok(again == cold, "byte-identical after 130 other frames in both directions");
		if (again != cold) {
			int diff = 0;
			for (int y = 0; y < cold.height(); ++y)
				for (int x = 0; x < cold.width(); ++x)
					if (cold.pixel(x, y) != again.pixel(x, y))
						++diff;
			std::printf("     %d pixels differ\n", diff);
		}
	}

	std::printf("\n-- and that holds for a user's component too --\n");
	{
		if (ScriptComponents::available()) {
			QString err;
			ScriptComponents::loadSource(
				QStringLiteral("//@component t.wobble\n//@stage Transform\n"
					       "//@param amt float 0 1 0.3 Amount\n"
					       "function evaluate(ctx, io){\n"
					       "  io.x += ctx.p.amt * Math.sin(ctx.t * 5) * 0.1;\n"
					       "  io.rotation += 20 * Math.cos(ctx.t * 3);\n"
					       "}"),
				ComponentRegistry::instance(), &err);
			if (!err.isEmpty())
				std::printf("     load error: %s\n", qPrintable(err));
			const TimelineModel m = oneClip({comp("t.wobble", "amt", 0.8)});
			const QImage cold = render(m, 1234, &fp);
			ok(cold != plain, "the scripted component changes the frame");
			for (qint64 t = 0; t < 3000; t += 53)
				render(m, t, &fp);
			ok(render(m, 1234, &fp) == cold,
			   "and the same instant still renders byte-identically");
		} else {
			std::printf("  SKIP no scripting engine in this build\n");
		}
	}

	std::printf("\n-- a missing component does not stop the frame --\n");
	{
		ComponentInstance missing;
		missing.typeId = QStringLiteral("nobody.hasthis");
		missing.instanceId = QStringLiteral("m");
		const QImage r = render(oneClip({missing, comp("harpia.alwaysRotate",
							       "degreesPerSecond", 90.0)}),
					1000, &fp);
		ok(!r.isNull() && r != plain,
		   "the rest of the clip still renders around the one that is absent");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
