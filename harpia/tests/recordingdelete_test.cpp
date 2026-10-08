// Deleting recordings with the Delete key (ui/RecordingDelete).
//
//   * Delete removes the selected recordings wherever the keyboard is in the
//     window -- on a button, on the list -- with real key presses.
//   * Not while typing: a line edit, a spin box or an editable combo keeps the
//     key (and still edits its text). A read-only field does not count.
//   * Nothing selected: nothing happens.
//   * The question names one file, or counts several; No leaves the files,
//     Yes sends them away and reports which are gone.
#include "ui/RecentListWidget.hpp"
#include "ui/RecordingDelete.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

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

static QString makeFile(const QString &dir, const QString &name)
{
	const QString p = dir + QLatin1Char('/') + name;
	QFile f(p);
	if (f.open(QIODevice::WriteOnly))
		f.write("x");
	return p;
}

// Answers the next message box with `button`, closing it whatever happens so a
// missing box fails a check instead of hanging the test.
static void answerNextBox(QMessageBox::StandardButton button, QString *text)
{
	QTimer::singleShot(100, [button, text]() {
		for (QWidget *w : QApplication::topLevelWidgets())
			if (auto *box = qobject_cast<QMessageBox *>(w); box && box->isVisible()) {
				if (text)
					*text = box->text();
				if (QAbstractButton *b = box->button(button))
					b->click();
				else
					box->reject();
				return;
			}
	});
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir tmp;
	// The recycle bin, sandboxed: on Linux it is $XDG_DATA_HOME/Trash, so this
	// test never puts anything in the real one. The folder is deliberately NOT
	// made: with it missing, Qt 6.4 "trashes" into the current directory, so
	// the test runs from an empty one and checks nothing lands there.
	const QString dataHome = tmp.path() + QStringLiteral("/data");
	qputenv("XDG_DATA_HOME", dataHome.toLocal8Bit());
	const QString cwd = tmp.path() + QStringLiteral("/cwd");
	QDir().mkpath(cwd);
	QDir::setCurrent(cwd);
	QApplication app(argc, argv);

	std::printf("\n-- the pieces --\n");
	{
		QLineEdit edit, ro;
		ro.setReadOnly(true);
		QSpinBox spin;
		QPushButton button;
		QComboBox plain, editable;
		editable.setEditable(true);
		QKeySequenceEdit keys;
		ok(recording_delete::focusEditsText(&edit), "a line edit keeps Delete");
		ok(!recording_delete::focusEditsText(&ro), "a read-only one does not");
		ok(recording_delete::focusEditsText(&spin), "a spin box keeps it");
		ok(recording_delete::focusEditsText(&editable) && !recording_delete::focusEditsText(&plain),
		   "an editable combo keeps it, a plain one does not");
		ok(recording_delete::focusEditsText(&keys), "a shortcut field keeps it");
		ok(!recording_delete::focusEditsText(&button) && !recording_delete::focusEditsText(nullptr),
		   "a button, or nothing focused, does not");
		ok(recording_delete::confirmText({QStringLiteral("/r/Day 144.mp4")}) ==
			   QStringLiteral("Move \"Day 144.mp4\" to the recycle bin?"),
		   "one file is named");
		ok(recording_delete::confirmText({QStringLiteral("/a.mp4"), QStringLiteral("/b.mp4")}) ==
			   QStringLiteral("Move 2 recordings to the recycle bin?"),
		   "several are counted");
		ok(recording_delete::recycle(tmp.path() + QStringLiteral("/never-there.mp4")),
		   "a file that is already gone is not a failure");
	}

	// A window like the main one: the strip, a button, a text field.
	QWidget win;
	auto *lay = new QVBoxLayout(&win);
	auto *list = new RecentListWidget(&win);
	auto *button = new QPushButton(QStringLiteral("Record"), &win);
	auto *edit = new QLineEdit(QStringLiteral("abc"), &win);
	lay->addWidget(list);
	lay->addWidget(button);
	lay->addWidget(edit);
	QStringList files;
	for (const char *n : {"Dark Island - 0033.mp4", "Day 144.mp4", "Day 145.mp4"}) {
		files << makeFile(tmp.path(), QString::fromLatin1(n));
		auto *it = new QListWidgetItem(QFileInfo(files.back()).fileName(), list);
		it->setData(kClipPathRole, files.back());
	}
	QStringList asked;
	int calls = 0;
	recording_delete::installDeleteKey(&win, list, [&](const QStringList &p) {
		++calls;
		asked = p;
	});
	win.show();
	win.activateWindow();
	settle(200);

	std::printf("\n-- the Delete key --\n");
	list->item(0)->setSelected(true);
	list->item(2)->setSelected(true);
	ok(recording_delete::selectedPaths(list) == QStringList({files[0], files[2]}),
	   "the selection, in list order");
	button->setFocus();
	settle(50);
	QTest::keyClick(button, Qt::Key_Delete);
	settle(50);
	ok(calls == 1 && asked == QStringList({files[0], files[2]}),
	   "with the keyboard on a button, Delete asks to delete the selected recordings");
	list->setFocus();
	settle(50);
	QTest::keyClick(list, Qt::Key_Delete);
	settle(50);
	ok(calls == 2, "and with it on the list");
	edit->setFocus();
	edit->setCursorPosition(0);
	settle(50);
	QTest::keyClick(edit, Qt::Key_Delete);
	settle(50);
	ok(calls == 2, "but not while typing");
	ok(edit->text() == QStringLiteral("bc"), "where Delete still deletes a character");
	list->clearSelection();
	button->setFocus();
	settle(50);
	QTest::keyClick(button, Qt::Key_Delete);
	settle(50);
	ok(calls == 2, "nothing selected: nothing happens");

	std::printf("\n-- the question --\n");
	{
		QString text;
		answerNextBox(QMessageBox::No, &text);
		const QStringList gone = recording_delete::confirmAndRecycle(&win, {files[1]});
		std::printf("     asked: %s\n", qPrintable(text));
		ok(text.contains(QStringLiteral("Day 144.mp4")), "it names the recording");
		ok(gone.isEmpty() && QFile::exists(files[1]), "No keeps the file");
	}
	{
		answerNextBox(QMessageBox::Yes, nullptr);
		const QStringList gone = recording_delete::confirmAndRecycle(&win, {files[0], files[1]});
		ok(gone == QStringList({files[0], files[1]}), "Yes: both reported gone");
		ok(!QFile::exists(files[0]) && !QFile::exists(files[1]), "and both are off the disk");
		ok(QFile::exists(files[2]), "the one not asked about stays");
#ifdef Q_OS_LINUX
		ok(QFile::exists(dataHome + QStringLiteral("/Trash/files/Day 144.mp4")),
		   "they went to the recycle bin, not straight off the disk");
		ok(QDir(cwd).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty(),
		   "and nothing was 'trashed' into the current directory (the Qt 6.4 bug)");
#endif
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
