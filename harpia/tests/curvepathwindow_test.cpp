// Curved motion paths in the real editor window.
//
//   * "Curve path" (Animation section) curves every position key, as one undo
//     step, and the preview then draws the curve, not straight lines.
//   * The key at the playhead shows its handles where a curve meets it.
//   * Dragging a handle with the mouse makes the key's handles its own and
//     keeps them mirrored; Alt-drag moves one alone.
//   * Ctrl+Z undoes a handle drag.
//   * "Straight path" puts the lines back.
#include "editor/EditorWidgets.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMouseEvent>
#include <QPushButton>
#include <QShortcut>
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

static void mouse(QWidget *w, QEvent::Type t, QPointF p, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
	const Qt::MouseButtons held = t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton);
	QMouseEvent e(t, p, w->mapToGlobal(p), t == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held, mods);
	QApplication::sendEvent(w, &e);
}

static void drag(QWidget *w, QPointF from, QPointF to, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
	mouse(w, QEvent::MouseButtonPress, from, mods);
	for (int k = 1; k <= 5; ++k)
		mouse(w, QEvent::MouseMove, from + (to - from) * (k / 5.0), mods);
	mouse(w, QEvent::MouseButtonRelease, to, mods);
	settle(150);
}

static QPushButton *button(QWidget &w, const QString &text)
{
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->isVisible() && b->text() == text)
			return b;
	return nullptr;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	const QString media = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
	const QString video = media + QStringLiteral("/av_red.mp4");
	if (!QFile::exists(video)) {
		std::printf("missing %s\n", qPrintable(video));
		return 2;
	}
	VideoEditorWindow w(video);
	w.resize(1100, 800);
	w.show();
	settle(300);
	if (QPushButton *b = button(w, QStringLiteral("Full Editing")))
		b->click();
	settle(300);
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->isCheckable() && b->text() == QStringLiteral("Inspector") && !b->isChecked()) {
			b->click();
			settle(300);
			break;
		}
	TimelineView *tv = w.findChild<TimelineView *>();
	PreviewCanvas *canvas = w.findChild<PreviewCanvas *>();
	ok(tv && canvas, "in Full editing, with the preview");
	if (!tv || !canvas)
		return 1;

	// One clip with three position keys: left-low, top-middle, right-low.
	TimelineModel m = tv->model();
	int src = -1;
	for (const TlTrack &t : m.tracks)
		for (const TlClip &c : t.clips)
			src = c.sourceId;
	TlTrack t;
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = src;
	c.srcEndMs = 3000;
	c.scale = 0.4;
	const double xs[] = {0.2, 0.5, 0.8}, ys[] = {0.75, 0.25, 0.75};
	for (int i = 0; i < 3; ++i) {
		TlKeyframe k;
		k.tMs = i * 1000;
		k.tf.posX = xs[i];
		k.tf.posY = ys[i];
		k.tf.scale = 0.4;
		for (int l = 0; l < kTlLaneCount; ++l)
			k.channel(l).on = l == TlLanePos;
		c.keys.append(k);
	}
	t.clips = {c};
	m.tracks = {t};
	tv->setModelAndCommit(m);
	settle(300);
	tv->zoomToFit();
	settle(50);
	{
		const QPoint at = tv->clipRectForTest(0, 0).center();
		QMouseEvent press(QEvent::MouseButtonPress, QPointF(at), tv->mapToGlobal(QPointF(at)), Qt::LeftButton,
				  Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(tv, &press);
		QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at), tv->mapToGlobal(QPointF(at)), Qt::LeftButton,
				    Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(tv, &release);
		settle(300);
	}
	ok(tv->selectedClipPtr() != nullptr, "the clip is selected");
	// The playhead onto the middle key, as pressing its dot does.
	emit canvas->pathKeyPressed(1);
	settle(300);
	const auto &path = canvas->motionPathForTest();
	ok(path.on && path.keys.size() == 3, "the path is drawn with its three keys");
	ok(path.line.size() <= 3, "straight to begin with: just the keys");
	ok(!path.showIn && !path.showOut, "and no handles");

	std::printf("\n-- Curve path --\n");
	QShortcut *undo = nullptr;
	for (QShortcut *s : w.findChildren<QShortcut *>())
		if (s->key() == QKeySequence(Qt::CTRL | Qt::Key_Z) && s->isEnabled())
			undo = s;
	QPushButton *curve = button(w, QStringLiteral("Curve path"));
	ok(curve != nullptr, "the Animation section has Curve path");
	if (!curve)
		return 1;
	curve->click();
	settle(300);
	const auto keysNow = [&]() { return tv->selectedClipPtr()->keys; };
	bool allCurved = true;
	for (const TlKeyframe &k : keysNow())
		allCurved = allCurved && k.curvedPath;
	ok(allCurved, "every position key is curved");
	std::printf("     the drawn line now has %d points\n", int(canvas->motionPathForTest().line.size()));
	ok(canvas->motionPathForTest().line.size() > 20, "the preview draws the curve");
	ok(canvas->motionPathForTest().showIn && canvas->motionPathForTest().showOut,
	   "the middle key shows both handles");

	std::printf("\n-- dragging a handle --\n");
	{
		const QPointF from = canvas->pathHandlePxForTest(true);
		drag(canvas, from, from + QPointF(0, -40));
		const TlKeyframe k = keysNow()[1];
		std::printf("     out (%.3f, %.3f)  in (%.3f, %.3f)\n", k.outX, k.outY, k.inX, k.inY);
		ok(k.handlesManual, "the key's handles are its own now");
		ok(k.outY < -0.01, "the out handle moved up");
		ok(std::abs(k.inX + k.outX) < 1e-9 && std::abs(k.inY + k.outY) < 1e-9, "the in handle mirrors it");
	}
	{
		const TlKeyframe before = keysNow()[1];
		const QPointF from = canvas->pathHandlePxForTest(false);
		drag(canvas, from, from + QPointF(0, 50), Qt::AltModifier);
		const TlKeyframe k = keysNow()[1];
		ok(k.handlesBroken, "Alt-drag breaks the handles");
		ok(std::abs(k.outX - before.outX) < 1e-9 && std::abs(k.outY - before.outY) < 1e-9,
		   "and the out handle stays where it was");
		ok(k.inY > before.inY + 0.01, "while the in handle moves");
	}
	if (undo) {
		const TlKeyframe before = keysNow()[1];
		emit undo->activated();
		settle(300);
		const TlKeyframe k = keysNow()[1];
		ok(!k.handlesBroken && std::abs(k.inY - before.inY) > 0.01, "Ctrl+Z undoes the Alt-drag");
	}

	std::printf("\n-- Straight path --\n");
	if (QPushButton *straight = button(w, QStringLiteral("Straight path"))) {
		straight->click();
		settle(300);
		bool none = true;
		for (const TlKeyframe &k : keysNow())
			none = none && !k.curvedPath;
		ok(none, "every key straight again");
		ok(canvas->motionPathForTest().line.size() <= 3, "straight lines on the preview");
	} else {
		ok(false, "the Animation section has Straight path");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
