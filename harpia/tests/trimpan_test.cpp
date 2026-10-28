// Navigating a zoomed Simple Trim bar must never alter its trim or playhead.
#include <QtWidgets>
#include <QTest>
#include <QSignalSpy>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#define private public
#include "editor/EditorWidgets.hpp"
#undef private

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *description)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
	failures += !condition;
}
void press(Timeline &view, QPoint point, Qt::MouseButton button = Qt::MiddleButton,
	   Qt::MouseButtons held = Qt::MiddleButton)
{
	QMouseEvent event(QEvent::MouseButtonPress, QPointF(point), view.mapToGlobal(point), button, held, Qt::NoModifier);
	QApplication::sendEvent(&view, &event);
}
void move(Timeline &view, QPoint point, Qt::MouseButtons held = Qt::MiddleButton)
{
	QMouseEvent event(QEvent::MouseMove, QPointF(point), view.mapToGlobal(point), Qt::NoButton, held, Qt::NoModifier);
	QApplication::sendEvent(&view, &event);
}
void release(Timeline &view, QPoint point, Qt::MouseButton button = Qt::MiddleButton,
	     Qt::MouseButtons held = Qt::NoButton)
{
	QMouseEvent event(QEvent::MouseButtonRelease, QPointF(point), view.mapToGlobal(point), button, held, Qt::NoModifier);
	QApplication::sendEvent(&view, &event);
}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QWidget host;
	host.resize(900, 150);
	Timeline view(&host);
	view.resize(900, 150);
	view.setDuration(120000);
	view.setStart(4000);
	view.setEnd(90000);
	view.setPlayhead(25000);
	host.show();
	view.show();
	QApplication::processEvents();
	view.zoom_ = 6.0;
	view.viewStart_ = 30000;
	const int y = view.lp_.barTop + view.lp_.barH / 2;
	const QPoint from(650, y), to(450, y);
	QSignalSpy starts(&view, &Timeline::startChanged), ends(&view, &Timeline::endChanged);
	QSignalSpy scrubs(&view, &Timeline::scrub), hovers(&view, &Timeline::hoverScrub);
	const qint64 anchor = view.xToMs(from.x());
	press(view, from);
	check(view.cursor().shape() == Qt::ClosedHandCursor, "middle press shows the grab cursor");
	for (int x = from.x(); x >= to.x(); x -= 5)
		move(view, QPoint(x, y));
	check(view.viewStart_ > 30000, "pulling the trim timeline left reveals later time");
	check(std::llabs(view.xToMs(to.x()) - anchor) <= 1, "the grabbed instant remains under the pointer across many moves");
	release(view, to);
	check(view.grab_ == Timeline::Grab::None && !view.testAttribute(Qt::WA_SetCursor), "middle release restores the inherited cursor and ends the pan");
	press(view, to);
	move(view, from);
	release(view, from);
	check(std::llabs(view.viewStart_ - 30000) <= 1, "an equal reverse drag returns to the original time window");
	check(view.start() == 4000 && view.end() == 90000 && view.playhead() == 25000,
	      "panning leaves both trim boundaries and the playhead unchanged");
	check(starts.isEmpty() && ends.isEmpty() && scrubs.isEmpty() && hovers.isEmpty(), "panning emits no trim, scrub or hover-preview edits");

	press(view, from);
	const double zoom = view.zoom_;
	QWheelEvent wheel(QPointF(from), view.mapToGlobal(from), QPoint(), QPoint(0, 120),
		Qt::MiddleButton, Qt::NoModifier, Qt::NoScrollPhase, false);
	QApplication::sendEvent(&view, &wheel);
	check(view.zoom_ == zoom, "wheel input cannot change scale in the middle of a grab");
	press(view, to, Qt::LeftButton, Qt::LeftButton | Qt::MiddleButton);
	release(view, to, Qt::LeftButton, Qt::MiddleButton);
	check(view.grab_ == Timeline::Grab::Pan, "an extra button press and release do not end a middle-button pan");
	move(view, to);
	check(view.viewStart_ > 30000 && starts.isEmpty() && scrubs.isEmpty(), "the original middle grab continues without editing after an unrelated release");
	release(view, to);
	const qint64 stopped = view.viewStart_;
	move(view, from, Qt::NoButton);
	check(view.viewStart_ == stopped, "buttonless movement after release never pans");

	press(view, from);
	const int hoverCount = hovers.size();
	move(view, to, Qt::NoButton);
	check(view.grab_ == Timeline::Grab::None && view.viewStart_ == stopped && hovers.size() == hoverCount,
	      "a lost middle release stops the grab without issuing a hover preview");
	view.setCursor(Qt::CrossCursor);
	press(view, from);
	QEvent ungrab(QEvent::UngrabMouse);
	QApplication::sendEvent(&view, &ungrab);
	check(view.grab_ == Timeline::Grab::None && view.cursor().shape() == Qt::CrossCursor,
	      "losing the mouse grab restores a preexisting explicit cursor");
	view.unsetCursor();
	press(view, from);
	view.hide();
	check(view.grab_ == Timeline::Grab::None && !view.testAttribute(Qt::WA_SetCursor), "hiding a timeline cancels its middle grab");
	view.show();
	QApplication::processEvents();

	view.zoom_ = 6.0;
	view.viewStart_ = 30000;
	press(view, from);
	move(view, QPoint(1000000, y));
	check(view.viewStart_ == 0, "panning clamps at the beginning of the source");
	move(view, QPoint(-1000000, y));
	check(view.viewStart_ == view.duration_ - view.visibleMs(), "panning clamps at the end of the source");
	release(view, to);
	view.zoom_ = 400.0;
	view.viewStart_ = 30000;
	press(view, from);
	for (int x = from.x() - 1; x >= to.x(); --x)
		move(view, QPoint(x, y));
	release(view, to);
	check(view.viewStart_ > 30050, "tiny per-pixel movements accumulate correctly at frame-level zoom");

	view.zoom_ = 1.0;
	view.viewStart_ = 0;
	press(view, from);
	move(view, to);
	release(view, to);
	check(view.viewStart_ == 0, "a fully fitted timeline stays at its only valid time window");
	press(view, from);
	view.setDuration(60000);
	check(view.grab_ == Timeline::Grab::None && !view.testAttribute(Qt::WA_SetCursor), "changing source resets an unfinished middle grab");

	view.setStart(4000);
	view.setEnd(50000);
	view.zoom_ = 3.0;
	const QPoint trim(view.msToX(view.start()), y);
	const qint64 trimBefore = view.start();
	press(view, trim, Qt::LeftButton, Qt::LeftButton);
	move(view, trim + QPoint(40, 0), Qt::LeftButton);
	release(view, trim + QPoint(40, 0), Qt::LeftButton);
	check(view.start() > trimBefore && !starts.isEmpty() && !scrubs.isEmpty() && view.viewStart_ == 0,
	      "normal left-button trimming still edits the handle after panning");
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
