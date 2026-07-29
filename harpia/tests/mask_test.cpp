// Masking a clip to a shape.
//
// The mask CUTS: outside the shape the clip becomes transparent, so the tracks
// below show through. That is the difference from Inverse Selection, which
// darkens the whole composition, and it is the property that makes
// picture-in-picture and shaped reveals possible at all.
//
// What can go wrong is all in the alpha channel, which no screenshot of a
// single clip would reveal:
//
//   - the outside darkened instead of cleared, so it looks right alone and
//     wrong the moment anything is underneath;
//   - the mask inverted, keeping exactly the part meant to go;
//   - the shape placed in canvas space rather than the clip's, so it slides
//     across the picture when the clip moves;
//   - feather doing nothing, or feathering the wrong side of the edge.
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/component/ComponentStack.hpp"
#include "editor/timeline/TimelineCompositor.hpp"
#include "editor/timeline/TimelineModel.hpp"

#include <QGuiApplication>
#include <QImage>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static QImage solid(QSize sz, QColor c)
{
	QImage i(sz, QImage::Format_RGBA8888);
	i.fill(c);
	return i;
}

// Run just the mask over one frame, with the given properties.
static QImage masked(QSize sz, QColor colour, const QMap<QString, QVariant> &props)
{
	QVector<ComponentInstance> comps;
	ComponentInstance ci;
	ci.typeId = QStringLiteral("harpia.mask");
	ci.instanceId = QStringLiteral("m");
	ci.props = props;
	comps.append(ci);

	ComponentStack stack(comps, ComponentRegistry::instance());
	EvalContext ctx;
	ctx.tMs = 0;
	ctx.durMs = 1000;
	ctx.fps = 30;
	ctx.canvas = sz;
	QImage frame = solid(sz, colour);
	stack.evaluatePixels(ctx, frame);
	return frame;
}

static int alphaAt(const QImage &img, double fx, double fy)
{
	return qAlpha(img.pixel(int(fx * img.width()), int(fy * img.height())));
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);
	registerBuiltinComponents(ComponentRegistry::instance());

	const QSize sz(400, 300);

	std::printf("\n-- it is registered as something you can add --\n");
	{
		const ComponentType *t = ComponentRegistry::instance().find(QStringLiteral("harpia.mask"));
		ok(t != nullptr, "Mask exists");
		if (!t)
			return 1;
		ok(t->addable, "and can be added to a clip");
		ok(t->stage == Stage::Pixel, "as Pixel work");
		ok(t->props.size() == 9, "with shape, centre, size, rotation, corner, feather, invert");
		// A feathered edge reads its neighbours, and the coverage map is built
		// from the whole frame's geometry -- a sub-rect would move the shape.
		ok(!t->pointOp, "and NOT a point op, so it never takes the sub-rect fast path");
	}

	std::printf("\n-- inside is kept, outside is CLEARED not darkened --\n");
	{
		QMap<QString, QVariant> p;
		p[QStringLiteral("shape")] = 2; // circle
		p[QStringLiteral("centreX")] = 0.5;
		p[QStringLiteral("centreY")] = 0.5;
		p[QStringLiteral("width")] = 0.4;
		p[QStringLiteral("feather")] = 0.0;
		const QImage out = masked(sz, QColor(200, 60, 60), p);

		const int mid = alphaAt(out, 0.5, 0.5);
		const int corner = alphaAt(out, 0.04, 0.04);
		std::printf("     alpha in the middle %d, in the corner %d\n", mid, corner);
		ok(mid == 255, "the middle is fully opaque");
		// The assertion that separates a cut from a dim. A darkening mask would
		// leave alpha at 255 and change the colour instead.
		ok(corner == 0, "the corner is fully transparent, not merely dark");

		const QColor c = out.pixelColor(out.width() / 2, out.height() / 2);
		ok(c.red() > 150 && c.green() < 100,
		   "and the kept part still has its own colour, unaltered");
	}

	std::printf("\n-- invert keeps the other side --\n");
	{
		QMap<QString, QVariant> p;
		p[QStringLiteral("shape")] = 2;
		p[QStringLiteral("width")] = 0.4;
		p[QStringLiteral("feather")] = 0.0;
		p[QStringLiteral("invert")] = true;
		const QImage out = masked(sz, QColor(200, 60, 60), p);
		std::printf("     alpha in the middle %d, in the corner %d\n", alphaAt(out, 0.5, 0.5),
			    alphaAt(out, 0.04, 0.04));
		ok(alphaAt(out, 0.5, 0.5) == 0, "the middle is gone");
		ok(alphaAt(out, 0.04, 0.04) == 255, "and the corner survives");
	}

	std::printf("\n-- the shape follows its centre --\n");
	{
		QMap<QString, QVariant> p;
		p[QStringLiteral("shape")] = 2;
		p[QStringLiteral("centreX")] = 0.2;
		p[QStringLiteral("centreY")] = 0.5;
		p[QStringLiteral("width")] = 0.25;
		p[QStringLiteral("feather")] = 0.0;
		const QImage out = masked(sz, QColor(200, 60, 60), p);
		std::printf("     alpha at x=0.2: %d, at x=0.8: %d\n", alphaAt(out, 0.2, 0.5),
			    alphaAt(out, 0.8, 0.5));
		ok(alphaAt(out, 0.2, 0.5) == 255, "opaque where it was put");
		ok(alphaAt(out, 0.8, 0.5) == 0, "and clear on the far side");
	}

	std::printf("\n-- feather softens the edge rather than moving it --\n");
	{
		QMap<QString, QVariant> hard, soft;
		for (QMap<QString, QVariant> *p : {&hard, &soft}) {
			(*p)[QStringLiteral("shape")] = 0; // rectangle: a straight edge to sample
			(*p)[QStringLiteral("centreX")] = 0.5;
			(*p)[QStringLiteral("centreY")] = 0.5;
			(*p)[QStringLiteral("width")] = 0.5;
			(*p)[QStringLiteral("height")] = 0.5;
		}
		hard[QStringLiteral("feather")] = 0.0;
		soft[QStringLiteral("feather")] = 0.25;

		const QImage h = masked(sz, QColor(200, 60, 60), hard);
		const QImage s = masked(sz, QColor(200, 60, 60), soft);

		// The rectangle's left edge sits at x = 0.25. A hard cut is 0 or 255
		// either side of it; a feathered one has values in between.
		int hardMid = 0, softMid = 0;
		for (double x = 0.18; x < 0.33; x += 0.005) {
			const int a = alphaAt(h, x, 0.5);
			if (a > 20 && a < 235)
				++hardMid;
			const int b = alphaAt(s, x, 0.5);
			if (b > 20 && b < 235)
				++softMid;
		}
		std::printf("     partly-transparent samples across the edge: hard %d, soft %d\n",
			    hardMid, softMid);
		ok(softMid > hardMid + 3, "feathering produces a gradient the hard cut does not");
		ok(alphaAt(s, 0.5, 0.5) > 200, "the middle stays essentially opaque");
	}

	std::printf("\n-- and the track below shows through the hole --\n");
	{
		// The whole point, end to end through the real compositor: a masked clip
		// over a second one.
		TimelineModel m;
		TlTrack top;
		top.kind = TlTrack::Kind::Video;
		TlClip a;
		a.type = TlClip::Type::Video;
		a.sourceId = 1;
		a.srcStartMs = 0;
		a.srcEndMs = 2000;
		a.outStartMs = 0;
		ComponentInstance ci;
		ci.typeId = QStringLiteral("harpia.mask");
		ci.instanceId = QStringLiteral("m");
		ci.props[QStringLiteral("shape")] = 2;
		ci.props[QStringLiteral("width")] = 0.4;
		ci.props[QStringLiteral("feather")] = 0.0;
		a.components.append(ci);
		top.clips.append(a);

		TlTrack bottom;
		bottom.kind = TlTrack::Kind::Video;
		TlClip b;
		b.type = TlClip::Type::Video;
		b.sourceId = 2;
		b.srcStartMs = 0;
		b.srcEndMs = 2000;
		b.outStartMs = 0;
		bottom.clips.append(b);

		m.tracks.append(top); // index 0 is the top lane
		m.tracks.append(bottom);

		struct TwoColours : TimelineCompositor::FrameProvider {
			QSize sz;
			QImage frameFor(int id, qint64) override
			{
				return solid(sz, id == 1 ? QColor(230, 40, 40) : QColor(40, 60, 230));
			}
		} provider;
		provider.sz = sz;

		const QImage out = TimelineCompositor::compose(m, 500, sz, provider, nullptr, 30.0);
		const QColor mid = out.pixelColor(out.width() / 2, out.height() / 2);
		const QColor corner = out.pixelColor(8, 8);
		std::printf("     middle rgb(%d,%d,%d), corner rgb(%d,%d,%d)\n", mid.red(), mid.green(),
			    mid.blue(), corner.red(), corner.green(), corner.blue());
		ok(mid.red() > 150 && mid.blue() < 100, "the masked clip shows in the middle");
		ok(corner.blue() > 150 && corner.red() < 100,
		   "and the clip UNDER it shows through the hole");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
