// The export panel must never disappear while a worker still owns its output.
// Exercise the actual button, Escape, window close, stage changes and queued
// progress rather than testing a duplicate progress formula.
#include "ui/ExportProgressDialog.hpp"
#include "ui/UiText.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
void check(bool condition, const char *message)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}
QLabel *label(ExportProgressDialog &dialog, const char *name)
{
	return dialog.findChild<QLabel *>(QString::fromLatin1(name));
}
} // namespace

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	const QString name = QStringLiteral("<my & export> ") + QString(180, QLatin1Char('x')) + QStringLiteral(".mp4");
	const QString folder = QDir::tempPath() + QStringLiteral("/a very long destination/") + QString(180, QLatin1Char('d'));
	ExportProgressDialog dialog(folder + QLatin1Char('/') + name,
				    QStringLiteral("MP4 · 1920 × 1080 · 30 fps · CRF 18"));
	dialog.show();
	QApplication::processEvents();
	auto *progress = dialog.findChild<QProgressBar *>(QStringLiteral("exportProgress"));
	auto *cancel = dialog.findChild<QPushButton *>(QStringLiteral("exportCancel"));
	check(progress && cancel, "progress and cancellation controls exist");
	check(dialog.windowModality() == Qt::WindowModal, "the editor stays locked while its export runs");
	check(dialog.findChild<QLineEdit *>(QStringLiteral("exportFilename"))->text() == name,
	      "long filenames including markup characters remain literal and copyable");
	check(dialog.findChild<QLineEdit *>(QStringLiteral("exportDestination"))->text() == QDir::toNativeSeparators(folder),
	      "the complete destination remains available");
	check(dialog.width() < 800, "long paths do not force an oversized window");
	check(label(dialog, "exportSettings")->text().contains(QStringLiteral("1920")), "output settings are visible");
	check(label(dialog, "exportBytes")->text() == QStringLiteral("Waiting for output…"),
	      "buffered output is explained instead of displaying zero megabytes");
	check(label(dialog, "exportRemaining")->text() == QStringLiteral("Calculating…"), "ETA starts without a false estimate");
	check(label(dialog, "exportRemaining")->toolTip().contains(QStringLiteral("current stage")), "single-export estimate explains its stage scope");

	bool queuedWorkRan = false;
	QTimer::singleShot(0, &dialog, [&] { queuedWorkRan = true; });
	dialog.setStage(QStringLiteral("Rendering video"));
	dialog.setProgress(20, 60000, 4500);
	check(!queuedWorkRan, "updating modal progress does not reenter the event loop");
	check(progress->value() == 20, "real progress is displayed");
	check(label(dialog, "exportBytes")->text() == humanFileSize(4500), "small nonzero files use the shared file-size formatter");
	check(label(dialog, "exportRemaining")->text() == QStringLiteral("Calculating…"), "early ETA is withheld while rendering warms up");
	dialog.setProgress(10, 60000, 0);
	check(progress->value() == 20, "progress never moves backward between updates");
	check(label(dialog, "exportBytes")->text() != QStringLiteral("Waiting for output…"), "buffering updates cannot erase written bytes");
	dialog.setFrameProgress(17, 0);
	check(label(dialog, "exportFrames")->text() == QStringLiteral("17"), "an unknown frame total shows only the completed count");
	dialog.setFrameProgress(60, 180);
	check(label(dialog, "exportFrames")->text().contains(QStringLiteral("60")), "completed and total frames are visible");
	QTest::qWait(3100);
	dialog.setProgress(20, 60000, 4500);
	check(label(dialog, "exportRemaining")->text().startsWith(QStringLiteral("About ")), "a warmed-up ETA becomes visible");
	check(label(dialog, "exportSpeed")->text().contains(QStringLiteral("fps")), "frame telemetry provides rendering speed");
	dialog.setStage(QStringLiteral("Mixing audio"));
	check(label(dialog, "exportRemaining")->text() == QStringLiteral("Calculating…"), "audio mixing clears the rendering ETA");
	check(label(dialog, "exportSpeed")->text() == QStringLiteral("—"), "render speed is not reused for audio mixing");
	dialog.setProgress(100, 0, 2000000);
	check(progress->value() == 99 && dialog.isVisible(), "100 percent is withheld until the output is ready");
	check(label(dialog, "exportBytes")->text().endsWith(QStringLiteral("MB")), "larger files use the same units as the rest of the app");
	dialog.finish(true);
	check(progress->value() == 100 && !dialog.isVisible(), "successful completion reaches 100 and allows dismissal");

	ExportProgressDialog canceled(QStringLiteral("/tmp/export.mp4"), QStringLiteral("MP4"));
	canceled.show();
	QSignalSpy cancelSpy(&canceled, &ExportProgressDialog::cancelRequested);
	canceled.findChild<QPushButton *>(QStringLiteral("exportCancel"))->click();
	check(cancelSpy.count() == 1 && canceled.isVisible(), "Cancel requests cancellation without hiding the panel");
	check(!canceled.findChild<QPushButton *>(QStringLiteral("exportCancel"))->isEnabled(), "Cancel is disabled during shutdown");
	QTest::keyClick(&canceled, Qt::Key_Escape);
	canceled.close();
	canceled.setProgress(100, 100, 9000000);
	canceled.setStage(QStringLiteral("Finalizing file"));
	check(cancelSpy.count() == 1, "repeated cancellation requests are emitted only once");
	check(canceled.isVisible() && label(canceled, "exportStage")->text() == QStringLiteral("Canceling…"),
	      "queued worker updates cannot hide or overwrite cancellation status");
	check(!label(canceled, "exportCancelHint")->isHidden(), "cleanup status stays visible until the worker stops");
	canceled.finish(false);
	check(!canceled.isVisible() && canceled.findChild<QProgressBar *>()->value() < 100,
	      "canceled output closes after cleanup without claiming completion");

	for (bool escape : {false, true}) {
		ExportProgressDialog closing(QStringLiteral("/tmp/export.mp4"), QStringLiteral("MP4"));
		closing.show();
		QSignalSpy spy(&closing, &ExportProgressDialog::cancelRequested);
		if (escape)
			QTest::keyClick(&closing, Qt::Key_Escape);
		else
			closing.close();
		check(spy.count() == 1 && closing.isVisible(), escape ? "Escape requests cancellation and stays open"
								      : "title-bar close requests cancellation and stays open");
		closing.finish(false);
	}

	ExportProgressDialog batch(QStringLiteral("/tmp/a.mp4"), QStringLiteral("MP4"));
	batch.setItemProgress(0, 3);
	batch.setProgress(30, 4000, 3000000);
	batch.setFrameProgress(90, 90);
	batch.setItemProgress(1, 3);
	batch.setOutputPath(QStringLiteral("/tmp/b.mp4"));
	check(label(batch, "exportItem")->text() == QStringLiteral("Clip 2 of 3"), "batch numbering is human readable");
	check(label(batch, "exportRemaining")->toolTip().contains(QStringLiteral("whole batch")), "batch estimate explains its overall scope");
	check(batch.findChild<QProgressBar *>()->value() == 30, "the next file preserves overall batch progress");
	check(label(batch, "exportFrames")->text() == QStringLiteral("—") &&
		      label(batch, "exportBytes")->text() == QStringLiteral("Waiting for output…"),
	      "the next file resets per-file counters");
	check(batch.findChild<QLineEdit *>(QStringLiteral("exportFilename"))->text() == QStringLiteral("b.mp4"),
	      "the current batch filename is updated");
	batch.setBytes(4500);
	batch.setBytes(100);
	check(label(batch, "exportBytes")->text() == humanFileSize(4500), "separate byte reports remain monotonic per file");
	batch.finish(false);
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
