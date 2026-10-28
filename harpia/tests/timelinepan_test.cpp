// Grabbing the timeline and sliding it.
//
// Zoomed in, the only ways to move along the timeline were the horizontal
// scrollbar and the wheel. Grabbing the thing itself and dragging is the
// gesture people try first, and it did nothing.
//
// The whole promise of a grab is that the instant under the pointer stays under
// the pointer, and that is exactly the part that goes wrong quietly:
//
//   - the view nudged by each mouse-move's delta rather than measured from the
//     press, which drifts over a long drag and never quite lines up;
//   - the sign inverted, so the timeline runs away from the hand;
//   - the pan mode never cleared on release, after which every buttonless
//     mouse-move slides the view -- the release handler returns early for
//     anything that is not the left button, and a pan is a MIDDLE-button drag;
//   - the new gesture eating a gesture that already worked, so dragging a clip
//     or scrubbing empty space stops doing what it always did.
#include "editor/timeline/TimelineModel.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static void press(QWidget *w, QPoint p, Qt::MouseButton b,
		  Qt::KeyboardModifiers m = Qt::NoModifier)
{
	QMouseEvent e(QEvent::MouseButtonPress, QPointF(p), w->mapToGlobal(p), b, b, m);
	QApplication::sendEvent(w, &e);
}
static void moveTo(QWidget *w, QPoint p, Qt::MouseButtons held,
		   Qt::KeyboardModifiers m = Qt::NoModifier)
{
	QMouseEvent e(QEvent::MouseMove, QPointF(p), w->mapToGlobal(p), Qt::NoButton, held, m);
	QApplication::sendEvent(w, &e);
}
static void release(QWidget *w, QPoint p, Qt::MouseButton b)
{
	QMouseEvent e(QEvent::MouseButtonRelease, QPointF(p), w->mapToGlobal(p), b, Qt::NoButton,
		      Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}

// A long timeline, zoomed in, so there is somewhere to pan TO.
static TimelineView *build(QWidget *parent = nullptr)
{
	auto *tv = new TimelineView(parent);
	tv->resize(900, 300);
	TimelineModel m;
	TlTrack t;
	t.kind = TlTrack::Kind::Video;
	for (int i = 0; i < 6; ++i) {
		TlClip c;
		c.type = TlClip::Type::Video;
		c.sourceId = 1;
		c.srcStartMs = 0;
		c.srcEndMs = 10000;
		c.outStartMs = qint64(i) * 10000;
		t.clips.append(c);
	}
	m.tracks.append(t);
	tv->setModel(m);
	tv->show();
	QApplication::processEvents();
	tv->setZoomForTest(6.0); // 1/6th of the span visible
	QApplication::processEvents();
	return tv;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- which presses are a grab --\n");
	{
		// Left alone is not: empty space scrubs and a clip moves, and taking
		// left would trade one navigation gesture for another.
		QWidget dummy;
		QMouseEvent l(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0), Qt::LeftButton,
			      Qt::LeftButton, Qt::NoModifier);
		QMouseEvent alt(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0),
				Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
		QMouseEvent mid(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0),
				Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
		QMouseEvent rgt(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0),
				Qt::RightButton, Qt::RightButton, Qt::NoModifier);
		ok(TimelineView::canPanFrom(&mid), "the middle button grabs");
		ok(TimelineView::canPanFrom(&alt), "so does Alt+left, for mice without one");
		ok(!TimelineView::canPanFrom(&l), "a plain left press does not");
		ok(!TimelineView::canPanFrom(&rgt), "and neither does right, which opens a menu");
	}

	std::printf("\n-- dragging slides the view, and the right way round --\n");
	{
		TimelineView *tv = build();
		// Start somewhere with room to move in both directions.
		tv->setViewStartForTest(20000);
		QApplication::processEvents();
		const qint64 before = tv->viewStartMs();

		const QPoint from(600, 150), to(400, 150);
		press(tv, from, Qt::MiddleButton);
		moveTo(tv, to, Qt::MiddleButton);
		release(tv, to, Qt::MiddleButton);
		QApplication::processEvents();
		std::printf("     dragged 200px LEFT: view %lld -> %lld ms\n", (long long)before,
			    (long long)tv->viewStartMs());
		// Pulling the content left brings later material into view, exactly as
		// a hand on a map does. Inverting this is the single most common way to
		// get a pan wrong and it feels immediately awful.
		ok(tv->viewStartMs() > before, "dragging left moves forward in time");

		const qint64 mid = tv->viewStartMs();
		press(tv, to, Qt::MiddleButton);
		moveTo(tv, from, Qt::MiddleButton);
		release(tv, from, Qt::MiddleButton);
		QApplication::processEvents();
		std::printf("     dragged 200px RIGHT: view %lld -> %lld ms\n", (long long)mid,
			    (long long)tv->viewStartMs());
		ok(tv->viewStartMs() < mid, "and dragging right moves back");
		ok(std::llabs(tv->viewStartMs() - before) < 2,
		   "an equal drag back lands where it started");
		tv->deleteLater();
	}

	std::printf("\n-- the instant you grabbed stays under the pointer --\n");
	{
		// The actual promise, and the thing a per-move delta gets subtly wrong.
		// Checked over MANY small moves, because that is where accumulated
		// rounding would show and a single big move would not.
		TimelineView *tv = build();
		tv->setViewStartForTest(15000);
		QApplication::processEvents();

		const QPoint from(700, 150);
		const qint64 grabbed = tv->timeAtXForTest(from.x());
		press(tv, from, Qt::MiddleButton);
		for (int x = 700; x >= 300; x -= 10) {
			moveTo(tv, QPoint(x, 150), Qt::MiddleButton);
			QApplication::processEvents();
		}
		const QPoint end(300, 150);
		release(tv, end, Qt::MiddleButton);
		QApplication::processEvents();

		const qint64 nowUnder = tv->timeAtXForTest(end.x());
		std::printf("     grabbed %lld ms; after 40 moves it sits under %lld ms\n",
			    (long long)grabbed, (long long)nowUnder);
		// A couple of ms of rounding across forty moves is the pixel grid, not
		// drift. Anything larger means the view is being nudged, not placed.
		ok(std::llabs(nowUnder - grabbed) <= 60,
		   "the grabbed moment is still under the pointer after forty moves");
		tv->deleteLater();
	}

	std::printf("\n-- and it still works zoomed right in, one pixel at a time --\n");
	{
		// Where measuring from the press stops being a nicety. Zoomed to
		// frame level a pixel is a fraction of a millisecond, so a pan that
		// applied each move's delta separately would round every single step to
		// zero and the timeline would not move AT ALL. Measured from the press,
		// the total is what gets rounded, so it moves.
		//
		// The forty-move check above does not catch this: at moderate zoom the
		// per-move version drifts by a few ms and passes.
		TimelineView *tv = build();
		tv->setZoomForTest(400.0); // a fraction of a millisecond per pixel
		tv->setViewStartForTest(20000);
		QApplication::processEvents();
		const qint64 before = tv->viewStartMs();
		press(tv, QPoint(700, 150), Qt::MiddleButton);
		for (int x = 699; x >= 400; --x) {
			moveTo(tv, QPoint(x, 150), Qt::MiddleButton);
			QApplication::processEvents();
		}
		release(tv, QPoint(400, 150), Qt::MiddleButton);
		QApplication::processEvents();
		std::printf("     300 single-pixel moves at 400x zoom: view %lld -> %lld ms\n",
			    (long long)before, (long long)tv->viewStartMs());
		ok(tv->viewStartMs() > before, "the view actually moved");
		// And by roughly the right amount: 300 pixels' worth, not one pixel's.
		const double msPerPx = double(tv->visibleMsForTest()) / 830.0;
		const double want = 300.0 * msPerPx;
		std::printf("     moved %lld ms; 300 px at this zoom is about %.0f ms\n",
			    (long long)(tv->viewStartMs() - before), want);
		ok(double(tv->viewStartMs() - before) > want * 0.7,
		   "by the whole drag, not by a rounded-down fraction of it");
		tv->deleteLater();
	}

	std::printf("\n-- releasing ends it --\n");
	{
		// The release handler returns early for anything that is not the left
		// button. A pan is a MIDDLE-button drag, so without an explicit exit it
		// stays armed and every later mouse-move slides the view.
		TimelineView *tv = build();
		tv->setViewStartForTest(20000);
		QApplication::processEvents();
		press(tv, QPoint(600, 150), Qt::MiddleButton);
		moveTo(tv, QPoint(500, 150), Qt::MiddleButton);
		release(tv, QPoint(500, 150), Qt::MiddleButton);
		QApplication::processEvents();
		const qint64 after = tv->viewStartMs();

		// Now just move the mouse about, no buttons down.
		for (int x = 500; x <= 800; x += 50) {
			moveTo(tv, QPoint(x, 150), Qt::NoButton);
			QApplication::processEvents();
		}
		std::printf("     view %lld ms, then %lld ms after moving with no button held\n",
			    (long long)after, (long long)tv->viewStartMs());
		ok(tv->viewStartMs() == after, "moving the mouse afterwards does not slide the view");
		tv->deleteLater();
	}

	std::printf("\n-- it cannot be dragged off the ends --\n");
	{
		TimelineView *tv = build();
		tv->setViewStartForTest(0);
		QApplication::processEvents();
		press(tv, QPoint(200, 150), Qt::MiddleButton);
		moveTo(tv, QPoint(880, 150), Qt::MiddleButton); // hard right: before zero
		release(tv, QPoint(880, 150), Qt::MiddleButton);
		QApplication::processEvents();
		std::printf("     after dragging past the start: %lld ms\n",
			    (long long)tv->viewStartMs());
		ok(tv->viewStartMs() == 0, "it stops at the beginning rather than going negative");
		tv->deleteLater();
	}

	std::printf("\n-- and the gestures that already worked still do --\n");
	{
		TimelineView *tv = build();
		tv->setViewStartForTest(0);
		QApplication::processEvents();

		// Empty space below the lanes: a plain left drag still scrubs.
		const qint64 view0 = tv->viewStartMs();
		press(tv, QPoint(400, 280), Qt::LeftButton);
		moveTo(tv, QPoint(500, 280), Qt::LeftButton);
		release(tv, QPoint(500, 280), Qt::LeftButton);
		QApplication::processEvents();
		std::printf("     after a plain left drag on empty space: playhead %lld ms, view "
			    "%lld ms\n",
			    (long long)tv->playhead(), (long long)tv->viewStartMs());
		ok(tv->playhead() > 0, "a plain left drag still moves the playhead");
		ok(tv->viewStartMs() == view0, "and does NOT pan");

		// A clip still drags.
		tv->selectClip(0, 0);
		QApplication::processEvents();
		const qint64 start0 = tv->model().tracks[0].clips[0].outStartMs;
		const QRect r = tv->clipRectForTest(0, 0);
		const QPoint on(r.center());
		press(tv, on, Qt::LeftButton);
		for (int i = 1; i <= 5; ++i)
			moveTo(tv, QPoint(on.x() + i * 12, on.y()), Qt::LeftButton);
		release(tv, QPoint(on.x() + 60, on.y()), Qt::LeftButton);
		QApplication::processEvents();
		std::printf("     clip start %lld -> %lld ms\n", (long long)start0,
			    (long long)tv->model().tracks[0].clips[0].outStartMs);
		ok(tv->model().tracks[0].clips[0].outStartMs != start0,
		   "and a plain left drag on a clip still moves the clip");
		tv->deleteLater();
	}

	std::printf("\n-- a grab takes over wheel easing and keeps its original anchor --\n");
	{
		TimelineView *tv = build();
		tv->setViewStartForTest(20000);
		QWheelEvent wheel(QPointF(500, 150), tv->mapToGlobal(QPoint(500, 150)), QPoint(),
			QPoint(0, -120), Qt::NoButton, Qt::ShiftModifier, Qt::NoScrollPhase, false);
		QApplication::sendEvent(tv, &wheel);
		bool wasEasing = false;
		for (QTimer *timer : tv->findChildren<QTimer *>())
			wasEasing = wasEasing || timer->isActive();
		ok(wasEasing, "a wheel gesture really started smooth view movement");
		const qint64 grabbed = tv->timeAtXForTest(600);
		press(tv, QPoint(600, 150), Qt::MiddleButton);
		bool stillEasing = false;
		for (QTimer *timer : tv->findChildren<QTimer *>())
			stillEasing = stillEasing || timer->isActive();
		ok(!stillEasing, "middle drag stops the old wheel animation immediately");
		moveTo(tv, QPoint(550, 150), Qt::MiddleButton);
		// Chording another button cannot reset where this grab began.
		QMouseEvent extra(QEvent::MouseButtonPress, QPointF(550, 150),
			tv->mapToGlobal(QPoint(550, 150)), Qt::RightButton,
			Qt::MiddleButton | Qt::RightButton, Qt::NoModifier);
		QApplication::sendEvent(tv, &extra);
		QMouseEvent unrelated(QEvent::MouseButtonRelease, QPointF(550, 150),
			tv->mapToGlobal(QPoint(550, 150)), Qt::RightButton, Qt::MiddleButton, Qt::NoModifier);
		QApplication::sendEvent(tv, &unrelated);
		moveTo(tv, QPoint(400, 150), Qt::MiddleButton);
		ok(std::llabs(tv->timeAtXForTest(400) - grabbed) <= 2,
		   "unrelated button presses and releases do not restart or end the grab");
		const qint64 held = tv->viewStartMs();
		QElapsedTimer clock;
		clock.start();
		while (clock.elapsed() < 60) {
			QApplication::processEvents();
			QThread::msleep(2);
		}
		ok(tv->viewStartMs() == held, "old easing cannot drag the held view back later");
		moveTo(tv, QPoint(250, 150), Qt::NoButton); // a lost release, not a drag
		ok(tv->viewStartMs() == held && tv->cursor().shape() != Qt::ClosedHandCursor,
		   "losing the held middle button ends the grab without one final unwanted pan");
		moveTo(tv, QPoint(150, 150), Qt::NoButton);
		ok(tv->viewStartMs() == held, "buttonless movement stays out of pan mode afterwards");
		tv->deleteLater();
	}

	std::printf("\n-- panning from headers or a marker never changes the edit --\n");
	{
		TimelineView *tv = build();
		tv->setViewStartForTest(20000);
		tv->selectClip(0, 0);
		tv->setPlayhead(12345);
		const TimelineModel original = tv->model();
		int edits = 0, selections = 0, scrubs = 0;
		QObject::connect(tv, &TimelineView::clipsChanged, [&]() { ++edits; });
		QObject::connect(tv, &TimelineView::selectionChanged, [&](int, int) { ++selections; });
		QObject::connect(tv, &TimelineView::scrub, [&](qint64) { ++scrubs; });
		const qint64 before = tv->viewStartMs();
		press(tv, QPoint(35, 100), Qt::MiddleButton);
		moveTo(tv, QPoint(-80, 100), Qt::MiddleButton);
		release(tv, QPoint(-80, 100), Qt::MiddleButton);
		ok(tv->viewStartMs() > before, "the fixed track-header gutter can also start a middle grab");
		ok(tv->model() == original && tv->selectedTrack() == 0 && tv->selectedClip() == 0 &&
		   tv->playhead() == 12345 && edits == 0 && selections == 0 && scrubs == 0,
		   "middle navigation changes no clip, selection, playhead or edit/scrub signal");
		tv->addMarker(25000, QStringLiteral("Keep selected"));
		tv->setViewStartForTest(22000);
		const QPoint marker = tv->markerHandleRectForTest(25000).center();
		const TimelineModel withMarker = tv->model();
		press(tv, marker, Qt::MiddleButton);
		moveTo(tv, marker - QPoint(40, 0), Qt::MiddleButton);
		release(tv, marker - QPoint(40, 0), Qt::MiddleButton);
		ok(tv->model() == withMarker && tv->playhead() == 12345,
		   "a marker under a middle grab is neither moved nor scrubbed");
		tv->deleteSelected();
		ok(tv->model().markers.isEmpty() && tv->model().tracks[0].clips.size() == 6,
		   "panning preserves the selected marker rather than reverting to clip selection");
		tv->deleteLater();
	}

	std::printf("\n-- interrupted grabs restore cursor ownership and end navigation --\n");
	{
		QWidget parent;
		parent.resize(900, 300);
		parent.setCursor(Qt::CrossCursor);
		parent.show();
		TimelineView *tv = build(&parent);
		QPixmap image(16, 16);
		image.fill(Qt::green);
		const QCursor custom(image, 3, 4);
		for (QEvent::Type type : {QEvent::UngrabMouse, QEvent::Hide, QEvent::WindowDeactivate}) {
			tv->setViewStartForTest(20000);
			const bool explicitCursor = type != QEvent::Hide;
			if (explicitCursor)
				tv->setCursor(custom);
			else
				tv->unsetCursor();
			press(tv, QPoint(600, 150), Qt::MiddleButton);
			moveTo(tv, QPoint(500, 150), Qt::MiddleButton);
			const qint64 held = tv->viewStartMs();
			QEvent interrupted(type);
			QApplication::sendEvent(tv, &interrupted);
			const bool restored = explicitCursor
				? tv->testAttribute(Qt::WA_SetCursor) && tv->cursor().shape() == custom.shape() &&
				  tv->cursor().hotSpot() == custom.hotSpot() &&
				  tv->cursor().pixmap().toImage() == custom.pixmap().toImage()
				: !tv->testAttribute(Qt::WA_SetCursor) && tv->cursor().shape() == Qt::CrossCursor;
			ok(restored, "ungrab, hide and deactivation restore the original cursor and its ownership");
			moveTo(tv, QPoint(400, 150), Qt::MiddleButton);
			ok(tv->viewStartMs() == held, "an interrupted grab cannot resume on held-button movement");
			release(tv, QPoint(400, 150), Qt::MiddleButton);
		}
		tv->setCursor(custom);
		press(tv, QPoint(600, 150), Qt::MiddleButton);
		release(tv, QPoint(600, 150), Qt::MiddleButton);
		ok(tv->testAttribute(Qt::WA_SetCursor) && tv->cursor().hotSpot() == custom.hotSpot() &&
		   tv->cursor().pixmap().toImage() == custom.pixmap().toImage(),
		   "ordinary release also restores an explicit custom cursor");
		tv->unsetCursor();
		press(tv, QPoint(600, 150), Qt::MiddleButton);
		moveTo(tv, QPoint(500, 150), Qt::NoButton);
		ok(!tv->testAttribute(Qt::WA_SetCursor) && tv->cursor().shape() == Qt::CrossCursor,
		   "a lost middle button restores the inherited parent cursor");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
