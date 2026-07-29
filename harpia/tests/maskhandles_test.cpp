// Dragging a Mask component's shape on the picture.
//
// Before this, a mask could only be placed by typing five numbers into the
// Inspector and watching the preview to see where they landed. The grips make
// it direct — and put the canvas in the position of having to place a shape
// that is stored in the CLIP's normalised space onto a clip that is itself
// moved, zoomed and possibly turned.
//
// That composition is the thing most likely to be wrong, and it is invisible in
// a screenshot of an untouched clip, because every mistake in it looks correct
// while both rotations are zero:
//
//   - the two rotations applied in the wrong order, so the grips leave the
//     shape the moment either the clip or the mask is turned;
//   - the drag read in screen axes rather than the clip's, so on a turned clip
//     the shape runs off at an angle to the pointer;
//   - the size measured against the LIVE pose each mouse-move instead of the
//     pose at the press, which compounds;
//   - a size grip that resizes about the centre rather than holding the far
//     edge, so the whole shape creeps while you drag one side;
//   - a whole drag landing as thirty undo entries rather than one.
#include "editor/EditorWidgets.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/component/BuiltinComponents.hpp"
#include "editor/component/ComponentRegistry.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalSpy>
#include <QThread>

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
	while (t.elapsed() < ms) {
		QApplication::processEvents();
		QThread::msleep(5);
	}
}
static QPushButton *button(QWidget *w, const QString &text)
{
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (b->text() == text)
			return b;
	return nullptr;
}

// A real press-move-release, the way the window system delivers one.
static void dragMouse(QWidget *w, QPoint from, QPoint to, int steps = 6)
{
	QMouseEvent press(QEvent::MouseButtonPress, QPointF(from), w->mapToGlobal(from),
			  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(w, &press);
	for (int i = 1; i <= steps; ++i) {
		const QPoint p(from.x() + (to.x() - from.x()) * i / steps,
			       from.y() + (to.y() - from.y()) * i / steps);
		QMouseEvent move(QEvent::MouseMove, QPointF(p), w->mapToGlobal(p), Qt::NoButton,
				 Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(w, &move);
		QApplication::processEvents();
	}
	QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(to), w->mapToGlobal(to), Qt::LeftButton,
			Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(w, &rel);
	QApplication::processEvents();
}

// The canvas on its own, with a clip box handed to it directly. Everything
// about the grips' placement is decided here, and driving it without a window
// means the geometry can be stated exactly rather than inferred.
static const int kVw = 400, kVh = 400; // square, so a 90-degree turn is easy to state
static PreviewCanvas *bareCanvas()
{
	auto *pc = new PreviewCanvas;
	pc->resize(600, 600);
	QImage f(kVw, kVh, QImage::Format_RGBA8888);
	f.fill(Qt::darkGray);
	pc->setFrame(f);
	pc->setVideoSize(kVw, kVh);
	pc->setTransformMode(true);
	pc->show();
	QApplication::processEvents();
	return pc;
}

static PreviewCanvas::MaskEdit basicMask()
{
	PreviewCanvas::MaskEdit m;
	m.on = true;
	m.shape = SpotShape::Rect;
	m.cx = 0.5;
	m.cy = 0.5;
	m.w = 0.5;
	m.h = 0.5;
	m.rotation = 0.0;
	return m;
}

static QPointF maskCentreOf(const QVector<QPointF> &h)
{
	return (h[0] + h[7]) / 2.0; // TL and BR average to the shape's centre
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	registerBuiltinComponents(ComponentRegistry::instance());

	// The clip fills the whole 400x400 frame, so widget pixels and clip
	// fractions relate by one number and the expected values below are stated,
	// not derived from the code under test.
	const QRectF wholeClip(0, 0, kVw, kVh);

	std::printf("\n-- with nothing turned, the grips are where arithmetic says --\n");
	{
		PreviewCanvas *pc = bareCanvas();
		pc->setTransformBox(wholeClip, 0.0);
		pc->setMaskEdit(basicMask());
		const QVector<QPointF> h = pc->maskHandlePointsForTest();
		ok(h.size() == 9, "eight size grips and a rotate knob");
		if (h.size() == 9) {
			// The clip is drawn 600x600 (square frame in a square widget), so a
			// half-width of 0.25 of the clip is 150 widget px from the centre.
			const QPointF c = maskCentreOf(h);
			std::printf("     centre (%.1f,%.1f), TL (%.1f,%.1f), TR (%.1f,%.1f)\n", c.x(),
				    c.y(), h[0].x(), h[0].y(), h[2].x(), h[2].y());
			ok(std::abs(c.x() - 300) < 0.5 && std::abs(c.y() - 300) < 0.5,
			   "a centred mask has its centre at the middle of the clip");
			ok(std::abs(h[0].x() - 150) < 0.5 && std::abs(h[0].y() - 150) < 0.5,
			   "and its top-left grip a quarter of the clip away, as w=0.5 means");
			ok(std::abs(h[8].x() - c.x()) < 0.5 && h[8].y() < h[0].y(),
			   "the rotate knob is on a stalk above the top edge");
		}
		pc->deleteLater();
	}

	std::printf("\n-- the mask's own rotation turns the shape, it does not move it --\n");
	{
		// The property that separates the two orders. Rotating a shape about its
		// own centre cannot move that centre; applying the mask's angle about the
		// CLIP's centre instead -- the natural way to get it backwards -- moves it
		// whenever the mask is off-centre, which is most of the time.
		PreviewCanvas *pc = bareCanvas();
		pc->setTransformBox(wholeClip, 0.0);
		PreviewCanvas::MaskEdit m = basicMask();
		m.cx = 0.25; // deliberately off-centre: the case that discriminates
		m.cy = 0.5;
		pc->setMaskEdit(m);
		const QPointF before = maskCentreOf(pc->maskHandlePointsForTest());
		const double r0 = QLineF(before, pc->maskHandlePointsForTest()[0]).length();

		m.rotation = 37.0;
		pc->setMaskEdit(m);
		const QVector<QPointF> h = pc->maskHandlePointsForTest();
		const QPointF after = maskCentreOf(h);
		const double r1 = QLineF(after, h[0]).length();
		std::printf("     centre (%.1f,%.1f) -> (%.1f,%.1f); corner radius %.2f -> %.2f\n",
			    before.x(), before.y(), after.x(), after.y(), r0, r1);
		ok(QLineF(before, after).length() < 0.5,
		   "turning the mask left its centre exactly where it was");
		ok(std::abs(r1 - r0) < 0.5, "and the corner kept its distance, so it really turned");
		// It has to have moved SOMEWHERE, or "unchanged centre" would be free.
		ok(QLineF(h[0], QPointF(0, 0)).length() > 0.0 &&
			   std::abs(h[0].x() - 150.0) > 5.0,
		   "the corner itself did move, so this is not a no-op");
		pc->deleteLater();
	}

	std::printf("\n-- turning the CLIP carries the mask round with it --\n");
	{
		PreviewCanvas *pc = bareCanvas();
		PreviewCanvas::MaskEdit m = basicMask();
		m.cx = 0.2; // 0.3 of the clip left of centre
		m.cy = 0.5;
		m.rotation = 37.0; // and turned, so order errors have something to bite on
		pc->setTransformBox(wholeClip, 0.0);
		pc->setMaskEdit(m);
		const QPointF flat = maskCentreOf(pc->maskHandlePointsForTest());

		pc->setTransformBox(wholeClip, 90.0);
		const QPointF turned = maskCentreOf(pc->maskHandlePointsForTest());
		// Screen axes: y grows downward, so +90 degrees sends (x,y) to (-y,x)
		// about the clip's centre. A point 0.3 of the clip to the LEFT of centre
		// (-180, 0) in a 600px clip therefore lands 180px ABOVE it.
		const QPointF want(300.0, 300.0 - 180.0);
		std::printf("     flat (%.1f,%.1f), clip turned 90 -> (%.1f,%.1f), expected "
			    "(%.1f,%.1f)\n",
			    flat.x(), flat.y(), turned.x(), turned.y(), want.x(), want.y());
		ok(QLineF(turned, want).length() < 1.0,
		   "the shape swung a quarter turn about the clip's centre with the clip");
		pc->deleteLater();
	}

	std::printf("\n-- a circle takes its height from its width, as the renderer does --\n");
	{
		PreviewCanvas *pc = bareCanvas();
		pc->setTransformBox(wholeClip, 0.0);
		PreviewCanvas::MaskEdit m = basicMask();
		m.shape = SpotShape::Circle;
		m.w = 0.5;
		m.h = 0.1; // ignored for a circle -- and if it is not, the grips lie
		pc->setMaskEdit(m);
		const QVector<QPointF> h = pc->maskHandlePointsForTest();
		const QPointF c = maskCentreOf(h);
		std::printf("     half-extents %.1f x %.1f\n", std::abs(h[0].x() - c.x()),
			    std::abs(h[0].y() - c.y()));
		ok(std::abs(std::abs(h[0].y() - c.y()) - std::abs(h[0].x() - c.x())) < 0.5,
		   "the grips sit on a square box, so they sit on the circle");
		pc->deleteLater();
	}

	std::printf("\n-- dragging the body moves it and only moves it --\n");
	{
		PreviewCanvas *pc = bareCanvas();
		pc->setTransformBox(wholeClip, 0.0);
		pc->setMaskEdit(basicMask());
		QSignalSpy poses(pc, &PreviewCanvas::maskPoseChanged);
		QSignalSpy done(pc, &PreviewCanvas::maskEditFinished);

		// 60px right, 30px down on a 600px clip = +0.1, +0.05.
		dragMouse(pc, QPoint(300, 300), QPoint(360, 330));
		const PreviewCanvas::MaskEdit &m = pc->maskEdit();
		std::printf("     centre (%.3f,%.3f), size %.3f x %.3f, %d live updates\n", m.cx, m.cy,
			    m.w, m.h, int(poses.count()));
		ok(std::abs(m.cx - 0.6) < 0.005 && std::abs(m.cy - 0.55) < 0.005,
		   "the centre followed the pointer exactly");
		ok(std::abs(m.w - 0.5) < 1e-9 && std::abs(m.h - 0.5) < 1e-9,
		   "and the size did not change");
		ok(poses.count() >= 3, "the pose was reported live, so the Inspector can follow");
		ok(done.count() == 1, "and the gesture ended once -- one undo step, not six");
		pc->deleteLater();
	}

	std::printf("\n-- a size grip holds the far edge --\n");
	{
		PreviewCanvas *pc = bareCanvas();
		pc->setTransformBox(wholeClip, 0.0);
		pc->setMaskEdit(basicMask());
		const QVector<QPointF> h0 = pc->maskHandlePointsForTest();
		const double leftBefore = h0[0].x(); // the edge that must not move

		// Grab the right-edge grip (index 4) and pull it 60px further right.
		const QPoint grip = h0[4].toPoint();
		dragMouse(pc, grip, QPoint(grip.x() + 60, grip.y()));
		const PreviewCanvas::MaskEdit &m = pc->maskEdit();
		const QVector<QPointF> h1 = pc->maskHandlePointsForTest();
		std::printf("     w %.3f -> %.3f, h %.3f, left edge %.1f -> %.1f\n", 0.5, m.w, m.h,
			    leftBefore, h1[0].x());
		ok(std::abs(m.w - 0.6) < 0.005, "the width grew by exactly the drag");
		ok(std::abs(m.h - 0.5) < 1e-9, "the height was left alone");
		// The centre moved by half the growth; if it had not, the far edge would
		// have walked left and "drag the right edge" would be a lie.
		ok(std::abs(h1[0].x() - leftBefore) < 1.0, "and the left edge stayed exactly put");
		pc->deleteLater();
	}

	std::printf("\n-- the size is measured from the press, not compounded --\n");
	{
		// Applied against the LIVE pose on each of eight mouse-moves rather than
		// the pose at the press, the increments accumulate; a loose "it got
		// bigger" bound would not notice, so the bound is the exact geometry.
		PreviewCanvas *pc = bareCanvas();
		pc->setTransformBox(wholeClip, 0.0);
		pc->setMaskEdit(basicMask());
		const QPoint grip = pc->maskHandlePointsForTest()[4].toPoint();
		dragMouse(pc, grip, QPoint(grip.x() + 120, grip.y()), 8);
		std::printf("     after an 8-step 120px drag: w %.4f (the geometry says 0.7000)\n",
			    pc->maskEdit().w);
		ok(std::abs(pc->maskEdit().w - 0.7) < 0.005,
		   "the width is the press-to-pointer distance, whatever the step count");
		pc->deleteLater();
	}

	std::printf("\n-- on a TURNED clip the drag follows the pointer, not the screen --\n");
	{
		// The clip is a quarter turn round, so dragging straight DOWN the screen
		// is a move along the clip's own -x. Reading the drag in screen axes would
		// change cy here instead of cx.
		PreviewCanvas *pc = bareCanvas();
		pc->setTransformBox(wholeClip, 90.0);
		pc->setMaskEdit(basicMask());
		const QPoint centre = maskCentreOf(pc->maskHandlePointsForTest()).toPoint();
		dragMouse(pc, centre, QPoint(centre.x(), centre.y() + 60));
		const PreviewCanvas::MaskEdit &m = pc->maskEdit();
		std::printf("     centre (%.3f,%.3f) after dragging 60px down a clip turned 90\n",
			    m.cx, m.cy);
		ok(std::abs(m.cx - 0.6) < 0.005, "the shape moved along the clip's own axis");
		ok(std::abs(m.cy - 0.5) < 0.005, "and not down it");
		// And the grips are still under the pointer afterwards, which is the whole
		// promise of a handle.
		const QPointF now = maskCentreOf(pc->maskHandlePointsForTest());
		ok(QLineF(now, QPointF(centre.x(), centre.y() + 60)).length() < 2.0,
		   "the grips ended up under the pointer, where they were dragged to");
		pc->deleteLater();
	}

	std::printf("\n-- the rotate knob turns the shape --\n");
	{
		PreviewCanvas *pc = bareCanvas();
		pc->setTransformBox(wholeClip, 0.0);
		pc->setMaskEdit(basicMask());
		const QVector<QPointF> h = pc->maskHandlePointsForTest();
		const QPointF c = maskCentreOf(h);
		const double r = QLineF(c, h[8]).length();
		// From straight above the centre to straight right of it: a quarter turn.
		dragMouse(pc, h[8].toPoint(), QPoint(int(c.x() + r), int(c.y())));
		std::printf("     rotation 0 -> %.1f degrees\n", pc->maskEdit().rotation);
		ok(std::abs(pc->maskEdit().rotation - 90.0) < 3.0,
		   "swinging the knob a quarter turn turned the shape ninety degrees");
		pc->deleteLater();
	}

	std::printf("\n-- pressing off the shape starts no mask drag --\n");
	{
		PreviewCanvas *pc = bareCanvas();
		pc->setTransformBox(wholeClip, 0.0);
		pc->setMaskEdit(basicMask());
		QSignalSpy poses(pc, &PreviewCanvas::maskPoseChanged);
		// Well outside the shape (which spans 150..450) but inside the clip: this
		// press belongs to the clip's own transform tools, not the mask's.
		dragMouse(pc, QPoint(20, 20), QPoint(90, 60));
		std::printf("     mask pose updates from an off-shape drag: %d\n", int(poses.count()));
		ok(poses.count() == 0, "the mask was not dragged from outside itself");
		ok(std::abs(pc->maskEdit().cx - 0.5) < 1e-9, "and its pose is untouched");
		pc->deleteLater();
	}

	// ---- End to end, through the real window -------------------------------
	std::printf("\n-- and in the editor it writes the component and undoes as one --\n");
	{
		VideoEditorWindow w(QStringLiteral(SRC_MEDIA));
		w.resize(1300, 900);
		w.show();
		QApplication::processEvents();
		if (QPushButton *full = button(&w, QStringLiteral("Full Editing"))) {
			full->click();
			settle(400);
		}
		TimelineView *tv = w.findChild<TimelineView *>();
		PreviewCanvas *pc = w.findChild<PreviewCanvas *>();
		ok(tv && pc, "the editor came up with a preview canvas");
		if (!tv || !pc) {
			std::printf("\nFAILURES\n");
			return 1;
		}

		TimelineModel m;
		TlTrack t;
		t.kind = TlTrack::Kind::Video;
		TlClip c;
		c.type = TlClip::Type::Text;
		c.srcStartMs = 0;
		c.srcEndMs = 4000;
		c.outStartMs = 0;
		c.text.text = QStringLiteral("Mask me");
		ComponentInstance ci;
		ci.typeId = QStringLiteral("harpia.mask");
		ci.instanceId = QStringLiteral("m1");
		ci.props[QStringLiteral("centreX")] = 0.5;
		ci.props[QStringLiteral("centreY")] = 0.5;
		ci.props[QStringLiteral("width")] = 0.5;
		ci.props[QStringLiteral("height")] = 0.5;
		c.components.append(ci);
		t.clips.append(c);
		m.tracks.append(t);
		tv->setModel(m);
		emit tv->editCommitted(); // a baseline for undo, which setModel does not record
		tv->selectClip(0, 0);
		settle(700);

		ok(pc->maskEdit().on,
		   "selecting a clip that carries a Mask puts its grips on the picture");
		const QVector<QPointF> h = pc->maskHandlePointsForTest();
		ok(h.size() == 9, "nine of them, as everywhere else");
		if (h.size() != 9 || !pc->maskEdit().on) {
			std::printf("\nFAILURES\n");
			return 1;
		}

		const auto propOf = [&](const char *k) {
			return tv->model()
				.tracks[0]
				.clips[0]
				.components[0]
				.props.value(QString::fromLatin1(k))
				.toDouble();
		};
		const double cx0 = propOf("centreX");
		const QPoint centre = maskCentreOf(h).toPoint();
		dragMouse(pc, centre, QPoint(centre.x() + 70, centre.y()));
		settle(600);
		const double cx1 = propOf("centreX");
		std::printf("     component centreX %.3f -> %.3f\n", cx0, cx1);
		ok(std::abs(cx1 - cx0) > 0.02, "the drag wrote the component's own property");

		QMetaObject::invokeMethod(&w, "undo");
		settle(600);
		std::printf("     after one undo: %.3f\n", propOf("centreX"));
		ok(std::abs(propOf("centreX") - cx0) < 1e-6,
		   "and one undo put the whole gesture back");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
