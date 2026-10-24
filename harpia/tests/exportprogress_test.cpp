// Export progress must not dispatch queued completion inside an update (the
// reentrancy crash in v0.1.406). Exercise the real batch wiring, then cancel
// and complete single exports through the normal options/progress dialogs.
#include "editor/VideoEditorWindow.hpp"
#include "editor/ExportOptionsDialog.hpp"
#include "ui/ExportProgressDialog.hpp"
#include "ui/ExportDoneDialog.hpp"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QMessageBox>
#include <QLineEdit>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
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

static bool progressShowing(QWidget &w)
{
	for (ExportProgressDialog *d : w.findChildren<ExportProgressDialog *>())
		if (d->isVisible())
			return true;
	return false;
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
	QTemporaryDir out;

	VideoEditorWindow w(video);
	w.resize(1200, 800);
	w.show();
	settle(300);

	// Every message box is answered (and counted), so none can hold the test.
	int boxes = 0;
	QTimer closer;
	QObject::connect(&closer, &QTimer::timeout, [&boxes]() {
		for (QWidget *top : QApplication::topLevelWidgets())
			if (auto *box = qobject_cast<QMessageBox *>(top); box && box->isVisible()) {
				++boxes;
				box->reject();
			}
	});
	closer.start(50);

	std::printf("\n-- batch completion queued during a progress update --\n");
	QMetaObject::invokeMethod(&w, "startBatchForTest", Q_ARG(QString, out.path()));
	ok(progressShowing(w), "a batch is running, with its progress dialog");

	// A normal update, then a queued finish. Updating the custom dialog
	// never pumps the event loop, so completion cannot clear it mid-update.
	QMetaObject::invokeMethod(&w, "onBatchProgress", Qt::DirectConnection, Q_ARG(int, 0), Q_ARG(int, 1),
				  Q_ARG(int, 10), Q_ARG(qint64, qint64(5000)));
	// Completion stays queued while this update runs.
	QMetaObject::invokeMethod(&w, "onBatchFinished", Qt::QueuedConnection, Q_ARG(int, 0), Q_ARG(int, 0),
				  Q_ARG(bool, true));
	QMetaObject::invokeMethod(&w, "onBatchProgress", Qt::DirectConnection, Q_ARG(int, 0), Q_ARG(int, 1),
				  Q_ARG(int, 50), Q_ARG(qint64, qint64(4000)));
	ok(progressShowing(w) && boxes == 0, "progress updates do not dispatch a queued completion");
	settle(300);
	ok(!progressShowing(w), "the progress dialog is closed");
	ok(boxes == 1, "the \"stopped\" message was shown once");

	// The real batch is still writing its clip; let it end on its own.
	settle(4000);
	ok(!progressShowing(w), "the real finish brings nothing back");
	ok(boxes == 1, "and shows no second message");
	ok(w.isVisible(), "the editor is still open");


	// Exercise onSave itself, including both new backend signal connections
	// and the existing success dialog. The folder and filename are the first
	// two text fields in the options window (before spinbox internal fields).
	auto startSingle = [&](const QString &name) {
		QTimer::singleShot(0, &w, [&, name]() {
			auto *options = w.findChild<ExportOptionsDialog *>();
			if (!options) {
				ok(false, "the export options window opens");
				return;
			}
			const auto edits = options->findChildren<QLineEdit *>();
			if (edits.size() >= 2) {
				edits[0]->setText(name);
				edits[1]->setText(out.path());
			}
			for (auto *combo : options->findChildren<QComboBox *>()) {
				const int mp4 = combo->findText(QStringLiteral("MP4 (H.264)"));
				if (mp4 >= 0)
					combo->setCurrentIndex(mp4);
			}
			options->accept();
		});
		QMetaObject::invokeMethod(&w, "onSave", Qt::DirectConnection);
		for (auto *panel : w.findChildren<ExportProgressDialog *>())
			if (panel->isVisible())
				return panel;
		return static_cast<ExportProgressDialog *>(nullptr);
	};
	int exported = 0;
	QObject::connect(&w, &VideoEditorWindow::exported, [&](const QString &) { ++exported; });
	ExportProgressDialog *single = startSingle(QStringLiteral("canceled"));
	ok(single != nullptr, "a single export opens the detailed progress panel");
	if (single) {
		const QString summary = single->findChild<QLabel *>(QStringLiteral("exportSettings"))->text();
		ok(summary.contains(QStringLiteral("MP4")) && summary.contains(QStringLiteral("320 × 180")) &&
		   summary.contains(QStringLiteral("30 fps")) && summary.contains(QStringLiteral("CRF")),
		   "the panel summarizes the chosen format, dimensions, frame rate and quality");
		single->findChild<QPushButton *>(QStringLiteral("exportCancel"))->click();
		ok(single->isVisible() && single->findChild<QLabel *>(QStringLiteral("exportStage"))->text() ==
		   QStringLiteral("Canceling…"), "the single export stays visible while cancellation is pending");
	}
	settle(1000);
	ok(!progressShowing(w) && !QFile::exists(out.filePath(QStringLiteral("canceled.mp4"))),
	   "single cancellation closes only after the partial output has been removed");
	ok(exported == 0 && boxes == 1, "single cancellation emits neither success nor an error popup");

	bool completedDialog = false;
	QTimer dismissSuccess;
	QObject::connect(&dismissSuccess, &QTimer::timeout, [&]() {
		for (auto *done : w.findChildren<ExportDoneDialog *>())
			if (done->isVisible()) {
				completedDialog = true;
				done->reject();
			}
	});
	dismissSuccess.start(20);
	single = startSingle(QStringLiteral("complete"));
	ok(single != nullptr, "a new export can start after cancellation");
	QElapsedTimer waitForSuccess;
	waitForSuccess.start();
	while (!completedDialog && waitForSuccess.elapsed() < 15000)
		settle(20);
	dismissSuccess.stop();
	ok(completedDialog && exported == 1, "a finished file opens the existing share and copy dialog once");
	ok(!progressShowing(w) && QFile::exists(out.filePath(QStringLiteral("complete.mp4"))),
	   "successful completion closes progress and keeps the ready output");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
