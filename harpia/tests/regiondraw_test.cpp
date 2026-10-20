// "Draw the area": the Custom Region by dragging over it.
//
// The rectangle a drag makes -- either direction, clamped to the screen,
// edges snapped to its edges, the size snapped to common resolutions from the
// side being dragged -- and in device pixels at a 2x display. Then the layer
// itself, with real mouse and key events: a drag reports its rectangle and
// closes, a click without a drag does nothing, Esc and a right-click cancel.
#include "ui/RegionDraw.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QSignalSpy>
#include <QThread>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static void settle(int ms)
{
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < ms) {
		QApplication::processEvents();
		QThread::msleep(5);
	}
}

static void mouse(QWidget *w, QEvent::Type type, const QPoint &at, Qt::MouseButton b = Qt::LeftButton)
{
	const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton
				      : type == QEvent::MouseMove	  ? Qt::MouseButtons(Qt::LeftButton)
									  : Qt::MouseButtons(b);
	QMouseEvent e(type, QPointF(at), w->mapToGlobal(QPointF(at)), type == QEvent::MouseMove ? Qt::NoButton : b, held,
		      Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}

static void drag(QWidget *w, QPoint from, QPoint to)
{
	mouse(w, QEvent::MouseButtonPress, from);
	for (int i = 1; i <= 8; ++i)
		mouse(w, QEvent::MouseMove, from + (to - from) * i / 8);
	mouse(w, QEvent::MouseButtonRelease, to);
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- the rectangle a drag makes --\n");
	const QSize fhd(1920, 1080);
	ok(RegionDraw::dragRect({100, 100}, {500, 400}, fhd, 1.0) == QRect(100, 100, 400, 300),
	   "corner to corner: 400 x 300");
	ok(RegionDraw::dragRect({500, 400}, {100, 100}, fhd, 1.0) == QRect(100, 100, 400, 300),
	   "the same dragged the other way");
	ok(RegionDraw::dragRect({100, 100}, {2500, 1500}, fhd, 1.0) == QRect(100, 100, 1820, 980),
	   "past the screen's edge: stops at it");
	ok(RegionDraw::dragRect({6, 9}, {600, 500}, fhd, 1.0) == QRect(0, 0, 600, 500),
	   "a start a few pixels from the corner: the corner");
	ok(RegionDraw::dragRect({300, 200}, {1912, 1075}, fhd, 1.0).bottomRight() == QPoint(1919, 1079),
	   "an end a few pixels from the far corner: that corner");
	{
		const QRect r = RegionDraw::dragRect({200, 100}, {200 + 1274, 100 + 714}, fhd, 1.0);
		std::printf("     1274 x 714 dragged -> %d x %d at %d,%d\n", r.width(), r.height(), r.x(), r.y());
		ok(r == QRect(200, 100, 1280, 720), "close to 1280 x 720: exactly, the start corner kept");
	}
	{
		const QRect r = RegionDraw::dragRect({1500, 1060}, {1500 - 1074, 1060 - 1000}, QSize(1920, 2000), 1.0);
		ok(r.width() == 1080 && r.right() == 1499, "close to a 1080 width dragged leftwards: the far edge moves");
	}
	{
		const QSize tall(1080, 1920);
		const QRect r = RegionDraw::dragRect({0, 0}, {1080, 1915}, tall, 1.0);
		ok(r == QRect(0, 0, 1080, 1920), "a vertical display, end to end: 1080 x 1920");
	}
	{
		// A 2x display: 1280 recorded pixels are 640 on screen.
		const QRect r = RegionDraw::dragRect({100, 100}, {100 + 636, 100 + 357}, fhd, 2.0);
		const QRect dev = RegionDraw::toDevicePx(r, 2.0);
		std::printf("     at 2x: %d x %d on screen -> %d x %d recorded\n", r.width(), r.height(), dev.width(),
			    dev.height());
		ok(dev == QRect(200, 200, 1280, 720), "at 2x, the snap is to recorded pixels: 1280 x 720 at 200,200");
	}

	std::printf("\n-- the layer --\n");
	QScreen *screen = QGuiApplication::primaryScreen();
	RegionDraw draw;
	QSignalSpy drawn(&draw, &RegionDraw::drawn), cancelled(&draw, &RegionDraw::cancelled);
	draw.start(screen);
	settle(100);
	ok(draw.active() && draw.geometry() == screen->geometry(), "covers the display");
	const qreal dpr = screen->devicePixelRatio();

	drag(&draw, {120, 140}, {420, 360});
	settle(50);
	ok(drawn.count() == 1 && !draw.active(), "a drag reports one rectangle, and the layer goes");
	if (drawn.count() == 1) {
		const QRect r = drawn.first().first().toRect();
		std::printf("     %d x %d at %d,%d (device px)\n", r.width(), r.height(), r.x(), r.y());
		ok(r == RegionDraw::toDevicePx(QRect(120, 140, 300, 220), dpr), "the rectangle dragged over");
	}

	draw.start(screen);
	settle(50);
	drag(&draw, {300, 300}, {305, 303});
	settle(50);
	ok(drawn.count() == 1 && draw.active() && cancelled.count() == 0, "a click without a drag: nothing, still drawing");
	{
		QImage img(draw.size(), QImage::Format_ARGB32_Premultiplied);
		img.fill(Qt::transparent);
		mouse(&draw, QEvent::MouseButtonPress, {200, 200});
		mouse(&draw, QEvent::MouseMove, {500, 450});
		draw.render(&img);
		const int outside = qAlpha(img.pixel(100, 600)), inside = qAlpha(img.pixel(350, 330));
		std::printf("     mid-drag: alpha %d outside the area, %d inside it\n", outside, inside);
		ok(outside > 80 && inside < 10, "mid-drag the screen is dimmed and the area left clear");
		img.save(QString::fromLocal8Bit(argc > 1 ? argv[1] : ".") + QStringLiteral("/regiondraw.png"));
		mouse(&draw, QEvent::MouseButtonRelease, {500, 450});
		settle(50);
		ok(drawn.count() == 2, "and on release it is the region");
	}

	draw.start(screen);
	settle(50);
	QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
	QApplication::sendEvent(&draw, &esc);
	ok(cancelled.count() == 1 && !draw.active() && drawn.count() == 2, "Esc cancels, nothing drawn");

	draw.start(screen);
	settle(50);
	mouse(&draw, QEvent::MouseButtonPress, {200, 200}, Qt::RightButton);
	ok(cancelled.count() == 2 && !draw.active(), "so does a right-click");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
