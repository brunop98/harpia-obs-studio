// Opaque frames are tagged RGBX8888 (FrameSeeker::toImage, the still loaders)
// so QPainter takes its fast blit instead of the per-pixel path a straight-
// alpha source forces. The claim that makes that safe: a frame whose fourth
// byte is 255 everywhere composites to the SAME BYTES whichever way it is
// tagged -- placed 1:1, scaled up or down, rotated, faded, cropped, and drawn
// into a transparent transition layer -- and a component that writes alpha (a
// mask) still cuts an opaque-tagged frame, because the compositor retags it
// before the components run.
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/timeline/TimelineCompositor.hpp"
#include "editor/timeline/TimelineModel.hpp"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// A noisy picture: every pixel different, so a wrong offset or a wrong channel
// order anywhere shows up in the byte comparison.
static QImage noisy(int w, int h, QImage::Format fmt)
{
	QImage im(w, h, fmt);
	for (int y = 0; y < h; ++y) {
		uchar *r = im.scanLine(y);
		for (int x = 0; x < w; ++x) {
			r[x * 4 + 0] = uchar((x * 7 + y * 3) & 255);
			r[x * 4 + 1] = uchar((x ^ y) & 255);
			r[x * 4 + 2] = uchar((x * 13 + y * 5) & 255);
			r[x * 4 + 3] = 255;
		}
	}
	return im;
}

class Frames : public TimelineCompositor::FrameProvider {
public:
	QImage img;
	QImage frameFor(int, qint64) override { return img; }
	QSize sourceSize(int) override { return img.size(); }
};

static bool sameBytes(const QImage &a, const QImage &b)
{
	if (a.size() != b.size() || a.format() != b.format())
		return false;
	for (int y = 0; y < a.height(); ++y)
		if (std::memcmp(a.constScanLine(y), b.constScanLine(y), size_t(a.width()) * 4))
			return false;
	return true;
}

static TimelineModel model(const TlClip &c)
{
	TimelineModel m;
	TlTrack t;
	t.kind = TlTrack::Kind::Video;
	t.clips.append(c);
	m.tracks.append(t);
	return m;
}

static TlClip clip()
{
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = 1;
	c.srcStartMs = 0;
	c.srcEndMs = 5000;
	c.outStartMs = 0;
	return c;
}

// The same composite with the frame tagged both ways.
static bool identical(const TimelineModel &m, const QImage &frame, QSize canvas, qint64 ms = 500)
{
	Frames a;
	a.img = frame; // RGBA8888, alpha 255
	Frames b;
	b.img = frame;
	b.img.reinterpretAsFormat(QImage::Format_RGBX8888);
	const QImage ra = TimelineCompositor::compose(m, ms, canvas, a, nullptr, 30.0);
	const QImage rb = TimelineCompositor::compose(m, ms, canvas, b, nullptr, 30.0);
	const bool same = !ra.isNull() && sameBytes(ra, rb);
	if (!same && !ra.isNull() && ra.size() == rb.size()) {
		int worst = 0;
		for (int y = 0; y < ra.height(); ++y) {
			const uchar *p = ra.constScanLine(y), *q = rb.constScanLine(y);
			for (int i = 0; i < ra.width() * 4; ++i)
				worst = std::max(worst, std::abs(int(p[i]) - int(q[i])));
		}
		std::printf("     (largest byte difference: %d)\n", worst);
	}
	return same;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);
	registerBuiltinComponents(ComponentRegistry::instance());

	const QSize canvas(320, 180);
	const QImage same = noisy(320, 180, QImage::Format_RGBA8888);
	const QImage bigger = noisy(480, 320, QImage::Format_RGBA8888);
	const QImage smaller = noisy(200, 90, QImage::Format_RGBA8888);

	std::printf("\n-- an opaque frame composites to the same bytes however it is tagged --\n");
	{
		ok(identical(model(clip()), same, canvas), "placed 1:1 at the canvas size");
		ok(identical(model(clip()), bigger, canvas), "fitted down (smooth scaling)");
		ok(identical(model(clip()), smaller, canvas), "fitted up");
		TlClip z = clip();
		z.scale = 1.7;
		z.posX = 0.4;
		ok(identical(model(z), same, canvas), "zoomed in and moved");
		TlClip r = clip();
		r.rotation = 27.0;
		ok(identical(model(r), same, canvas), "rotated");
		TlClip f = clip();
		f.opacity = 0.5;
		ok(identical(model(f), same, canvas), "half transparent (the fast blit no longer applies)");
		TlClip cr = clip();
		cr.crop = QRect(40, 20, 200, 120);
		ok(identical(model(cr), same, canvas), "cropped");
		TlClip s = clip();
		s.scale = 0.5;
		ok(identical(model(s), same, canvas), "at half size over the black canvas");
	}

	std::printf("\n-- and drawn into a transparent transition layer --\n");
	{
		TimelineModel m;
		TlTrack t;
		t.kind = TlTrack::Kind::Video;
		TlClip a = clip();
		a.srcEndMs = 1000;
		TlClip b = clip();
		b.outStartMs = 600;
		b.transition.enabled = true;
		t.clips.append(a);
		t.clips.append(b);
		m.tracks.append(t);
		ok(identical(m, same, canvas, 800), "mid-transition, both layers");
	}

	std::printf("\n-- a component that writes alpha still cuts an opaque-tagged frame --\n");
	{
		QImage white(320, 180, QImage::Format_RGBX8888);
		white.fill(QColor(255, 255, 255));
		Frames fp;
		fp.img = white;
		TlClip c = clip();
		ComponentInstance mask;
		mask.typeId = QStringLiteral("harpia.mask");
		mask.instanceId = QStringLiteral("m");
		mask.props.insert(QStringLiteral("width"), 0.5);
		mask.props.insert(QStringLiteral("height"), 0.5);
		mask.props.insert(QStringLiteral("feather"), 0.0);
		c.components.append(mask);
		const QImage out = TimelineCompositor::compose(model(c), 500, canvas, fp, nullptr, 30.0);
		const QColor centre = out.pixelColor(160, 90), corner = out.pixelColor(4, 4);
		std::printf("     centre %d,%d,%d  corner %d,%d,%d\n", centre.red(), centre.green(), centre.blue(),
			    corner.red(), corner.green(), corner.blue());
		ok(centre.red() > 250, "inside the mask: the clip");
		ok(corner.red() < 5, "outside it: cut away (the canvas shows through)");
	}

	std::printf("\n-- what the tag buys --\n");
	{
		const QImage hd = noisy(1280, 720, QImage::Format_RGBA8888);
		Frames a;
		a.img = hd;
		Frames b;
		b.img = hd;
		b.img.reinterpretAsFormat(QImage::Format_RGBX8888);
		const TimelineModel m = model(clip());
		const QSize cv(1280, 720);
		QElapsedTimer t;
		t.start();
		for (int i = 0; i < 20; ++i)
			TimelineCompositor::compose(m, 500, cv, a, nullptr, 30.0);
		const double msA = t.nsecsElapsed() / 20e6;
		t.start();
		for (int i = 0; i < 20; ++i)
			TimelineCompositor::compose(m, 500, cv, b, nullptr, 30.0);
		const double msB = t.nsecsElapsed() / 20e6;
		std::printf("     720p compose: RGBA8888 %.2f ms, RGBX8888 %.2f ms a frame\n", msA, msB);
		ok(msB <= msA * 1.05, "the opaque tag is never slower");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
