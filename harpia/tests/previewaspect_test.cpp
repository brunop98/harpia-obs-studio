// Changing the project resolution must never stretch the picture.
//
// Setting a project to 720x1280 while trimming a 16:9 clip drew the clip
// squeezed into the tall frame. Three separate things allowed that, and each is
// checked here on its own:
//
//   1. applyProjectFormat told the preview widget its shape was the PROJECT
//      canvas whatever the mode -- but Simple Trim writes the source at its own
//      size and Multi-Cut takes its shape from the primary clip, so the widget
//      was claiming a frame the export would never produce;
//   2. the widget then painted the frame across that whole rectangle, which
//      turns any disagreement into a distortion rather than a bar;
//   3. the reduced render size clamped width and height against separate
//      minimums, which changes the aspect ratio by itself on a tall canvas.
//
// The third is the sneakiest: it needs no mode confusion at all, just a narrow
// preview panel and a portrait project.
#include "editor/CanvasFit.hpp"
#include "editor/EditorWidgets.hpp"

#include <QApplication>
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

// Aspect ratios equal to within a rounding pixel or so.
static bool sameShape(QSize a, QSize b)
{
	if (a.height() <= 0 || b.height() <= 0)
		return false;
	const double ra = double(a.width()) / a.height();
	const double rb = double(b.width()) / b.height();
	return std::abs(ra - rb) <= 0.02 * rb;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- a reduced render size keeps the project's shape --\n");
	{
		// The ordinary case first: a 4K project shown in an 800px panel.
		const QSize a = reducedRenderSize(QSize(3840, 2160), 800);
		std::printf("     3840x2160 into 800 wide -> %dx%d\n", a.width(), a.height());
		ok(sameShape(a, QSize(3840, 2160)), "16:9 stays 16:9");
		ok(a.width() <= 800, "and is no wider than asked for");

		// The one that was broken. 720x1280 asked to fit a narrow panel used to
		// clamp to 160x256 -- taller in proportion than the project, i.e. the
		// picture stretched vertically to fill it.
		const QSize v = reducedRenderSize(QSize(720, 1280), 130);
		std::printf("     720x1280 into 130 wide -> %dx%d (project ratio %.4f, this %.4f)\n",
			    v.width(), v.height(), 720.0 / 1280.0, double(v.width()) / v.height());
		ok(sameShape(v, QSize(720, 1280)), "9:16 stays 9:16 even at the floor");
		ok(v.width() >= 160 || v.height() >= 90, "and the floor is still honoured");

		// A sweep, because the failure was at one particular corner of the
		// input space and a single case would not have caught it.
		bool allKeepShape = true;
		for (QSize canvas : {QSize(720, 1280), QSize(1080, 1920), QSize(1920, 1080),
				     QSize(3840, 2160), QSize(1080, 1080), QSize(2560, 1080)})
			for (int want : {40, 90, 130, 200, 400, 900, 4000})
				if (!sameShape(reducedRenderSize(canvas, want), canvas)) {
					allKeepShape = false;
					const QSize g = reducedRenderSize(canvas, want);
					std::printf("     BROKE %dx%d at want=%d -> %dx%d\n",
						    canvas.width(), canvas.height(), want, g.width(),
						    g.height());
				}
		ok(allKeepShape, "every canvas keeps its shape at every panel width");

		ok(reducedRenderSize(QSize(1920, 1080), 4000) == QSize(1920, 1080),
		   "a panel wider than the project renders at the project size");
		ok(reducedRenderSize(QSize(), 400).isEmpty(), "an empty canvas stays empty");
	}

	std::printf("\n-- CONTROL: the old arithmetic really did distort --\n");
	{
		// Without this the section above proves nothing: it has to be shown that
		// the inputs chosen can tell the two apart.
		const QSize canvas(720, 1280);
		const int wantW = 130;
		const double k = std::max(0.2, double(wantW) / canvas.width());
		const QSize old(std::max(160, int(std::lround(canvas.width() * k))),
				std::max(90, int(std::lround(canvas.height() * k))));
		std::printf("     old: %dx%d (ratio %.4f)   new: %dx%d\n", old.width(), old.height(),
			    double(old.width()) / old.height(), reducedRenderSize(canvas, wantW).width(),
			    reducedRenderSize(canvas, wantW).height());
		ok(!sameShape(old, canvas), "CONTROL: the previous clamp changed the aspect here");
	}

	std::printf("\n-- and the widget letterboxes rather than stretches --\n");
	{
		// The real widget, painted for real. A 16:9 frame handed to a canvas the
		// project has just made 9:16: the picture must sit in a band across the
		// middle with bars above and below, not fill the frame.
		PreviewCanvas c;
		c.resize(400, 600);
		c.setVideoSize(720, 1280); // the project went vertical
		QImage frame(1920, 1080, QImage::Format_RGBA8888);
		frame.fill(QColor(220, 40, 40)); // unmistakably not the background
		c.setFrame(frame);

		QImage shot(c.size(), QImage::Format_RGBA8888);
		shot.fill(Qt::black);
		c.render(&shot);

		// Count how tall and how wide the red region is.
		const int cx = shot.width() / 2, cy = shot.height() / 2;
		auto isPicture = [&](int x, int y) { return shot.pixelColor(x, y).red() > 120; };
		int top = cy, bottom = cy, left = cx, right = cx;
		while (top > 0 && isPicture(cx, top - 1))
			--top;
		while (bottom < shot.height() - 1 && isPicture(cx, bottom + 1))
			++bottom;
		while (left > 0 && isPicture(left - 1, cy))
			--left;
		while (right < shot.width() - 1 && isPicture(right + 1, cy))
			++right;
		const QSize drawn(right - left + 1, bottom - top + 1);
		std::printf("     widget 400x600, canvas 9:16, frame 16:9 -> picture drawn %dx%d\n",
			    drawn.width(), drawn.height());

		ok(isPicture(cx, cy), "the picture is there at all");
		ok(sameShape(drawn, QSize(16, 9)), "and is drawn at the FRAME's 16:9, not squeezed");
		// The canvas rect inside a 400x600 widget at 9:16 is 337x600; a 16:9
		// picture fitted into it is 337x190, so there is a lot of bar.
		ok(drawn.height() < 300, "with bars above and below");
	}

	std::printf("\n-- the ordinary case is untouched --\n");
	{
		// Same shape both sides: the frame fills the canvas exactly, as before.
		PreviewCanvas c;
		c.resize(400, 300);
		c.setVideoSize(1920, 1080);
		QImage frame(1280, 720, QImage::Format_RGBA8888);
		frame.fill(QColor(40, 200, 40));
		c.setFrame(frame);

		QImage shot(c.size(), QImage::Format_RGBA8888);
		shot.fill(Qt::black);
		c.render(&shot);

		const int cy = shot.height() / 2;
		auto isPicture = [&](int x, int y) { return shot.pixelColor(x, y).green() > 120; };
		int left = 0, right = shot.width() - 1;
		while (left < shot.width() - 1 && !isPicture(left, cy))
			++left;
		while (right > 0 && !isPicture(right, cy))
			--right;
		std::printf("     widget 400x300, both 16:9 -> picture spans x %d..%d of %d\n", left,
			    right, shot.width());
		ok(left == 0 && right == shot.width() - 1,
		   "a same-shaped frame still fills the canvas edge to edge");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
