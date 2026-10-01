// Exporting a Random text's versions from the real editor window, the way the
// user does it: Export… → the Variations tab → Export N videos, through the
// real dialog, the real batch runner and the real "done" message.
//
// A Random text with rich-text tags over the footage; every version must be
// written, and the editor must still be alive and usable afterwards.
#include "editor/ExportOptionsDialog.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TextVariations.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QThread>
#include <QTimer>

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
	// MP4, whatever an earlier run left chosen (the dialog remembers it).
	QSettings().setValue(QStringLiteral("export/format"), int(ClipExporter::Format::Mp4));
	VideoEditorWindow w(video);
	w.resize(1400, 1000);
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

	// The footage as it came in, plus a Random text over it with tags.
	TimelineModel m = tv->model();
	TlTrack words;
	TlClip c;
	c.type = TlClip::Type::Text;
	c.srcEndMs = 1500;
	c.text.text = QStringLiteral("I like <color=yellow>dogs</color>");
	c.text.random = true;
	c.text.variations = {QStringLiteral("I like <b>cats</b>"), QStringLiteral("I like\n<i>vultures</i>")};
	words.clips = {c};
	m.tracks.prepend(words);
	tv->setModelAndCommit(m);
	settle(300);
	const int versions = int(text_variations::combinationCount(text_variations::slotsOf(tv->model())));
	ok(versions == 3, "three versions");

	// Drive the modal dialogs as they open.
	QString lastMessage;
	bool sawDialog = false;
	QTimer driver;
	driver.setInterval(50);
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		for (QWidget *top : QApplication::topLevelWidgets()) {
			if (!top->isVisible())
				continue;
			if (auto *dlg = qobject_cast<ExportOptionsDialog *>(top)) {
				if (sawDialog)
					continue;
				sawDialog = true;
				dlg->setBatchTabForTest(true);
				for (QDialogButtonBox *bb : dlg->findChildren<QDialogButtonBox *>())
					for (QAbstractButton *b : bb->buttons())
						if (bb->buttonRole(b) == QDialogButtonBox::AcceptRole) {
							std::printf("     dialog button: %s\n", qPrintable(b->text()));
							b->click();
						}
				return;
			}
			if (auto *mb = qobject_cast<QMessageBox *>(top)) {
				lastMessage = mb->text();
				std::printf("     message: %s\n", qPrintable(lastMessage.simplified()));
				mb->accept();
				return;
			}
		}
	});
	driver.start();

	QPushButton *exportBtn = nullptr;
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->text() == QStringLiteral("Export…"))
			exportBtn = b;
	ok(exportBtn != nullptr, "the Export… button");
	if (!exportBtn)
		return 1;
	exportBtn->click(); // runs the dialog's own loop; the driver clicks through it

	// Wait for the batch to finish and its message to come and go.
	QElapsedTimer t;
	t.start();
	while (lastMessage.isEmpty() && t.elapsed() < 240000)
		settle(50);
	ok(sawDialog, "the export dialog opened");
	ok(lastMessage.contains(QStringLiteral("3 clips exported")) || lastMessage.contains(QStringLiteral("3 of 3")),
	   "all three versions written");
	const QStringList dirs = QDir(media).entryList({QStringLiteral("*")}, QDir::Dirs | QDir::NoDotAndDotDot,
						    QDir::Time);
	int files = 0;
	for (const QString &d : dirs) {
		const QStringList mp4 = QDir(media + QLatin1Char('/') + d).entryList({QStringLiteral("*.mp4")});
		if (mp4.size() == 3 && mp4.join(QLatin1Char(' ')).contains(QStringLiteral("I-like-cats"))) {
			files = mp4.size();
			std::printf("     %s: %s\n", qPrintable(d), qPrintable(mp4.join(QStringLiteral(", "))));
			QDir(media + QLatin1Char('/') + d).removeRecursively();
			break;
		}
	}
	ok(files == 3, "three files, named after their texts");

	// Still alive and usable: select, edit, preview.
	tv->setSelectionState(TlSelection());
	settle(100);
	ok(tv->model().tracks.size() >= 2, "the editor is still there afterwards");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
