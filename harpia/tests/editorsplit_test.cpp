// The editor's layout: the divider between the preview and the editing area
// moves in Full editing however many lanes the project has, and the Vertical
// layout puts the preview in a tall column of its own.
//
// The timeline used to be exactly as tall as all its lanes, as a MINIMUM, so
// the editing area could never be shorter than its track stack: with a few
// lanes the divider sat with both sides at their minimums and would not move,
// and the preview could not be made any bigger. The lanes now scroll under a
// fixed ruler instead (a scrollbar, and Alt+wheel), so the minimum is the
// ruler and one lane.
//
// The Vertical layout (the Layout dropdown): editing area | Inspector | a tall
// preview, with the toolbar across the top, for vertical videos. One choice
// for every mode, remembered across windows; in it, Simple Trim's bar and
// Multi-Cut's tracks are drawn twice as tall, with the audio section right
// under them. And Fit makes the edit fill the timeline's width.
//
//   editorsplit_test <workdir>   (run_avsync.sh generates av_red.mp4, av_tall.mp4)
#include "editor/EditorWidgets.hpp"
#include "editor/TrackEditor.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
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

	std::printf("\n-- Fit --\n");
	{
		QPushButton *fit = nullptr;
		for (QPushButton *b : w.findChildren<QPushButton *>())
			if (b->text() == QStringLiteral("Fit"))
				fit = b;
		const QRect c = tv->contentRectForTest();
		const int before = tv->clipRectForTest(0, 0).right();
		ok(fit && fit->isVisible(), "a Fit button in Full editing");
		if (fit)
			fit->click();
		settle(100);
		const int after = tv->clipRectForTest(0, 0).right();
		std::printf("     the 4 s clip ends at x %d of %d..%d before, %d after\n", before, c.left(), c.right(),
			    after);
		ok(after > c.right() - c.width() / 10 && after <= c.right(),
		   "the edit fills the width, its end just inside the right edge");
		// Zoomed right in, then Fit again: the same view.
		const QPointF mid(c.center());
		for (int i = 0; i < 4; ++i) {
			QWheelEvent in(mid, tv->mapToGlobal(mid), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
				       Qt::NoScrollPhase, false);
			QApplication::sendEvent(tv, &in);
			settle(150);
		}
		const int zoomed = tv->clipRectForTest(0, 0).right();
		if (fit)
			fit->click();
		settle(100);
		ok(zoomed > c.right() + 50 && tv->clipRectForTest(0, 0).right() == after,
		   "and from zoomed in, Fit comes back to it");
	}

	std::printf("\n-- the Vertical layout --\n");
	{
		const QString tall = QString::fromLocal8Bit(argc > 1 ? argv[1] : ".") + QStringLiteral("/av_tall.mp4");
		VideoEditorWindow v(tall);
		v.setWindowState(Qt::WindowNoState);
		v.resize(1400, 800);
		v.show();
		settle(400);
		const auto modeButton = [&v](const char *text) {
			for (QPushButton *b : v.findChildren<QPushButton *>())
				if (b->text() == QString::fromLatin1(text)) {
					b->click();
					settle(300);
					return;
				}
		};
		modeButton("Full Editing");
		auto *combo = v.findChild<QComboBox *>(QStringLiteral("editorLayout"));
		TimelineView *vt = v.findChild<TimelineView *>();
		QWidget *canvas = nullptr;
		for (QWidget *c : v.findChildren<QWidget *>())
			if (QString::fromLatin1(c->metaObject()->className()) == QStringLiteral("harpia::PreviewCanvas"))
				canvas = c;
		QPushButton *play = v.findChild<QPushButton *>(QStringLiteral("playButton")), *inspectorBtn = nullptr;
		for (QPushButton *b : v.findChildren<QPushButton *>())
			if (b->text() == QStringLiteral("Inspector"))
				inspectorBtn = b;
		ok(combo && vt && canvas && play, "a Layout dropdown, a timeline, a preview, a Play button");
		if (!combo || !vt || !canvas || !play)
			return 1;
		const auto at = [&v](QWidget *w) { return w->mapTo(&v, QPoint(0, 0)); };
		ok(combo->currentText().contains(QStringLiteral("Standard")) && at(canvas).y() < at(vt).y(),
		   "it starts Standard: the preview above the timeline");

		combo->setCurrentIndex(combo->findData(1));
		emit combo->activated(combo->currentIndex());
		settle(400);
		if (inspectorBtn && !inspectorBtn->isChecked()) {
			inspectorBtn->click();
			settle(300);
		}
		QLabel *insHeader = nullptr;
		for (QLabel *l : v.findChildren<QLabel *>())
			if (l->text() == QStringLiteral("Inspector") && l->isVisible())
				insHeader = l;
		std::printf("     timeline x %d w %d | inspector x %d | preview x %d, %dx%d | play y %d\n", at(vt).x(),
			    vt->width(), insHeader ? at(insHeader).x() : -1, at(canvas).x(), canvas->width(),
			    canvas->height(), at(play).y());
		ok(at(canvas).x() > at(vt).x() + vt->width(), "Vertical: the preview is a column right of the timeline");
		ok(insHeader && at(insHeader).x() > at(vt).x() + vt->width() - 1 && at(insHeader).x() < at(canvas).x(),
		   "with the Inspector between them");
		ok(canvas->height() > canvas->width(), "a tall column, for a 9:16 picture");
		ok(at(play).y() < at(vt).y() && at(play).y() < at(canvas).y(), "and the toolbar across the top");
		v.grab().save(QString::fromLocal8Bit(argc > 1 ? argv[1] : ".") + QStringLiteral("/layout_vertical.png"));

		// The other modes follow, and fit the tall column.
		QPushButton *audioHeader = nullptr;
		for (QPushButton *b : v.findChildren<QPushButton *>())
			if (b->text().contains(QStringLiteral("Audio")) && b->text().contains(QStringLiteral("voiceover")))
				audioHeader = b;
		modeButton("Multi-Cut");
		auto *cuts = v.findChild<TrackEditor *>();
		ok(combo->currentText().contains(QStringLiteral("Vertical")) && cuts && at(canvas).x() > at(cuts).x() &&
			   at(play).y() < at(canvas).y(),
		   "Multi-Cut is Vertical too: tracks left, the tall preview right");
		if (cuts && audioHeader) {
			const QRect src = cuts->sourceRectForTest(), out = cuts->outputRectForTest();
			std::printf("     Multi-Cut tracks %d and %d tall; the editor ends at y %d, the audio header is at %d\n",
				    src.height(), out.height(), at(cuts).y() + cuts->height(), at(audioHeader).y());
			ok(src.height() == 2 * cuts->layoutParams().srcH && out.height() == 2 * cuts->layoutParams().outH,
			   "its tracks drawn twice as tall");
			ok(at(audioHeader).y() - (at(cuts).y() + cuts->height()) < 30,
			   "and the audio section right under them, not at the foot of the column");
		}
		modeButton("Simple Trim");
		auto *trim = v.findChild<Timeline *>();
		ok(combo->currentText().contains(QStringLiteral("Vertical")) && trim && at(canvas).x() > at(trim).x(),
		   "Simple Trim is Vertical too");
		if (trim && audioHeader) {
			std::printf("     the trim bar is %d tall; the audio header %d below it\n", trim->height(),
				    at(audioHeader).y() - (at(trim).y() + trim->height()));
			ok(trim->minimumHeight() >= 2 * trim->layoutParams().barH && at(audioHeader).y() - (at(trim).y() + trim->height()) < 30,
			   "its bar twice as tall, the audio section right under it");
		}
		v.grab().save(QString::fromLocal8Bit(argc > 1 ? argv[1] : ".") + QStringLiteral("/layout_vertical_trim.png"));
		modeButton("Multi-Cut");
		v.grab().save(QString::fromLocal8Bit(argc > 1 ? argv[1] : ".") + QStringLiteral("/layout_vertical_multicut.png"));
		modeButton("Full Editing");
		ok(combo->currentText().contains(QStringLiteral("Vertical")) && at(canvas).x() > at(vt).x(),
		   "back in Full editing, Vertical again");
	} // destroyed, not closed: close() asks about unsaved changes
	{
		const QString tall = QString::fromLocal8Bit(argc > 1 ? argv[1] : ".") + QStringLiteral("/av_tall.mp4");
		VideoEditorWindow v(tall);
		v.setWindowState(Qt::WindowNoState);
		v.resize(1400, 800);
		v.show();
		settle(400);
		for (QPushButton *b : v.findChildren<QPushButton *>())
			if (b->text() == QStringLiteral("Full Editing")) {
				b->click();
				settle(400);
			}
		auto *combo = v.findChild<QComboBox *>(QStringLiteral("editorLayout"));
		TimelineView *vt = v.findChild<TimelineView *>();
		QWidget *canvas = nullptr;
		for (QWidget *c : v.findChildren<QWidget *>())
			if (QString::fromLatin1(c->metaObject()->className()) == QStringLiteral("harpia::PreviewCanvas"))
				canvas = c;
		ok(combo && combo->currentText().contains(QStringLiteral("Vertical")) && canvas && vt &&
			   canvas->mapTo(&v, QPoint()).x() > vt->mapTo(&v, QPoint()).x(),
		   "a new editor window remembers the Vertical layout");
		combo->setCurrentIndex(combo->findData(0));
		emit combo->activated(combo->currentIndex());
		settle(400);
		ok(canvas->mapTo(&v, QPoint()).y() < vt->mapTo(&v, QPoint()).y(), "and Standard puts it all back");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
