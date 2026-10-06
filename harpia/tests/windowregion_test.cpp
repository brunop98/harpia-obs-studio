// Setting the recording area from a window: the pure half.
//
//   * The window under the pointer is the TOPMOST one there, never one of ours
//     (the picker's own overlay covers the whole screen).
//   * A window straddling two monitors belongs to the one holding most of it.
//   * Its area: clipped to that monitor, relative to the monitor's corner,
//     even width and height; nothing when too little of it is on screen.
#include "platform/WindowList.hpp"

#include <QCoreApplication>

#include <cstdio>

using namespace harpia;
using namespace harpia::window_region;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static DesktopWindow win(quintptr id, QRect r, bool ours = false)
{
	DesktopWindow w;
	w.id = id;
	w.bounds = r;
	w.ours = ours;
	return w;
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);

	std::printf("\n-- which window is under the pointer --\n");
	{
		const QVector<DesktopWindow> z = {
			win(1, QRect(0, 0, 1920, 1080), true), // our full-screen picker overlay
			win(2, QRect(100, 100, 400, 300)),     // a small window on top
			win(3, QRect(50, 50, 1000, 700)),      // a big one behind it
		};
		ok(windowAt(z, QPoint(200, 200)) == 1, "the topmost window there, not our overlay");
		ok(windowAt(z, QPoint(800, 600)) == 2, "the one behind where the top one is not");
		ok(windowAt(z, QPoint(1500, 900)) == -1, "nothing but our own overlay: no window");
	}

	std::printf("\n-- which monitor it is on --\n");
	{
		const QVector<QRect> monitors = {QRect(0, 0, 1920, 1080), QRect(-1920, 0, 1920, 1080)};
		ok(monitorFor(QRect(100, 100, 400, 300), monitors) == 0, "wholly on the main monitor");
		ok(monitorFor(QRect(-1500, 100, 800, 600), monitors) == 1, "on the left monitor");
		ok(monitorFor(QRect(-300, 100, 1000, 600), monitors) == 0, "straddling: the one with most of it");
		ok(monitorFor(QRect(5000, 5000, 100, 100), monitors) == -1, "off every monitor: none");
	}

	std::printf("\n-- its recording area --\n");
	{
		const QRect left(-1920, 0, 1920, 1080);
		const auto r = regionForWindow(QRect(-1500, 100, 801, 601), left);
		ok(r && *r == QRect(420, 100, 800, 600), "relative to the monitor's corner, even size");
		const auto clipped = regionForWindow(QRect(-300, -50, 1000, 600), QRect(0, 0, 1920, 1080));
		ok(clipped && *clipped == QRect(0, 0, 700, 550), "clipped to the monitor");
		ok(!regionForWindow(QRect(1910, 100, 400, 300), QRect(0, 0, 1920, 1080)),
		   "only a sliver on this monitor: no area");
		const auto max = regionForWindow(QRect(0, 0, 1920, 1040), QRect(0, 0, 1920, 1080));
		ok(max && *max == QRect(0, 0, 1920, 1040), "a maximised window: the screen above the taskbar");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
