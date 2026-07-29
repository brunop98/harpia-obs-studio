// An effect clip grades only inside its own transform area.
//
// An effect clip has a pose like any other clip. It used to be ignored: the
// grade covered the whole frame however the clip was placed, so the position
// and zoom controls in its Inspector were dead. Now the pose bounds where the
// effect lands, which is what makes a vignette-on-one-corner or a
// blur-this-region possible at all.
//
// The things that can go wrong here are all pixel facts, invisible from the
// data model:
//
//   - the pose ignored, so the whole frame still grades (the old bug);
//   - the area applied but INVERTED, grading everything except it;
//   - the area right but offset, because the rect was built from the top-left
//     instead of the centre;
//   - a clip nobody moved no longer covering the frame, which would silently
//     change every project that already has one.
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"
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

// A flat grey source, so any change in brightness is the effect and nothing
// else. Returned through a provider because that is what the compositor takes.
static QImage greyFrame(QSize sz)
{
	QImage img(sz, QImage::Format_RGBA8888);
	img.fill(QColor(128, 128, 128));
	return img;
}

static int lum(const QImage &img, double fx, double fy)
{
	const QColor c = img.pixelColor(int(fx * img.width()), int(fy * img.height()));
	return c.red();
}

// One video clip covering the frame, with an effect clip above it.
static TimelineModel build(const TlTransform &effectPose)
{
	TimelineModel m;

	TlTrack fxTrack;
	fxTrack.kind = TlTrack::Kind::Effect;
	TlClip e;
	e.type = TlClip::Type::Effect;
	e.outStartMs = 0;
	e.srcStartMs = 0;
	e.srcEndMs = 2000;
	e.posX = effectPose.posX;
	e.posY = effectPose.posY;
	e.scale = effectPose.scale;
	e.rotation = effectPose.rotation;
	e.opacity = effectPose.opacity;
	// Brightness, turned all the way up: the graded part must be obviously
	// brighter than the grey underneath, not subtly so.
	ComponentInstance ci;
	ci.typeId = effectComponentId(FxType::Brightness);
	ci.instanceId = QStringLiteral("b");
	ci.props.insert(QStringLiteral("amount"), 0.9);
	e.components.append(ci);
	fxTrack.clips.append(e);

	TlTrack vid;
	vid.kind = TlTrack::Kind::Video;
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = 1;
	c.srcStartMs = 0;
	c.srcEndMs = 2000;
	c.outStartMs = 0;
	vid.clips.append(c);

	m.tracks.append(fxTrack); // index 0 is the top lane
	m.tracks.append(vid);
	return m;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);
	registerBuiltinComponents(ComponentRegistry::instance());

	const QSize canvas(320, 180);
	struct Grey : TimelineCompositor::FrameProvider {
		QSize sz;
		QImage frameFor(int, qint64) override { return greyFrame(sz); }
	} provider;
	provider.sz = canvas;

	std::printf("\n-- a clip nobody moved still grades everything --\n");
	int wholeCentre = 0, wholeCorner = 0;
	{
		TlTransform def; // centred, scale 1, no rotation
		const QImage out =
			TimelineCompositor::compose(build(def), 500, canvas, provider, nullptr, 30.0);
		wholeCentre = lum(out, 0.5, 0.5);
		wholeCorner = lum(out, 0.08, 0.08);
		std::printf("     centre %d  corner %d  (grey was 128)\n", wholeCentre, wholeCorner);
		ok(wholeCentre > 150, "the middle is brighter than the grey under it");
		// Every project that already has an effect clip depends on this.
		ok(wholeCorner > 150, "and so is the far corner — the default is the whole canvas");
	}

	std::printf("\n-- shrink it and only that part grades --\n");
	{
		TlTransform tf;
		tf.scale = 0.4; // a 40% box in the middle
		const QImage out =
			TimelineCompositor::compose(build(tf), 500, canvas, provider, nullptr, 30.0);
		const int centre = lum(out, 0.5, 0.5);
		const int corner = lum(out, 0.08, 0.08);
		std::printf("     centre %d  corner %d\n", centre, corner);
		ok(centre > 150, "inside the box is graded");
		// The assertion that separates "bounded" from "the pose is ignored".
		ok(corner < 140, "outside it is the untouched grey");
		ok(centre > corner + 30, "so it is a region, not the whole frame");
	}

	std::printf("\n-- move it and the graded part moves with it --\n");
	{
		TlTransform tf;
		tf.scale = 0.4;
		tf.posX = 0.2; // over to the left
		tf.posY = 0.5;
		const QImage out =
			TimelineCompositor::compose(build(tf), 500, canvas, provider, nullptr, 30.0);
		const int left = lum(out, 0.2, 0.5);
		const int right = lum(out, 0.8, 0.5);
		std::printf("     left %d  right %d\n", left, right);
		ok(left > 150, "the left is graded, where the clip was put");
		ok(right < 140, "and the right is not");
		// Built from the top-left instead of the centre, the box would land at
		// 0.2+0.2 rather than around 0.2, and this is what would catch it.
		ok(lum(out, 0.5, 0.5) < 140, "the middle is untouched, so it is centred on the pose");
	}

	std::printf("\n-- and the area is not inverted --\n");
	{
		// The single most likely wrong turn: clipping to everything EXCEPT the
		// path. Both of the checks above would still pass on a symmetric layout
		// if the sense were flipped and the numbers happened to line up; a box
		// in one corner cannot be confused with its complement.
		TlTransform tf;
		tf.scale = 0.3;
		tf.posX = 0.15;
		tf.posY = 0.15;
		const QImage out =
			TimelineCompositor::compose(build(tf), 500, canvas, provider, nullptr, 30.0);
		const int inBox = lum(out, 0.15, 0.15);
		const int outside = lum(out, 0.85, 0.85);
		std::printf("     in the box %d  the rest %d\n", inBox, outside);
		ok(inBox > outside + 30, "the box is the bright part, not the hole in it");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
