// The window picker's layer, with real mouse and key events: moving the
// pointer outlines the window under it (never our own), a click picks it, a
// click on bare desktop does not, Esc and right-click cancel, and every layer
// closes afterwards. The window list is given (the platform's is empty here).
#include "ui/WindowPicker.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QSignalSpy>
#include <QThread>
#include <QWidget>

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

static DesktopWindow win(quintptr id, QRect r, const QString &title, bool ours = false)
{
	DesktopWindow w;
	w.id = id;
	w.bounds = r;
	w.title = title;
	w.ours = ours;
	return w;
}

static void send(QWidget *w, QEvent::Type t, QPointF p, Qt::MouseButton b = Qt::NoButton)
{
	QMouseEvent e(t, p, w->mapToGlobal(p), b, t == QEvent::MouseButtonPress ? Qt::MouseButtons(b) : Qt::NoButton,
		      Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QScreen *screen = QGuiApplication::primaryScreen();
	const QRect phys(QPoint(0, 0), screen->geometry().size() * screen->devicePixelRatio());
	const QVector<DesktopWindow> list = {
		win(1, QRect(0, 0, 100, 40), QStringLiteral("Harpia"), true), // our own window: never picked
		win(2, QRect(100, 100, 300, 200), QStringLiteral("Notepad")),
		win(3, QRect(50, 50, 600, 450), QStringLiteral("Browser")),
	};

	WindowPicker picker;
	QSignalSpy picked(&picker, &WindowPicker::picked);
	QSignalSpy cancelled(&picker, &WindowPicker::cancelled);

	std::printf("\n-- hover and pick --\n");
	picker.startWith(list, {{screen, phys}});
	settle(100);
	ok(picker.active() && picker.layersForTest().size() == 1, "a layer over the screen");
	QWidget *layer = picker.layersForTest().value(0);
	ok(layer && layer->isVisible(), "shown");
	if (!layer)
		return 1;
	const double dpr = screen->devicePixelRatio();
	const auto local = [&](int x, int y) { return QPointF(x / dpr, y / dpr); };
	send(layer, QEvent::MouseMove, local(200, 150));
	ok(picker.hovered() == 1, "pointing at Notepad outlines Notepad (the top one)");
	send(layer, QEvent::MouseMove, local(500, 400));
	ok(picker.hovered() == 2, "pointing at the Browser behind it outlines the Browser");
	send(layer, QEvent::MouseMove, local(20, 20));
	ok(picker.hovered() == -1, "our own window is never outlined");
	ok(!layer->grab().isNull(), "the layer paints");
	send(layer, QEvent::MouseButtonPress, local(20, 20), Qt::LeftButton);
	ok(picked.isEmpty() && picker.active(), "a click on nothing pickable keeps picking");
	send(layer, QEvent::MouseButtonPress, local(200, 150), Qt::LeftButton);
	settle(50);
	ok(picked.size() == 1, "a click picks");
	if (picked.size() == 1) {
		const DesktopWindow w = picked.at(0).at(0).value<DesktopWindow>();
		ok(w.id == 2 && w.bounds == QRect(100, 100, 300, 200), "the window under the pointer, with its bounds");
	}
	ok(!picker.active(), "and the layer closes");

	std::printf("\n-- cancel --\n");
	picker.startWith(list, {{screen, phys}});
	settle(50);
	layer = picker.layersForTest().value(0);
	if (layer) {
		QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
		QApplication::sendEvent(layer, &esc);
	}
	settle(50);
	ok(cancelled.size() == 1 && !picker.active(), "Esc cancels and closes");
	picker.startWith(list, {{screen, phys}});
	settle(50);
	layer = picker.layersForTest().value(0);
	if (layer)
		send(layer, QEvent::MouseButtonPress, local(200, 150), Qt::RightButton);
	settle(50);
	ok(cancelled.size() == 2 && !picker.active() && picked.size() == 1, "a right-click cancels too");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
