// The editor's export progress when the export finishes DURING a progress
// update -- the crash in v0.1.406 (a null QProgressDialog in setLabelText,
// under VideoEditorWindow::onBatchProgress).
//
// On a modal progress dialog, QProgressDialog::setValue() runs the event loop.
// A finished() waiting in the queue is handled in the middle of the progress
// update: onBatchFinished deletes the dialog and clears its pointer, and the
// update used to carry on with that pointer. Reproduced exactly, in the real
// editor window with a real batch: queue the finish, then deliver a progress
// update. The editor must come through it, show its "stopped" message once,
// and close the progress dialog; the real batch finishing afterwards must not
// bring anything back.
#include "editor/VideoEditorWindow.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMessageBox>
#include <QProgressDialog>
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
	for (QProgressDialog *d : w.findChildren<QProgressDialog *>())
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

	std::printf("\n-- the batch finishes inside a progress update --\n");
	QMetaObject::invokeMethod(&w, "startBatchForTest", Q_ARG(QString, out.path()));
	ok(progressShowing(w), "a batch is running, with its progress dialog");

	// A first update: QProgressDialog only runs the event loop in setValue()
	// once it counts as shown, which the first non-zero value does -- not
	// show(). In a real export that is every update after the first.
	QMetaObject::invokeMethod(&w, "onBatchProgress", Qt::DirectConnection, Q_ARG(int, 0), Q_ARG(int, 1),
				  Q_ARG(int, 10), Q_ARG(qint64, qint64(5000)));
	// The finish waits in the queue; the next update's setValue() runs the
	// event loop and so handles it in the middle of the update.
	QMetaObject::invokeMethod(&w, "onBatchFinished", Qt::QueuedConnection, Q_ARG(int, 0), Q_ARG(int, 0),
				  Q_ARG(bool, true));
	QMetaObject::invokeMethod(&w, "onBatchProgress", Qt::DirectConnection, Q_ARG(int, 0), Q_ARG(int, 1),
				  Q_ARG(int, 50), Q_ARG(qint64, qint64(4000)));
	ok(true, "the progress update came through (it used to crash here)");
	settle(300);
	ok(!progressShowing(w), "the progress dialog is closed");
	ok(boxes == 1, "the \"stopped\" message was shown once");

	// The real batch is still writing its clip; let it end on its own.
	settle(4000);
	ok(!progressShowing(w), "the real finish brings nothing back");
	ok(boxes == 1, "and shows no second message");
	ok(w.isVisible(), "the editor is still open");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
