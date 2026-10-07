// The Animation section's keyframe list in the real editor window.
//
//   * It is drag-to-reorder: moving a row (the same model move Qt's drag makes)
//     reorders the clip's framings while the key times stay put, the playhead
//     lands on the moved key, and Ctrl+Z puts the old order back.
//   * Right-click opens a menu with Move to first, Move to last, Reset value
//     and Delete; picking one from the menu does it.
//   * Reset value makes the key 100% of the screen, centred.
//   * Delete takes the key; down to one key, the animation ends and the list
//     hides.
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QShortcut>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <functional>

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
	QListWidget *list = w.findChild<QListWidget *>(QStringLiteral("keyList"));
	ok(tv && list, "in Full editing, with the keyframe list");
	if (!tv || !list)
		return 1;

	// One clip, three keys at the screenshot's times: A, B, C.
	TimelineModel m = tv->model();
	int src = -1;
	for (const TlTrack &t : m.tracks)
		for (const TlClip &c : t.clips)
			src = c.sourceId;
	TlTrack t;
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = src;
	c.srcEndMs = 6000;
	const qint64 times[] = {0, 300, 5400};
	const double xs[] = {0.2, 0.5, 0.8}, scales[] = {1.5, 2.0, 2.5};
	for (int i = 0; i < 3; ++i) {
		TlKeyframe k;
		k.tMs = times[i];
		k.tf.posX = xs[i];
		k.tf.posY = 0.3;
		k.tf.scale = scales[i];
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
	if (!tv->selectedClipPtr())
		return 1;
	const auto keys = [&]() { return tv->selectedClipPtr()->keys; };
	const auto xsAre = [&](double a, double b, double cc) {
		const auto k = keys();
		return k.size() == 3 && k[0].tf.posX == a && k[1].tf.posX == b && k[2].tf.posX == cc;
	};
	const auto timesKept = [&]() {
		const auto k = keys();
		return k.size() == 3 && k[0].tMs == 0 && k[1].tMs == 300 && k[2].tMs == 5400;
	};
	ok(list->count() == 3, "three rows");
	ok(list->dragDropMode() == QAbstractItemView::InternalMove, "rows can be dragged to reorder");
	ok(list->contextMenuPolicy() == Qt::CustomContextMenu, "and right-clicked");

	QShortcut *undo = nullptr;
	for (QShortcut *s : w.findChildren<QShortcut *>())
		if (s->key() == QKeySequence(Qt::CTRL | Qt::Key_Z) && s->isEnabled())
			undo = s;

	std::printf("\n-- drag a row --\n");
	{
		// C's row dropped above A's: the move Qt's InternalMove drop makes.
		const bool moved = list->model()->moveRow(QModelIndex(), 2, QModelIndex(), 0);
		ok(moved, "the row moves");
		settle(300);
		ok(xsAre(0.8, 0.2, 0.5), "C's framing plays first, then A, then B");
		ok(timesKept(), "the key times stay where they were");
		ok(keys()[0].tf.scale == 2.5, "C's zoom went with it");
		ok(list->count() == 3 && list->currentRow() == 0, "the list is rebuilt, the moved key highlighted");
		const qint64 ph = tv->playhead() - tv->selectedClipPtr()->outStartMs;
		std::printf("     playhead at clip %lld ms\n", (long long)ph);
		ok(std::llabs(ph - 0) <= 40, "and the playhead is on it");
		if (undo) {
			emit undo->activated();
			settle(300);
			ok(xsAre(0.2, 0.5, 0.8), "Ctrl+Z puts the old order back");
		} else {
			ok(false, "Ctrl+Z is there");
		}
	}

	std::printf("\n-- right-click menu --\n");
	QStringList seen;
	const auto openMenu = []() -> QMenu * {
		if (auto *m = qobject_cast<QMenu *>(QApplication::activePopupWidget()))
			return m;
		for (QWidget *top : QApplication::topLevelWidgets())
			if (auto *m = qobject_cast<QMenu *>(top); m && m->isVisible())
				return m;
		return nullptr;
	};
	// Polls for the menu (menu.exec is blocking) and always closes it, so a
	// missing popup fails the checks instead of hanging the test.
	std::function<void(QString, int)> answer;
	answer = [&](QString text, int tries) {
		QMenu *menu = openMenu();
		if (!menu) {
			if (tries > 0)
				QTimer::singleShot(50, [&, text, tries]() { answer(text, tries - 1); });
			return;
		}
		{
			for (QAction *a : menu->actions())
				if (!a->isSeparator() && !a->text().isEmpty())
					seen << a->text() + (a->isEnabled() ? QString() : QStringLiteral(" (off)"));
			QAction *want = nullptr;
			for (QAction *a : menu->actions())
				if (!text.isEmpty() && !a->isSeparator() && a->text() == text)
					want = a;
			if (want && want->isEnabled()) {
				menu->setActiveAction(want);
				QTest::keyClick(menu, Qt::Key_Return);
			} else {
				menu->close();
			}
		}
	};
	const auto pick = [&](int row, const QString &text) {
		seen.clear();
		QTimer::singleShot(50, [&, text]() { answer(text, 40); });
		QListWidgetItem *it = list->item(row);
		if (!it)
			return;
		emit list->customContextMenuRequested(list->visualItemRect(it).center());
		settle(300);
	};
	pick(0, QString());
	std::printf("     menu on the first row: %s\n", qPrintable(seen.join(QStringLiteral(" | "))));
	ok(seen.contains(QStringLiteral("Move to first (off)")), "Move to first, off on the first row");
	ok(seen.contains(QStringLiteral("Move to last")), "Move to last");
	ok(seen.contains(QStringLiteral("Reset value (100%, centred)")), "Reset value");
	ok(seen.contains(QStringLiteral("Delete")), "Delete");

	pick(0, QStringLiteral("Move to last"));
	ok(xsAre(0.5, 0.8, 0.2) && timesKept(), "Move to last: A's framing plays last, the times stay");
	pick(2, QStringLiteral("Move to first"));
	ok(xsAre(0.2, 0.5, 0.8) && timesKept(), "Move to first: and back to the front");

	std::printf("\n-- reset value --\n");
	pick(1, QStringLiteral("Reset value (100%, centred)"));
	{
		const TlKeyframe k = keys()[1];
		std::printf("     key 2: x=%.2f y=%.2f scale=%.2f\n", k.tf.posX, k.tf.posY, k.tf.scale);
		ok(k.tf.posX == 0.5 && k.tf.posY == 0.5 && k.tf.scale == 1.0, "100% of the screen, centred");
		ok(k.tMs == 300, "at the same time");
		ok(keys()[0].tf.scale == 1.5 && keys()[2].tf.scale == 2.5, "the other keys untouched");
	}

	std::printf("\n-- delete --\n");
	pick(1, QStringLiteral("Delete"));
	ok(keys().size() == 2 && keys()[0].tMs == 0 && keys()[1].tMs == 5400, "the key is gone");
	ok(list->count() == 2, "and its row");
	w.keyRowAction(0, VideoEditorWindow::KeyRowAction::Delete);
	settle(200);
	ok(keys().isEmpty(), "down to one key, the animation ends");
	ok(std::abs(tv->selectedClipPtr()->baseTransform().posX - 0.8) < 1e-9,
	   "and the clip holds the framing that was left");
	ok(list->isHidden(), "the empty list hides");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
