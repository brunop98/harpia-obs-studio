// Multi-Cut with mixed resolutions: adding a clip must not re-shape the project.
//
// Multi-Cut assembles cuts from several files into ONE output, and that output
// has one resolution -- the primary source's. The exporter has always done the
// right thing: runVideoCutsMulti() takes its canvas from inputs[0] and
// fit-scales every other source into it.
//
// The PREVIEW did not. It sized itself to whichever source was selected, so
// adding a 4:3 file to a 16:9 project re-shaped the canvas and then drew the
// 16:9 frames stretched into it. The picture on screen stopped describing the
// file that would come out, which is the only thing a preview is for.
//
// Two claims here, and the second is the one that was broken:
//   1. the fit arithmetic matches what the encoder does (aspect kept, centred);
//   2. adding a source does not change the project's canvas, and a frame from
//      the odd-shaped one comes back letterboxed rather than stretched.
#include "editor/CanvasFit.hpp"

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

// The exporter's own arithmetic, transcribed from runVideoCutsMulti, so the two
// are compared rather than one of them being trusted.
static QSize exporterFit(QSize src, QSize canvas)
{
	const double sc = std::min(double(canvas.width()) / src.width(),
				   double(canvas.height()) / src.height());
	return QSize(std::max(2, int(src.width() * sc)), std::max(2, int(src.height() * sc)));
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);

	const QSize canvas(1920, 1080);

	std::printf("\n-- the fit keeps the shape and centres it --\n");
	{
		// A 4:3 source in a 16:9 canvas: full height, bars left and right.
		const QRect r = fitRectInCanvas(QSize(1440, 1080), canvas);
		std::printf("     4:3 in 16:9 -> %dx%d at (%d,%d)\n", r.width(), r.height(), r.x(),
			    r.y());
		ok(r.height() == 1080, "it uses the full height");
		ok(r.width() == 1440, "and keeps its own width, not the canvas's");
		ok(r.x() == 240 && r.y() == 0, "centred, so the bars are even");

		// Taller than the canvas: full width, bars top and bottom.
		const QRect t = fitRectInCanvas(QSize(1080, 1920), canvas);
		std::printf("     9:16 in 16:9 -> %dx%d at (%d,%d)\n", t.width(), t.height(), t.x(),
			    t.y());
		ok(t.height() == 1080 && t.width() == 607, "a portrait source fits by height");
		ok(t.y() == 0 && t.x() == 656, "and is centred horizontally");

		// Same shape: fills it exactly, whatever the pixel size.
		ok(fillsCanvas(QSize(1280, 720), canvas), "a same-shaped source fills the canvas");
		ok(fillsCanvas(canvas, canvas), "as does an identical one");
		ok(!fillsCanvas(QSize(1440, 1080), canvas), "and a differently-shaped one does not");
	}

	std::printf("\n-- and it matches what the exporter does --\n");
	{
		// The preview being "about right" is not enough: it has to be the same
		// scale the encoder picks, or the framing you approve is not the
		// framing you get.
		bool allMatch = true;
		for (QSize src : {QSize(1440, 1080), QSize(1080, 1920), QSize(640, 480),
				  QSize(3840, 2160), QSize(1920, 1080), QSize(720, 1280)}) {
			const QSize mine = fitRectInCanvas(src, canvas).size();
			const QSize theirs = exporterFit(src, canvas);
			// Within a pixel: the exporter also rounds down to even numbers
			// for the encoder, which this does not need to do.
			if (std::abs(mine.width() - theirs.width()) > 1 ||
			    std::abs(mine.height() - theirs.height()) > 1) {
				allMatch = false;
				std::printf("     MISMATCH %dx%d: preview %dx%d vs export %dx%d\n",
					    src.width(), src.height(), mine.width(), mine.height(),
					    theirs.width(), theirs.height());
			}
		}
		ok(allMatch, "every source fits to the size the encoder would choose");
	}

	std::printf("\n-- a frame from an odd-shaped source comes back letterboxed --\n");
	{
		// The bug, at the pixel level. A 4:3 frame handed to a 16:9 canvas used
		// to be stretched across it; now it keeps its shape with black bars.
		QImage src(640, 480, QImage::Format_RGBA8888);
		src.fill(QColor(200, 60, 60));
		const QImage out = fitIntoCanvas(src, canvas);
		std::printf("     640x480 -> %dx%d\n", out.width(), out.height());
		ok(out.size() == canvas, "the result is canvas-sized");
		// Middle is the picture; the far left and right are bars.
		ok(out.pixelColor(960, 540).red() > 150, "the picture is in the middle");
		ok(out.pixelColor(4, 540).red() < 40, "with a bar on the left");
		ok(out.pixelColor(1915, 540).red() < 40, "and one on the right");
		ok(out.pixelColor(960, 4).red() > 150, "and no bar top or bottom, since it fits by height");
	}
	{
		// A same-shaped frame is passed straight through -- the ordinary
		// single-resolution project must not pay for a copy, or for a
		// resample that would soften it.
		QImage src(1280, 720, QImage::Format_RGBA8888);
		src.fill(QColor(10, 200, 10));
		const QImage out = fitIntoCanvas(src, canvas);
		std::printf("     same-shaped 1280x720 -> %dx%d (unchanged: %s)\n", out.width(),
			    out.height(), out.size() == src.size() ? "yes" : "no");
		ok(out.size() == src.size(), "a same-shaped frame is returned untouched");
	}
	{
		// The preview decodes at a REDUCED size, so "already fits" has to be an
		// aspect test rather than a pixel-size one. A 16:9 frame decoded at
		// 1280x720 for a 1920x1080 canvas is the same shape and must not be
		// blown up into a canvas-sized image on every frame.
		QImage small(854, 480, QImage::Format_RGBA8888);
		small.fill(Qt::white);
		ok(fitIntoCanvas(small, canvas).size() == small.size(),
		   "a reduced-size decode of the right shape is left alone");
	}

	std::printf("\n-- degenerate inputs --\n");
	{
		ok(fitRectInCanvas(QSize(0, 0), canvas).isEmpty(), "a source with no area has no rect");
		ok(fitRectInCanvas(QSize(640, 480), QSize()).isEmpty(), "nor does an empty canvas");
		QImage img(10, 10, QImage::Format_RGBA8888);
		ok(fitIntoCanvas(img, QSize()).size() == img.size(),
		   "and an empty canvas returns the frame unchanged rather than nothing");
		ok(fitIntoCanvas(QImage(), canvas).isNull(), "a null frame stays null");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
