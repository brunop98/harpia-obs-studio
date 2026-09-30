// Undo in the real editor window, through the real Ctrl+Z / Ctrl+Y shortcuts.
//
//   * A change of selection is an undo step: click a clip, click another,
//     Ctrl+Z goes back to the first, again to nothing, Ctrl+Y forward.
//   * A selection box is ONE step, however many clips it swept past on the way.
//   * Force ripple (track menu) closes a track's gaps as one step that
//     Ctrl+Z puts back exactly.
//   * Dragging a group moves every member, and Ctrl+Z puts every member and
//     the selection back.
//   * An Inspector-style edit (nudge, through clipsChanged) is undoable too.
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMouseEvent>
#include <QPushButton>
#include <QShortcut>
#include <QThread>

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

static void mouse(QWidget *w, QEvent::Type t, QPoint p, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
	const Qt::MouseButtons held = t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton);
	QMouseEvent e(t, QPointF(p), w->mapToGlobal(QPointF(p)), t == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
		      held, mods);
	QApplication::sendEvent(w, &e);
}

static void click(QWidget *w, QPoint p, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
	mouse(w, QEvent::MouseButtonPress, p, mods);
	mouse(w, QEvent::MouseButtonRelease, p, mods);
	settle(30);
}

static void drag(QWidget *w, QPoint from, QPoint to, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
	mouse(w, QEvent::MouseButtonPress, from, mods);
	for (int k = 1; k <= 4; ++k)
		mouse(w, QEvent::MouseMove, from + (to - from) * k / 4, mods);
	mouse(w, QEvent::MouseButtonRelease, to, mods);
	settle(30);
}

// The live shortcut bound to `key`, as the registry built it.
static QShortcut *shortcutFor(QWidget &w, const QKeySequence &key)
{
	for (QShortcut *s : w.findChildren<QShortcut *>())
		if (s->key() == key && s->isEnabled())
			return s;
	return nullptr;
}

static QVector<qint64> starts(const TimelineView *tv, int track)
{
	QVector<qint64> out;
	for (const TlClip &c : tv->model().tracks[track].clips)
		out.push_back(c.outStartMs);
	return out;
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
	w.resize(1400, 900);
	w.show();
	settle(300);
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->text() == QStringLiteral("Full Editing")) {
			b->click();
			settle(300);
			break;
		}
	TimelineView *tv = w.findChild<TimelineView *>();
	ok(tv != nullptr, "in Full editing");
	if (!tv)
		return 1;
	int src = -1;
	for (const TlTrack &t : tv->model().tracks)
		for (const TlClip &c : t.clips)
			src = c.sourceId;
	ok(src >= 0, "the video is on the timeline");

	QShortcut *undoKey = shortcutFor(w, QKeySequence(Qt::CTRL | Qt::Key_Z));
	QShortcut *redoKey = shortcutFor(w, QKeySequence(Qt::CTRL | Qt::Key_Y));
	if (!redoKey)
		redoKey = shortcutFor(w, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z));
	ok(undoKey && redoKey, "Ctrl+Z and Ctrl+Y are bound");
	if (!undoKey || !redoKey)
		return 1;
	const auto undo = [&]() {
		emit undoKey->activated();
		settle(60);
	};
	const auto redo = [&]() {
		emit redoKey->activated();
		settle(60);
	};

	// One video track, three one-second clips with gaps between them.
	TimelineModel m;
	TlTrack t;
	t.kind = TlTrack::Kind::Video;
	for (qint64 at : {0, 2000, 5000}) {
		TlClip c;
		c.type = TlClip::Type::Video;
		c.sourceId = src;
		c.srcStartMs = 0;
		c.srcEndMs = 1000;
		c.outStartMs = at;
		t.clips.append(c);
	}
	m.tracks = {t};
	tv->setModelAndCommit(m);
	settle(400);
	tv->zoomToFit();
	settle(50);

	const auto centre = [&](int ci) { return tv->clipRectForTest(0, ci).center(); };

	std::printf("\n-- selection is an undo step --\n");
	{
		// Start from nothing selected (a clip selected before the model was
		// swapped in stays selected), which is itself one step.
		tv->setSelectionState(TlSelection());
		settle(60);
		ok(tv->selectedCountForTest() == 0, "nothing selected to begin with");
		click(tv, centre(0));
		ok(tv->selectedClip() == 0 && tv->selectedCountForTest() == 1, "clicked clip 1");
		click(tv, centre(2));
		ok(tv->selectedClip() == 2, "clicked clip 3");
		undo();
		ok(tv->selectedClip() == 0 && tv->selectedCountForTest() == 1, "Ctrl+Z: back to clip 1");
		undo();
		ok(tv->selectedCountForTest() == 0, "Ctrl+Z again: nothing selected");
		redo();
		ok(tv->selectedClip() == 0, "Ctrl+Y: clip 1 again");
		redo();
		ok(tv->selectedClip() == 2, "Ctrl+Y: clip 3 again");
	}

	std::printf("\n-- a selection box is one step --\n");
	{
		click(tv, centre(1));
		const QRect r0 = tv->clipRectForTest(0, 0), r2 = tv->clipRectForTest(0, 2);
		drag(tv, QPoint(r0.left() + 2, r0.top() + 2), QPoint(r2.right() - 2, r0.top() + 5), Qt::ShiftModifier);
		ok(tv->selectedCountForTest() == 3, "the box took all three");
		undo();
		ok(tv->selectedClip() == 1 && tv->selectedCountForTest() == 1,
		   "one Ctrl+Z goes straight back to the clip before the box");
		redo();
		ok(tv->selectedCountForTest() == 3, "Ctrl+Y: the box's three again");
	}

	std::printf("\n-- dragging the group, undone as one --\n");
	{
		const QVector<qint64> before = starts(tv, 0);
		const QPoint grab = centre(1); // not the primary
		drag(tv, grab, grab + QPoint(80, 0));
		const QVector<qint64> after = starts(tv, 0);
		const qint64 d = after[1] - before[1];
		std::printf("     moved by %lld ms\n", (long long)d);
		ok(d > 0 && after[0] - before[0] == d && after[2] - before[2] == d, "all three moved together");
		ok(tv->selectedCountForTest() == 3, "still all selected");
		undo();
		ok(starts(tv, 0) == before, "Ctrl+Z: every clip back where it was");
		ok(tv->selectedCountForTest() == 3, "with the group still selected");
		redo();
		ok(starts(tv, 0) == after, "Ctrl+Y: moved again");
		undo();
	}

	std::printf("\n-- force ripple --\n");
	{
		const QVector<qint64> before = starts(tv, 0);
		const int moved = tv->forceRipple(0);
		settle(60);
		const QVector<qint64> after = starts(tv, 0);
		std::printf("     starts %lld %lld %lld -> %lld %lld %lld\n", (long long)before[0], (long long)before[1],
			    (long long)before[2], (long long)after[0], (long long)after[1], (long long)after[2]);
		ok(moved == 2, "two clips moved");
		ok(after[0] == before[0] && after[1] == before[0] + 1000 && after[2] == before[0] + 2000,
		   "back to back, the first where it was");
		ok(tv->forceRipple(0) == 0, "a second time: nothing to do");
		undo();
		ok(starts(tv, 0) == before, "Ctrl+Z: the gaps are back");
		redo();
		ok(starts(tv, 0) == after, "Ctrl+Y: closed again");
	}

	std::printf("\n-- an edit through clipsChanged (nudge) --\n");
	{
		click(tv, centre(2));
		const QVector<qint64> before = starts(tv, 0);
		tv->nudgeSelection(500);
		settle(500); // past the coalescing timer
		ok(starts(tv, 0)[2] == before[2] + 500, "nudged");
		undo();
		ok(starts(tv, 0) == before, "Ctrl+Z puts it back");
	}

	std::printf("\n-- a paste and the selection it makes are one step --\n");
	{
		// One clip: a plain click on a member of a group keeps the group
		// (that is what lets a group be dragged), so clear it first.
		tv->setSelectionState(TlSelection());
		settle(60);
		click(tv, centre(0));
		ok(tv->selectedCountForTest() == 1, "one clip selected to copy");
		const int clipsBefore = tv->model().tracks[0].clips.size();
		const auto entries = tv->copySelection();
		tv->pasteAt(entries, 9000);
		settle(60);
		std::printf("     after paste: %d tracks, track 0 has %d clips, selected %d/%d (%d in all)\n",
			    int(tv->model().tracks.size()), int(tv->model().tracks[0].clips.size()), tv->selectedTrack(),
			    tv->selectedClip(), tv->selectedCountForTest());
		ok(tv->model().tracks[0].clips.size() == clipsBefore + 1, "pasted");
		ok(tv->selectedClip() == clipsBefore, "the pasted clip is selected");
		undo();
		std::printf("     after undo: %d tracks, track 0 has %d clips, selected %d/%d (%d in all)\n",
			    int(tv->model().tracks.size()), int(tv->model().tracks[0].clips.size()), tv->selectedTrack(),
			    tv->selectedClip(), tv->selectedCountForTest());
		ok(tv->model().tracks[0].clips.size() == clipsBefore, "one Ctrl+Z removes the paste");
		ok(tv->selectedClip() == 0 && tv->selectedCountForTest() == 1, "and brings back what was selected");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
