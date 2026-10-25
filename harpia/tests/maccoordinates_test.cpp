// A Mac desktop is laid out in points, while each display captures its own
// pixel scale. Exercise crops at mixed-DPI seams and negative desktop origins.
#include "platform/mac/MacCoordinates.hpp"

#include <cstdio>

using harpia::mac_platform::physicalRegion;

static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

int main()
{
	const QRect retina(0, 0, 1440, 900);
	const QSize retinaPixels(2880, 1800);
	const QRect external(-1920, 0, 1920, 1080);
	check(physicalRegion(QRect(100, 50, 400, 200), retina, retinaPixels) == QRect(200, 100, 800, 400),
	      "Retina window points become display-local pixels");
	check(physicalRegion(QRect(-1800, 10, 800, 600), external, external.size()) == QRect(120, 10, 800, 600),
	      "an external display's negative origin is subtracted before scaling");
	const QRect straddling(-100, 40, 200, 100);
	check(physicalRegion(straddling, retina, retinaPixels) == QRect(0, 80, 200, 200),
	      "the Retina half of a window crossing a mixed-DPI seam is clipped first");
	check(physicalRegion(straddling, external, external.size()) == QRect(1820, 40, 100, 100),
	      "the same window uses the external display's independent 1x scale");
	check(physicalRegion(QRect(-2559, -1439, 11, 13), QRect(-2560, -1440, 2560, 1440), QSize(3840, 2160)) ==
		QRect(1, 1, 16, 20), "fractional scaling expands bounds then makes encoder-safe even dimensions");
	check(physicalRegion(retina, retina, retinaPixels) == QRect(0, 0, 2880, 1800),
	      "a full-display window preserves every capture pixel");
	check(physicalRegion(QRect(25, 50, 100, 100), QRect(0, 0, 1000, 800), QSize(1200, 1600)) ==
		QRect(30, 100, 120, 200), "independent horizontal and vertical capture ratios are respected");
	check(physicalRegion(QRect(10, 10, 17, 17), external.translated(1920, 0), external.size()) ==
		QRect(10, 10, 16, 16), "odd dimensions are shortened without moving the crop origin");
	check(physicalRegion(QRect(10, 10, 9, 9), retina, retinaPixels) == QRect(20, 20, 18, 18),
	      "minimum usable size is measured in capture pixels rather than desktop points");
	check(!physicalRegion(QRect(1435, 0, 10, 100), retina, retinaPixels),
	      "a clipped sliver smaller than sixteen capture pixels is rejected");
	check(!physicalRegion(QRect(1500, 0, 100, 100), retina, retinaPixels),
	      "a window outside the chosen display cannot create a crop");
	check(!physicalRegion(QRect(0, 0, 100, 100), QRect(), retinaPixels) &&
	      !physicalRegion(QRect(0, 0, 100, 100), retina, QSize()),
	      "missing display bounds or capture dimensions fail without division by zero");
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
