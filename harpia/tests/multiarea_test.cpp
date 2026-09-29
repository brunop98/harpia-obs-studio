// Multi-Area: the recording switches between fixed, same-size areas as the
// mouse moves between them.
//
// The switcher (core/MultiArea.hpp) is where every rule lives -- stay put in
// the gaps, wait out the hover delay, prefer the current area when areas
// overlap, pan from wherever the frame is -- so it is pinned tick by tick
// with an injected clock. The Arrange overlay is driven with real mouse and
// key events: stamping, dragging, removing, and the two limits (area 1 stays,
// nine at most).
#include "core/MultiArea.hpp"
#include "ui/MultiAreaOverlay.hpp"

#include <QApplication>
#include <QImage>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QSignalSpy>
#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static void mouse(QWidget *w, QEvent::Type t, QPoint p, Qt::MouseButton b = Qt::LeftButton)
{
	const Qt::MouseButtons held = t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(b);
	QMouseEvent e(t, QPointF(p), w->mapToGlobal(QPointF(p)), t == QEvent::MouseMove ? Qt::NoButton : b,
		      held, Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	// Three 400x300 areas in a row with gaps: [0,400) [500,900) [1000,1400).
	const QSize sz(400, 300);
	const QVector<QPoint> tops{{0, 0}, {500, 0}, {1000, 0}};
	MultiAreaParams cut;
	cut.transition = AreaTransition::Cut;
	cut.hoverMs = 300;

	std::printf("\n-- hit test --\n");
	{
		ok(AreaSwitcher::areaAt(tops, sz, {10, 10}) == 0, "inside area 1");
		ok(AreaSwitcher::areaAt(tops, sz, {399, 299}) == 0, "last pixel is inside");
		ok(AreaSwitcher::areaAt(tops, sz, {400, 10}) == -1, "far edge is outside (half-open, like the crop)");
		ok(AreaSwitcher::areaAt(tops, sz, {450, 10}) == -1, "the gap is no area");
		ok(AreaSwitcher::areaAt(tops, sz, {1100, 10}) == 2, "inside area 3");
		const QVector<QPoint> overlap{{0, 0}, {200, 0}};
		ok(AreaSwitcher::areaAt(overlap, sz, {300, 10}) == 0, "overlap: lowest number by default");
		ok(AreaSwitcher::areaAt(overlap, sz, {300, 10}, 1) == 1, "overlap: the current area wins");
		ok(AreaSwitcher::startIndex(tops, sz, {600, 50}) == 1, "starts on the area under the mouse");
		ok(AreaSwitcher::startIndex(tops, sz, {450, 50}) == 0, "in a gap it starts on area 1");
		ok(AreaSwitcher::clampTop({1700, -5}, sz, {1920, 1080}) == QPoint(1520, 0), "areas stay on screen");
	}

	std::printf("\n-- cut with a hover delay --\n");
	{
		AreaSwitcher s;
		s.arm(tops, sz, 0);
		ok(s.armed() && s.current() == 0 && s.region().x == 0, "armed on area 1");
		ok(!s.tick({100, 100}, 0, cut), "inside the current area: nothing moves");
		s.tick({450, 100}, 100, cut);
		ok(s.current() == 0, "in the gap: stays on the last area");
		s.tick({600, 100}, 200, cut);
		ok(s.current() == 0 && !s.switchedThisTick(), "just entered area 2: not yet (delay)");
		s.tick({650, 100}, 400, cut);
		ok(s.current() == 0, "200 ms in: still waiting");
		const bool moved = s.tick({650, 100}, 500, cut);
		ok(s.current() == 1 && s.switchedThisTick() && moved, "300 ms in: switched");
		ok(s.region().x == 500 && s.region().width == 400, "cut lands on area 2 exactly, same size");
		s.tick({650, 100}, 520, cut);
		ok(!s.switchedThisTick(), "the switch is reported once");

		// Passing through area 3 on the way back to area 1 never shows it.
		s.tick({1100, 100}, 600, cut);
		s.tick({1100, 100}, 800, cut);
		ok(s.current() == 1, "brushed area 3 for 200 ms: no flash");
		s.tick({100, 100}, 850, cut);
		ok(s.current() == 1, "now in area 1: the timer restarts");
		s.tick({100, 100}, 1149, cut);
		ok(s.current() == 1, "299 ms in area 1: still area 2");
		s.tick({100, 100}, 1150, cut);
		ok(s.current() == 0, "300 ms: back to area 1");

		// Leaving for the gap and coming back cancels a pending switch.
		s.tick({600, 100}, 2000, cut);
		s.tick({450, 100}, 2100, cut);
		s.tick({600, 100}, 2200, cut);
		s.tick({600, 100}, 2400, cut);
		ok(s.current() == 0, "a trip through the gap resets the delay");
		s.tick({600, 100}, 2500, cut);
		ok(s.current() == 1, "and it switches 300 ms after the return");
	}

	std::printf("\n-- instant switching --\n");
	{
		MultiAreaParams now = cut;
		now.hoverMs = 0;
		AreaSwitcher s;
		s.arm(tops, sz, 2);
		ok(s.current() == 2 && s.region().x == 1000, "can start on any area");
		s.tick({10, 10}, 0, now);
		ok(s.current() == 0 && s.region().x == 0, "delay 0: switches on the first tick");
	}

	std::printf("\n-- smooth pan --\n");
	{
		MultiAreaParams pan;
		pan.transition = AreaTransition::Pan;
		pan.panMs = 400;
		pan.hoverMs = 0;
		AreaSwitcher s;
		s.arm(tops, sz, 0);
		s.tick({600, 10}, 1000, pan);
		ok(s.current() == 1 && s.panning(), "the switch starts a pan");
		ok(s.region().x == 0, "the first frame of the pan is still where it was");
		s.tick({600, 10}, 1200, pan);
		ok(s.region().x == 250, "halfway through the time is halfway there (ease-in-out)");
		s.tick({600, 10}, 1300, pan);
		const int x3 = s.region().x;
		ok(x3 > 250 && x3 < 500, "three quarters: past halfway, not there yet");
		s.tick({600, 10}, 1400, pan);
		ok(s.region().x == 500 && !s.panning(), "arrives exactly, then stops panning");

		// Change of mind mid-pan: the new pan starts from the in-between spot.
		s.tick({1100, 10}, 2000, pan);
		s.tick({1100, 10}, 2200, pan); // halfway 500 -> 1000
		ok(s.region().x == 750, "mid-pan at 750");
		s.tick({100, 10}, 2200, pan);
		ok(s.current() == 0 && s.region().x == 750, "a new switch mid-pan does not snap");
		s.tick({100, 10}, 2400, pan);
		ok(s.region().x == 375, "it pans on from where it was");
		s.tick({100, 10}, 2600, pan);
		ok(s.region().x == 0, "and lands on area 1");
	}

	std::printf("\n-- a hand-moved frame --\n");
	{
		AreaSwitcher s;
		s.arm(tops, sz, 1);
		s.rebaseCurrent({560, 40});
		ok(s.region().x == 560 && s.region().y == 40, "the area showing follows the drag");
		ok(s.tops()[1] == QPoint(560, 40) && s.tops()[0] == QPoint(0, 0), "only that area moved");
		ok(!s.tick({700, 100}, 0, cut), "and the switcher does not pull it back");
	}

	std::printf("\n-- Arrange overlay --\n");
	{
		MultiAreaOverlay ov;
		QScreen *scr = QGuiApplication::primaryScreen();
		ov.setScreen(scr);
		const QRect g = scr->geometry();
		std::printf("     screen %dx%d dpr %.1f\n", g.width(), g.height(), scr->devicePixelRatio());
		const QSize a(160, 90);
		ov.setAreas({QPoint(0, 0)}, a);
		ov.setMode(MultiAreaOverlay::Mode::Arrange);
		ov.show();
		QSignalSpy edited(&ov, &MultiAreaOverlay::areasEdited);
		QSignalSpy finished(&ov, &MultiAreaOverlay::arrangeFinished);

		// Click an empty spot: an area appears centred on it.
		const QPoint c(400, 300);
		mouse(&ov, QEvent::MouseButtonPress, c);
		mouse(&ov, QEvent::MouseButtonRelease, c);
		ok(ov.areas().size() == 2, "a click stamps a second area");
		ok(ov.areas().size() == 2 && ov.areas()[1] == QPoint(320, 255), "centred on the click");
		ok(edited.count() == 1, "and reports the new layout");

		// Drag it by its interior.
		mouse(&ov, QEvent::MouseButtonPress, {330, 260});
		mouse(&ov, QEvent::MouseMove, {430, 310});
		mouse(&ov, QEvent::MouseButtonRelease, {430, 310});
		ok(ov.areas()[1] == QPoint(420, 305), "dragging moves it by the same amount");
		ok(edited.count() == 2, "a finished drag reports once");

		// Drag area 1 (the region) too.
		mouse(&ov, QEvent::MouseButtonPress, {10, 10});
		mouse(&ov, QEvent::MouseMove, {60, 30});
		mouse(&ov, QEvent::MouseButtonRelease, {60, 30});
		ok(ov.areas()[0] == QPoint(50, 20), "area 1 moves as well");

		// Dragging off the screen stops at the edge.
		mouse(&ov, QEvent::MouseButtonPress, {430, 310});
		mouse(&ov, QEvent::MouseMove, {-500, -500});
		mouse(&ov, QEvent::MouseButtonRelease, {-500, -500});
		ok(ov.areas()[1] == QPoint(0, 0), "kept on screen");

		// Right-click removes -- but never area 1.
		mouse(&ov, QEvent::MouseButtonPress, {60, 30}, Qt::RightButton); // overlapping: topmost is area 2
		ok(ov.areas().size() == 1, "right-click removes the area under it");
		mouse(&ov, QEvent::MouseButtonPress, {60, 30}, Qt::RightButton);
		ok(ov.areas().size() == 1, "area 1 cannot be removed");

		// Delete removes the hovered one.
		const int i = ov.addAtLocal({500, 400});
		mouse(&ov, QEvent::MouseMove, {500, 400});
		QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
		QApplication::sendEvent(&ov, &del);
		ok(i == 1 && ov.areas().size() == 1, "Delete removes the hovered area");

		// Nine at most.
		for (int k = 0; k < 12; ++k)
			ov.addAtLocal({100 + k * 40, 200});
		ok(ov.areas().size() == kMaxAreas, "stops at nine areas");

		// Done / Enter / Esc finish.
		const QRect done = ov.doneRectLocal();
		mouse(&ov, QEvent::MouseButtonPress, done.center() + QPoint(done.width() / 2 - 30, 0));
		ok(finished.count() == 1, "the Done button finishes");
		QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
		QApplication::sendEvent(&ov, &esc);
		ok(finished.count() == 2, "Esc finishes too");

		// Passive takes no input at all.
		ov.setMode(MultiAreaOverlay::Mode::Passive);
		ok(ov.windowFlags().testFlag(Qt::WindowTransparentForInput), "Passive is click-through");
		const int before = ov.areas().size();
		ov.removeArea(8);
		mouse(&ov, QEvent::MouseButtonPress, {700, 500});
		ok(ov.areas().size() == before - 1, "and ignores clicks (no stamp)");
		ov.setMode(MultiAreaOverlay::Mode::Arrange);
		ok(!ov.windowFlags().testFlag(Qt::WindowTransparentForInput), "Arrange takes the mouse again");
	}

	std::printf("\n-- across monitors --\n");
	{
		// Two areas on monitor 0 and two on monitor 1, at the SAME device
		// coordinates on both screens -- only the monitor tells them apart.
		const QVector<QPoint> t{{0, 0}, {500, 0}, {0, 0}, {500, 0}};
		const QVector<int> m{0, 0, 1, 1};
		ok(AreaSwitcher::areaAtOn(t, m, sz, 1, {600, 50}) == 3, "the cursor's monitor picks the area");
		ok(AreaSwitcher::areaAtOn(t, m, sz, 0, {600, 50}) == 1, "same spot on the other monitor");
		ok(AreaSwitcher::areaAtOn(t, m, sz, 2, {600, 50}) == -1, "a monitor with no areas is a gap");
		ok(AreaSwitcher::startIndex(t, sz, {50, 50}, m, 1) == 2, "starts on the area under the mouse, any screen");

		MultiAreaParams pan;
		pan.transition = AreaTransition::Pan;
		pan.panMs = 400;
		pan.hoverMs = 0;
		AreaSwitcher s;
		s.arm(t, sz, 0, true, m);
		ok(s.currentMonitor() == 0, "armed on monitor 0");
		s.tick({600, 50}, 0, pan, 0);
		ok(s.current() == 1 && s.panning(), "same monitor: pans");
		s.tick({600, 50}, 400, pan, 0);
		const bool moved = s.tick({50, 50}, 1000, pan, 1);
		ok(s.current() == 2 && s.currentMonitor() == 1, "cursor on monitor 1: switches there");
		ok(!s.panning() && s.region().x == 0, "other monitor: always a cut, never a pan");
		ok(moved, "reported as moved (the picture changed even where pixels did not)");
		s.tick({50, 50}, 1100, pan, -1);
		ok(s.current() == 2, "cursor on no layout monitor: stays (a gap)");
		// Same coordinates, other monitor, no pixel move at all.
		AreaSwitcher c;
		c.arm({{100, 100}, {100, 100}}, sz, 0, true, {0, 1});
		ok(c.tick({150, 150}, 0, cut, 1) == false && c.current() == 0, "delay still applies across monitors");
		ok(c.tick({150, 150}, 300, cut, 1) == true && c.currentMonitor() == 1,
		   "an identical-coordinate switch to another monitor still reports a move");
	}

	std::printf("\n-- layout edits per monitor --\n");
	{
		using namespace area_layout;
		// Global order: A0 (region), B0, A1, B1.
		const QVector<AreaRef> lay{{0, {0, 0}}, {1, {10, 0}}, {0, {20, 0}}, {1, {30, 0}}};
		ok(topsOn(lay, 1) == QVector<QPoint>({{10, 0}, {30, 0}}), "a screen's share, in order");
		ok(numbersOn(lay, 1) == QVector<int>({2, 4}), "with its global numbers");
		const QVector<AreaRef> moved = mergeEdit(lay, 1, {{11, 5}, {30, 0}});
		ok(moved[1].top == QPoint(11, 5) && moved.size() == 4 && moved[3].top == QPoint(30, 0),
		   "a move keeps the area's number");
		const QVector<AreaRef> added = mergeEdit(lay, 0, {{0, 0}, {20, 0}, {40, 0}});
		ok(added.size() == 5 && added[4].monitor == 0 && added[4].top == QPoint(40, 0),
		   "a new area gets the next number");
		const QVector<AreaRef> removed = mergeEdit(lay, 0, {{0, 0}});
		ok(removed.size() == 3 && removed[0].monitor == 0 && removed[1].monitor == 1 && removed[2].monitor == 1,
		   "a removal closes up (later areas renumber)");
		const QVector<AreaRef> fresh = mergeEdit(lay, 2, {{5, 5}});
		ok(fresh.size() == 5 && fresh[4].monitor == 2, "the first area on a new monitor");
		ok(monitors(fresh) == QVector<int>({0, 1, 0, 1, 2}) && area_layout::tops(fresh).size() == 5, "flattens for the switcher");
	}

	std::printf("\n-- one overlay per monitor --\n");
	{
		MultiAreaOverlay ov;
		ov.setScreen(QGuiApplication::primaryScreen());
		ov.setAreas({QPoint(0, 0), QPoint(300, 0)}, QSize(160, 90));
		ov.setNumbers({2, 4});
		ov.setLockedIndex(-1); // the region is on another screen
		ov.setMaxAreas(3);
		ov.setMode(MultiAreaOverlay::Mode::Arrange);
		ov.removeArea(0);
		ok(ov.areas().size() == 1, "no region here: any area can be removed");
		ok(ov.addAtLocal({400, 400}) >= 0 && ov.addAtLocal({600, 400}) >= 0, "room for two more");
		ok(ov.addAtLocal({200, 600}) == -1, "the shared nine-area allowance stops the next");
		MultiAreaOverlay big;
		big.setScreen(QGuiApplication::primaryScreen());
		big.setAreas({}, QSize(5000, 5000));
		big.setLockedIndex(-1);
		ok(!big.areaFits() && big.addAtLocal({100, 100}) == -1, "a region bigger than this screen cannot be stamped");
		MultiAreaOverlay lk;
		lk.setScreen(QGuiApplication::primaryScreen());
		lk.setAreas({QPoint(0, 0), QPoint(300, 0), QPoint(0, 300)}, QSize(160, 90));
		lk.setLockedIndex(1);
		lk.removeArea(1);
		ok(lk.areas().size() == 3, "the locked area stays, wherever it is in the list");
		lk.removeArea(0);
		lk.removeArea(0); // was index 1 (the locked one) before the first removal
		ok(lk.areas().size() == 2, "and follows its index when an earlier one goes");
	}

	std::printf("\n-- area style: JSON --\n");
	{
		AreaStyle st;
		st.lineColor = QColor(0xff, 0x00, 0x00);
		st.lineWidth = 5;
		st.lineStyle = 3;
		st.badgeSize = 0;
		const QJsonObject o = areaStyleToJson(st);
		ok(o.value("lineColor").toString() == "#ff0000" && o.value("lineWidth").toInt() == 5,
		   "colours as #rrggbb, numbers as numbers");
		AreaStyle back;
		ok(areaStyleFromJson(o, back) == areaStyleFields().size(), "every field comes back");
		ok(back.lineColor == st.lineColor && back.lineWidth == 5 && back.lineStyle == 3 && back.badgeSize == 0,
		   "round trip is exact");
		ok(back.penStyle() == Qt::DashDotLine, "line style 3 is dash-dot");

		QJsonObject bad;
		bad["lineWidth"] = 999;
		bad["idleOpacity"] = -20;
		bad["lineColor"] = "not a colour";
		bad["nonsense"] = 1;
		AreaStyle clamped;
		const int n = areaStyleFromJson(bad, clamped);
		ok(n == 2, "only the two known, well-formed values apply");
		ok(clamped.lineWidth == 12 && clamped.idleOpacity == 5, "out-of-range values are clamped");
		ok(clamped.lineColor == AreaStyle().lineColor, "a bad colour leaves the old one");
	}

	std::printf("\n-- area style: drawn --\n");
	{
		MultiAreaOverlay ov;
		ov.setScreen(QGuiApplication::primaryScreen());
		ov.setAreas({QPoint(100, 100), QPoint(400, 100)}, QSize(200, 100));
		ov.setActive(0); // area 1 is the region frame's to draw
		AreaStyle st;
		st.lineColor = QColor(255, 0, 0);
		st.lineWidth = 4;
		st.lineStyle = 0;
		st.idleOpacity = 100;
		st.badgeSize = 0;
		ov.setStyle(st);
		QImage img(ov.size(), QImage::Format_ARGB32_Premultiplied);
		img.fill(Qt::transparent);
		ov.render(&img);
		const QColor edge = img.pixelColor(401, 150), inside = img.pixelColor(500, 150);
		std::printf("     edge %d,%d,%d,%d  inside alpha %d\n", edge.red(), edge.green(), edge.blue(),
			    edge.alpha(), inside.alpha());
		ok(edge.red() > 200 && edge.green() < 40 && edge.alpha() > 200, "outline in the chosen colour");
		ok(img.pixelColor(403, 150).red() > 200, "4 px wide, drawn inside the area");
		ok(img.pixelColor(405, 150).alpha() == 0, "and no wider");
		ok(inside.alpha() == 0, "the inside stays see-through");
		ok(img.pixelColor(101, 150).alpha() == 0, "the recorded area gets no outline (the frame has it)");
		ok(img.pixelColor(412, 112).alpha() == 0, "badge size 0 hides the numbers");

		st.recordingOpacity = 40;
		ov.setStyle(st);
		ov.setRecording(true);
		img.fill(Qt::transparent);
		ov.render(&img);
		const int a = img.pixelColor(401, 150).alpha();
		std::printf("     recording alpha %d\n", a);
		ok(a > 90 && a < 115, "while recording it draws at the recording opacity (40%)");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
