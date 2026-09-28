// The motion path on the preview. The pure half: which keys are points on the
// path (only those that pin position), the per-frame dots that make speed
// visible, and turning a click on the path back into a time. The widget half:
// a press on a key dot picks it, a drag moves it, a double-click or Alt+click
// on the line asks for a key there, and a right-click opens its menu -- each as
// a signal, checked with real mouse events. And Delete, after picking a key,
// deletes that key rather than the clip.
#include "editor/DeleteRouting.hpp"
#include "editor/EditorWidgets.hpp"
#include "editor/timeline/MotionPath.hpp"

#include <QApplication>
#include <QSignalSpy>
#include <QTest>

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

static TlKeyframe key(qint64 t, double x, double y, int lanes = 0xF, TlEase ease = TlEase::Linear)
{
	TlKeyframe k;
	k.tMs = t;
	k.tf.posX = x;
	k.tf.posY = y;
	for (int l = 0; l < kTlLaneCount; ++l) {
		k.channel(l).on = (lanes & (1 << l)) != 0;
		k.channel(l).ease = ease;
	}
	return k;
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);

	TlClip c;
	c.outStartMs = 5000;
	c.srcEndMs = 4000;
	c.keys = {key(0, 0.2, 0.5), key(500, 0.9, 0.9, 1 << TlLaneScale), key(1000, 0.8, 0.5)};

	std::printf("\n-- the path --\n");
	{
		const MotionPath mp = buildMotionPath(c, 30.0);
		ok(mp.keys.size() == 2 && mp.keys[0].keyIndex == 0 && mp.keys[1].keyIndex == 2,
		   "a key that pins only scale is not a point on the path");
		ok(mp.drawable() && std::abs(mp.keys[1].pos.x() - 0.8) < 1e-9, "the path runs from the first to the last position key");
		ok(mp.frames.size() >= 30 && mp.frames.size() <= 32, "about a dot per frame over one second at 30 fps");
		ok(std::abs(mp.frames[15].x() - 0.5) < 0.02 && std::abs(mp.frames[15].y() - 0.5) < 1e-9,
		   "linear: halfway in time is halfway along the line");

		TlClip eased = c;
		eased.keys[0].channel(TlLanePos).ease = TlEase::EaseInOut;
		const MotionPath me = buildMotionPath(eased, 30.0);
		const double startGap = std::abs(me.frames[1].x() - me.frames[0].x());
		const double midGap = std::abs(me.frames[16].x() - me.frames[15].x());
		ok(startGap < midGap / 3.0, "eased: dots bunch up at the start, where it is slow");
		for (const QPointF &f : me.frames)
			if (std::abs(f.y() - 0.5) > 1e-9) {
				ok(false, "the path stays straight whatever the ease");
				break;
			}

		TlClip one = c;
		one.keys = {key(0, 0.5, 0.5)};
		ok(!buildMotionPath(one, 30.0).drawable(), "one position key is not a path");
	}

	std::printf("\n-- hits and times --\n");
	{
		const QVector<QPointF> px = {{100, 100}, {300, 100}, {300, 300}};
		ok(motionPathKeyAt(px, {104, 98}, 9) == 0 && motionPathKeyAt(px, {150, 100}, 9) == -1,
		   "a key dot is hit within its radius, not along the line");
		ok(motionPathKeyAt({{100, 100}, {106, 100}}, {105, 100}, 9) == 1, "two dots close together: the nearer one");
		ok(motionPathHit(px, {200, 104}, 6) && motionPathHit(px, {303, 200}, 6) && !motionPathHit(px, {200, 200}, 6),
		   "the line is hit near either segment, not off it");

		const qint64 t = motionPathTimeAt(c, QPointF(0.5, 0.5), QSizeF(1000, 500), 10.0);
		ok(std::llabs(t - 500) <= 10, "a click halfway along a linear segment is halfway in time");
		ok(motionPathTimeAt(c, QPointF(0.5, 0.9), QSizeF(1000, 500), 10.0) == -1, "a click far from the path is not on it");
	}

	std::printf("\n-- the preview --\n");
	{
		PreviewCanvas cv;
		cv.resize(1000, 500);
		cv.setVideoSize(1000, 500);
		cv.show();
		QTest::qWaitForWindowExposed(&cv);
		PreviewCanvas::PathDraw pd;
		pd.on = true;
		pd.keys = {{0.2, 0.5}, {0.8, 0.5}};
		pd.keyIds = {0, 2};
		pd.frames = {{0.2, 0.5}, {0.5, 0.5}, {0.8, 0.5}};
		cv.setMotionPath(pd);
		ok(cv.displaySize() == QSizeF(1000, 500), "the picture fills the canvas here");

		QSignalSpy pressed(&cv, &PreviewCanvas::pathKeyPressed);
		QSignalSpy dragged(&cv, &PreviewCanvas::pathKeyDragged);
		QSignalSpy finished(&cv, &PreviewCanvas::pathKeyDragFinished);
		QSignalSpy add(&cv, &PreviewCanvas::pathAddRequested);
		QSignalSpy menu(&cv, &PreviewCanvas::pathKeyMenuRequested);

		QTest::mouseClick(&cv, Qt::LeftButton, Qt::NoModifier, QPoint(802, 251));
		ok(pressed.count() == 1 && pressed.first().at(0).toInt() == 2, "clicking the second dot picks key #2");
		ok(dragged.isEmpty() && finished.isEmpty(), "a click is not a drag");

		QTest::mousePress(&cv, Qt::LeftButton, Qt::NoModifier, QPoint(200, 250));
		QMouseEvent mv(QEvent::MouseMove, QPointF(300, 200), QPointF(300, 200), cv.mapToGlobal(QPointF(300, 200)),
			       Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(&cv, &mv);
		QTest::mouseRelease(&cv, Qt::LeftButton, Qt::NoModifier, QPoint(300, 200));
		ok(!dragged.isEmpty() && dragged.last().at(0).toInt() == 0 &&
			   std::abs(dragged.last().at(1).toDouble() - 0.3) < 1e-6 &&
			   std::abs(dragged.last().at(2).toDouble() - 0.4) < 1e-6,
		   "dragging the first dot 100 px right, 50 up moves key #0 to (0.3, 0.4)");
		ok(finished.count() == 1, "and ends as one gesture");

		pd.keys[0] = {0.2, 0.5}; // the model's answer, as the window would send it
		cv.setMotionPath(pd);
		QTest::mouseDClick(&cv, Qt::LeftButton, Qt::NoModifier, QPoint(500, 252));
		ok(add.count() == 1 && std::abs(add.first().at(0).toDouble() - 0.5) < 0.01,
		   "double-clicking the line asks for a key where it was clicked");
		QTest::mouseClick(&cv, Qt::LeftButton, Qt::AltModifier, QPoint(400, 249));
		ok(add.count() == 2, "and so does Alt+click");
		QTest::mouseClick(&cv, Qt::LeftButton, Qt::AltModifier, QPoint(400, 350));
		ok(add.count() == 2, "but not off the line");

		QTest::mouseClick(&cv, Qt::RightButton, Qt::NoModifier, QPoint(200, 250));
		ok(menu.count() == 1 && menu.first().at(0).toInt() == 0, "right-clicking a dot asks for its menu");

		pd.on = false;
		cv.setMotionPath(pd);
		pressed.clear();
		QTest::mouseClick(&cv, Qt::LeftButton, Qt::NoModifier, QPoint(802, 251));
		ok(pressed.isEmpty(), "with no path shown, nothing is picked");
	}

	std::printf("\n-- Delete after picking a key --\n");
	{
		DeleteContext d;
		d.fullEdit = true;
		d.timelineHasSel = true;
		d.pathKeyPicked = true;
		ok(deleteTargetFor(d) == DeleteTarget::PathKeyframe, "deletes the key, not the selected clip");
		d.pathKeyPicked = false;
		ok(deleteTargetFor(d) == DeleteTarget::TimelineClips, "otherwise the clip, as before");
		d.pathKeyPicked = true;
		d.editingText = true;
		ok(deleteTargetFor(d) == DeleteTarget::TextCursor, "and typing still wins");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
