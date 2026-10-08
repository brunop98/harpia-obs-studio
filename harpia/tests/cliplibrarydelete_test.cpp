// The Clip Library window's Delete key, in the real window.
//
//   * With a card selected, Delete removes it wherever the keyboard is in the
//     window -- here the size slider, which used to make Delete do nothing
//     (the key only worked while the grid itself had focus).
//   * It asks first; the clip goes to the recycle bin, its card goes, and the
//     window says which recordings were deleted (the main window's Recent
//     strip listens for that).
#include "model/PresetStore.hpp"
#include "ui/ClipLibraryWindow.hpp"
#include "ui/RecentListWidget.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QListWidget>
#include <QMessageBox>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <cstdio>

// PresetStore without libobs: the window only reads presets(), and the real
// constructor (model/PresetStore.cpp) resolves OBS's config path.
namespace harpia {
PresetStore::PresetStore(std::string configDir) : configDir_(std::move(configDir)) {}
void PresetStore::upsert(const Preset &preset)
{
	presets_.push_back(preset);
}
} // namespace harpia

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

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir tmp;
	const QString dataHome = tmp.path() + QStringLiteral("/data"); // the recycle bin, sandboxed
	QDir().mkpath(dataHome);
	qputenv("XDG_DATA_HOME", dataHome.toLocal8Bit());
	QApplication app(argc, argv);
	const QString media = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
	const QString video = media + QStringLiteral("/av_red.mp4");
	if (!QFile::exists(video)) {
		std::printf("missing %s\n", qPrintable(video));
		return 2;
	}
	const QString rec = tmp.path() + QStringLiteral("/rec");
	QDir().mkpath(rec);
	const QString a = rec + QStringLiteral("/Dark Island - 0033.mp4");
	const QString b = rec + QStringLiteral("/Day 144.mp4");
	QFile::copy(video, a);
	QFile::copy(video, b);

	PresetStore store(tmp.path().toStdString());
	Preset p;
	p.id = "test";
	p.name = "Test";
	p.outputFolder = rec.toStdString();
	store.upsert(p);

	ClipLibraryWindow win(store);
	QSignalSpy deleted(&win, &ClipLibraryWindow::recordingsDeleted);
	win.resize(900, 600);
	win.show();
	win.activateWindow();
	win.refresh();
	settle(500);
	QListWidget *grid = win.findChild<QListWidget *>();
	QSlider *slider = win.findChild<QSlider *>();
	ok(grid && slider, "the library, with its grid and size slider");
	if (!grid || !slider)
		return 1;
	ok(grid->count() == 2, "both recordings listed");
	QListWidgetItem *dayCard = nullptr;
	for (int i = 0; i < grid->count(); ++i)
		if (grid->item(i)->data(kClipPathRole).toString() == b)
			dayCard = grid->item(i);
	ok(dayCard != nullptr, "Day 144 has a card");
	if (!dayCard)
		return 1;

	std::printf("\n-- Delete with the keyboard on the slider --\n");
	grid->clearSelection();
	dayCard->setSelected(true);
	slider->setFocus();
	settle(50);
	QString asked;
	QTimer::singleShot(150, [&asked]() {
		for (QWidget *w : QApplication::topLevelWidgets())
			if (auto *box = qobject_cast<QMessageBox *>(w); box && box->isVisible()) {
				asked = box->text();
				if (QAbstractButton *yes = box->button(QMessageBox::Yes))
					yes->click();
				else
					box->reject();
				return;
			}
	});
	QTest::keyClick(slider, Qt::Key_Delete);
	settle(500);
	std::printf("     asked: %s\n", qPrintable(asked));
	ok(asked.contains(QStringLiteral("Day 144.mp4")), "it asks, naming the recording");
	ok(!QFile::exists(b), "the file is gone");
	ok(QFile::exists(dataHome + QStringLiteral("/Trash/files/Day 144.mp4")), "to the recycle bin");
	ok(QFile::exists(a), "the other one stays");
	ok(grid->count() == 1, "and its card goes");
	ok(deleted.size() == 1 && deleted.at(0).at(0).toStringList() == QStringList{b},
	   "the window says which recording went (the Recent strip refreshes on it)");

	std::printf("\n-- nothing selected --\n");
	grid->clearSelection();
	slider->setFocus();
	settle(50);
	QTest::keyClick(slider, Qt::Key_Delete);
	settle(300);
	ok(QFile::exists(a) && deleted.size() == 1, "Delete does nothing");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
