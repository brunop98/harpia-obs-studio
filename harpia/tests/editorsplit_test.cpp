// The divider between the preview and the editing area moves, in Full editing,
// however many lanes the project has.
//
// The timeline used to be exactly as tall as all its lanes, as a MINIMUM, so
// the editing area could never be shorter than its track stack: with a few
// lanes the divider sat with both sides at their minimums and would not move,
// and the preview could not be made any bigger. The lanes now scroll under a
// fixed ruler instead (a scrollbar, and Alt+wheel), so the minimum is the
// ruler and one lane.
//
//   editorsplit_test <workdir>   (run_avsync.sh generates av_red.mp4)
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTest>
#include <QWheelEvent>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *what)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", what);
	if (!c)
		++failures;
}

static void settle(int ms)
{
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < ms)
		QApplication::processEvents(QEventLoop::AllEvents, 20);
}

static QSplitter *previewSplit(QWidget *w)
{
	for (QSplitter *s : w->findChildren<QSplitter *>())
		if (s->orientation() == Qt::Vertical && s->count() == 2)
			return s;
	return nullptr;
}

// A real drag on the handle: press, move in steps, release.
static void dragHandle(QSplitter *s, int dy)
{
	QSplitterHandle *h = s->handle(1);
	const QPoint c = h->rect().center();
	QTest::mousePress(h, Qt::LeftButton, {}, c);
	for (int i = 1; i <= 10; ++i) {
		const QPointF at(c + QPoint(0, dy * i / 10));
		QMouseEvent mv(QEvent::MouseMove, at, h->mapToGlobal(at), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
		QApplication::sendEvent(h, &mv);
	}
	QTest::mouseRelease(h, Qt::LeftButton, {}, c + QPoint(0, dy));
	settle(100);
}

static void withLanes(TimelineView *tv, int n)
{
	TimelineModel m = tv->model();
	while (m.tracks.size() < n) {
		TlTrack t;
		t.kind = m.tracks.size() % 2 ? TlTrack::Kind::Audio : TlTrack::Kind::Video;
		m.tracks.append(t);
	}
	while (m.tracks.size() > n)
		m.tracks.removeLast();
	tv->setModelAndCommit(m);
	settle(200);
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	const QString src = QString::fromLocal8Bit(argc > 1 ? argv[1] : ".") + QStringLiteral("/av_red.mp4");

	VideoEditorWindow w(src);
	w.setWindowState(Qt::WindowNoState);
	w.resize(1200, 760);
	w.show();
	settle(400);
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->text() == QStringLiteral("Full Editing")) {
			b->click();
			settle(300);
		}
	TimelineView *tv = w.findChild<TimelineView *>();
	QSplitter *split = previewSplit(&w);
	ok(tv && tv->isVisible() && split, "Full editing, with its timeline under the preview");
	if (!tv || !split)
		return 1;

	std::printf("\n-- the timeline's minimum does not grow with its lanes --\n");
	withLanes(tv, 2);
	const int min2 = static_cast<QWidget *>(tv)->minimumSizeHint().height(), pref2 = static_cast<QWidget *>(tv)->sizeHint().height();
	withLanes(tv, 10);
	const int min10 = static_cast<QWidget *>(tv)->minimumSizeHint().height(), pref10 = static_cast<QWidget *>(tv)->sizeHint().height();
	std::printf("     2 lanes: min %d, preferred %d;  10 lanes: min %d, preferred %d\n", min2, pref2, min10,
		    pref10);
	ok(min10 == min2, "ten lanes need no more minimum height than two");
	ok(pref10 > pref2, "but still ask for room for all of them");

	std::printf("\n-- the divider moves both ways --\n");
	withLanes(tv, 6);
	dragHandle(split, -150);
	const QList<int> up = split->sizes();
	dragHandle(split, 400);
	const QList<int> down = split->sizes();
	std::printf("     after dragging up %d/%d, then down %d/%d\n", up[0], up[1], down[0], down[1]);
	ok(down[0] > up[0] + 150, "dragging down makes the preview bigger");
	ok(down[1] < static_cast<QWidget *>(tv)->sizeHint().height(), "shorter than the six lanes: they are not a floor any more");
	dragHandle(split, -200);
	ok(split->sizes()[0] < down[0] - 100, "and dragging up makes it smaller again");
	dragHandle(split, 400); // short again, for the scrolling below

	std::printf("\n-- lanes that do not fit scroll under the ruler --\n");
	QScrollBar *bar = tv->findChild<QScrollBar *>();
	ok(bar && bar->isVisible() && bar->maximum() > 0, "a scrollbar appears for the lanes that do not fit");
	const int topBefore = tv->clipRectForTest(0, 0).top();
	const QPointF mid(tv->width() / 2.0, tv->height() - 10.0);
	QWheelEvent wheel(mid, tv->mapToGlobal(mid), QPoint(), QPoint(0, -360), Qt::NoButton, Qt::AltModifier,
			  Qt::NoScrollPhase, false);
	QApplication::sendEvent(tv, &wheel);
	settle(100);
	const int topAfter = tv->clipRectForTest(0, 0).top();
	std::printf("     scrollbar %d of %d; the first clip's top %d -> %d (lanes start at %d)\n", bar ? bar->value() : -1,
		    bar ? bar->maximum() : -1, topBefore, topAfter, tv->contentRectForTest().top());
	ok(bar && bar->value() > 0 && topAfter < topBefore, "Alt+wheel scrolls the lanes, and the clips with them");
	const qint64 playhead = tv->playhead();
	const QRect hidden = tv->clipRectForTest(0, 0);
	if (hidden.bottom() < tv->contentRectForTest().top()) {
		// The clip is under the ruler now: a click there is the ruler's.
		const QPoint at(hidden.center().x(), tv->contentRectForTest().top() - 8);
		QTest::mouseClick(tv, Qt::LeftButton, {}, at);
		settle(50);
		ok(tv->selectedTrack() != 0 || tv->playhead() != playhead,
		   "a click on the ruler over a scrolled-away clip scrubs, it does not grab the clip");
	}
	bar->setValue(0);
	settle(50);
	ok(tv->clipRectForTest(0, 0).top() == topBefore, "and back to the top puts them where they were");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
