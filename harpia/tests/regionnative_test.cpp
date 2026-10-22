// The Custom Region on a second display at another scale (reported: a monitor
// to the LEFT of the main one, the two at different Windows scales, and the
// recording shifted from the frame, same size).
//
// Run on two offscreen displays like that one: "LEFT" at 125% to the left of
// "MAIN" at 100%. Qt's own arithmetic is right there (the first checks show
// it), so what goes wrong is Windows having the window somewhere other than
// where Qt worked out it is. A stand-in for Windows (native_window::Ops) puts
// the frame off by a set amount, and the checks are that:
//   * a placement Windows got wrong is put back, reported once, and the
//     region reported is the one asked for;
//   * a frame Windows moves on its own afterwards takes the crop with it --
//     what is recorded is what the frame surrounds;
//   * a pixel of rounding is not a frame in the wrong place;
//   * the draw layer turns a drag into the display's pixels from where the
//     layer really is.
//
//   QT_QPA_PLATFORM=offscreen:configfile=<two screens> regionnative_test
#include "ui/NativeWindowOps.hpp"
#include "ui/RegionDraw.hpp"
#include "ui/RegionTool.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QScreen>
#include <QSignalSpy>
#include <QWindow>

#include <cmath>
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
	while (t.elapsed() < ms)
		QApplication::processEvents();
}

static QScreen *screenNamed(const char *name)
{
	for (QScreen *s : QGuiApplication::screens())
		if (s->name() == QLatin1String(name))
			return s;
	return nullptr;
}

// The stand-in for Windows: a window is where Qt's arithmetic says (its
// display's origin plus its offset times the scale) plus a shift of our
// choosing; moving it changes the shift.
static QHash<const QWidget *, QPoint> shifts;
static QRect qtNative(const QWidget *w)
{
	const QScreen *s = w->windowHandle() ? w->windowHandle()->screen() : nullptr;
	if (!s)
		return QRect();
	const qreal k = s->devicePixelRatio();
	const QPoint o = s->geometry().topLeft();
	const QRect g = w->geometry();
	return QRect(o.x() + int(std::lround((g.x() - o.x()) * k)), o.y() + int(std::lround((g.y() - o.y()) * k)),
		     int(std::lround(g.width() * k)), int(std::lround(g.height() * k)));
}
static void playWindows()
{
	native_window::Ops ops;
	ops.rect = [](const QWidget *w) -> std::optional<QRect> {
		const QRect r = qtNative(w);
		if (r.isNull())
			return std::nullopt;
		return r.translated(shifts.value(w));
	};
	ops.moveTo = [](QWidget *w, const QPoint &p) {
		shifts[w] = p - qtNative(w).topLeft();
		return true;
	};
	native_window::setOpsForTest(ops);
}

static QRect asRect(const CaptureRegion &r)
{
	return QRect(r.x, r.y, r.width, r.height);
}

// Within a pixel: the stand-in rounds the window's position and the frame's
// 12.5 px margin (10 px at 125%) separately, as any real display would.
static bool near(const QRect &a, const QRect &b)
{
	return std::abs(a.x() - b.x()) <= 1 && std::abs(a.y() - b.y()) <= 1 && std::abs(a.width() - b.width()) <= 1 &&
	       std::abs(a.height() - b.height()) <= 1;
}
static bool near(const QPoint &a, const QPoint &b)
{
	return std::abs(a.x() - b.x()) <= 1 && std::abs(a.y() - b.y()) <= 1;
}

static void printRect(const char *what, const QRect &r)
{
	std::printf("     %s %d x %d at %d,%d\n", what, r.width(), r.height(), r.x(), r.y());
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QScreen *left = screenNamed("LEFT"), *main = screenNamed("MAIN");
	if (!left || !main) {
		std::printf("FAIL needs the two-screen offscreen configuration (LEFT and MAIN)\n");
		return 1;
	}
	std::printf("     LEFT at %d,%d scale %.2f; MAIN at %d,%d scale %.2f\n", left->geometry().x(),
		    left->geometry().y(), left->devicePixelRatio(), main->geometry().x(), main->geometry().y(),
		    main->devicePixelRatio());

	const QRect asked(400, 300, 640, 360);

	std::printf("\n-- Qt's arithmetic on its own (no Windows to ask) --\n");
	{
		RegionTool t;
		QSignalSpy changed(&t, &RegionTool::regionChanged);
		t.setScreen(left);
		t.setRegionDevicePx(asked);
		t.show();
		settle(100);
		printRect("reported", asRect(t.region()));
		ok(asRect(t.region()) == asked, "on the 125% display to the left: the region asked for");
		t.setScreen(main);
		t.setRegionDevicePx(asked);
		settle(100);
		ok(asRect(t.region()) == asked, "and moved to the 100% main display: the same");
	}

	playWindows();

	std::printf("\n-- Windows puts the frame elsewhere when it is placed --\n");
	{
		RegionTool t;
		t.setScreen(left);
		t.setRegionDevicePx(QRect(100, 100, 640, 360));
		t.show();
		settle(100);
		QSignalSpy changed(&t, &RegionTool::regionChanged), mismatch(&t, &RegionTool::placementMismatch);
		// The next placement lands 150 px right and 40 down of where asked.
		shifts[&t] = QPoint(150, 40);
		t.setRegionDevicePx(asked);
		ok(mismatch.count() == 1, "reported, once");
		if (mismatch.count() == 1) {
			printRect("asked", mismatch.first().at(0).toRect());
			printRect("landed", mismatch.first().at(1).toRect());
			ok(near(mismatch.first().at(1).toRect(), asked.translated(150, 40)), "with where it landed");
		}
		ok(near(shifts.value(&t), QPoint(0, 0)), "and the frame is moved back where it was asked to be");
		ok(changed.count() == 1 && asRect(changed.first().first().value<CaptureRegion>()) == asked,
		   "one regionChanged, with the region asked for");
		settle(100);
		ok(changed.count() == 1, "nothing more once things settle");
	}

	std::printf("\n-- Windows moves the frame on its own afterwards --\n");
	{
		RegionTool t;
		t.setScreen(left);
		t.setRegionDevicePx(asked);
		t.show();
		settle(100);
		QSignalSpy changed(&t, &RegionTool::regionChanged);
		shifts[&t] = QPoint(-120, 30);
		QMoveEvent mv(t.pos(), t.pos());
		QApplication::sendEvent(&t, &mv);
		settle(100);
		ok(changed.count() == 1, "the move is noticed");
		if (changed.count() == 1) {
			const QRect r = asRect(changed.first().first().value<CaptureRegion>());
			printRect("crop now", r);
			ok(near(r, asked.translated(-120, 30)), "and the crop goes to where the frame is");
		}
		ok(near(asRect(t.region()), asked.translated(-120, 30)), "region() is what the frame surrounds");
	}

	std::printf("\n-- a pixel of rounding is not a frame in the wrong place --\n");
	{
		RegionTool t;
		t.setScreen(left);
		t.show();
		settle(50);
		QSignalSpy mismatch(&t, &RegionTool::placementMismatch);
		shifts[&t] = QPoint(1, -1);
		t.setRegionDevicePx(asked);
		settle(50);
		ok(mismatch.isEmpty() && asRect(t.region()) == asked, "1 px off: left alone, the region as asked");
	}

	std::printf("\n-- the draw layer --\n");
	{
		RegionDraw d;
		QSignalSpy drawn(&d, &RegionDraw::drawn);
		const auto dragOn = [&d](QPoint from, QPoint to) {
			const auto send = [&d](QEvent::Type type, QPoint at, Qt::MouseButtons held) {
				QMouseEvent e(type, QPointF(at), d.mapToGlobal(QPointF(at)),
					      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held, Qt::NoModifier);
				QApplication::sendEvent(&d, &e);
			};
			send(QEvent::MouseButtonPress, from, Qt::LeftButton);
			send(QEvent::MouseMove, (from + to) / 2, Qt::LeftButton);
			send(QEvent::MouseMove, to, Qt::LeftButton);
			send(QEvent::MouseButtonRelease, to, Qt::NoButton);
		};
		d.start(left);
		settle(100);
		dragOn({100, 100}, {420, 340});
		const QRect plain = RegionDraw::toDevicePx(QRect(100, 100, 320, 240), left->devicePixelRatio());
		ok(drawn.count() == 1 && drawn.last().first().toRect() == plain,
		   "covering the display as asked: the plain conversion");
		d.start(left);
		settle(50);
		shifts[&d] = QPoint(-40, 25);
		dragOn({100, 100}, {420, 340});
		if (drawn.count() == 2)
			printRect("drawn", drawn.last().first().toRect());
		ok(drawn.count() == 2 && drawn.last().first().toRect() == plain.translated(-40, 25),
		   "the layer somewhere else: what was under the pointer");
	}

	native_window::setOpsForTest({});
	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
