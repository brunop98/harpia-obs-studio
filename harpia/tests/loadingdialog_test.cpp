// Loading feedback stays literal, counts real completed items and cannot
// dismiss a transaction or dispatch queued work in the middle of an update.
#include "ui/LoadingDialog.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	LoadingDialog dialog(QStringLiteral("Opening project"));
	dialog.show();
	QApplication::processEvents();
	auto *stage = dialog.findChild<QLabel *>(QStringLiteral("loadingStage"));
	auto *detail = dialog.findChild<QLineEdit *>(QStringLiteral("loadingDetail"));
	auto *progress = dialog.findChild<QProgressBar *>(QStringLiteral("loadingProgress"));
	check(stage && detail && progress, "loading feedback exposes its activity, detail and progress");
	if (!stage || !detail || !progress)
		return 1;
	check(dialog.windowTitle() == QStringLiteral("Opening project"), "the operation has its own window title");
	check(dialog.windowModality() == Qt::ApplicationModal, "loading without a visible owner blocks the application");
	check(progress->minimum() == 0 && progress->maximum() == 0 && !progress->isTextVisible(),
	      "unknown work starts with an indeterminate bar and no invented percentage");
	check(detail->isHidden() && dialog.findChildren<QPushButton *>().isEmpty(),
	      "an empty detail is hidden and unsupported cancellation is not offered");
	const QString message = QStringLiteral("Reading <b>project & media</b>…");
	const QString filename = QStringLiteral("<my & project> ") + QString(350, QLatin1Char('x')) + QStringLiteral(".harpiaproj");
	dialog.setStage(message, filename);
	QApplication::processEvents();
	check(stage->textFormat() == Qt::PlainText && stage->text() == message,
	      "activity descriptions cannot interpret filenames as markup");
	check(detail->text() == filename && detail->isReadOnly() && !detail->isHidden(),
	      "the full filename stays literal, visible and copyable");
	check(detail->toolTip().contains(filename.toHtmlEscaped()), "long details remain available in an escaped tooltip");
	check(dialog.width() <= 600, "an unbroken long filename does not widen the loading window");
	detail->selectAll();
	check(detail->selectedText() == filename, "the complete filename can be selected despite its compact field");
	bool queued = false;
	QTimer::singleShot(0, &dialog, [&]() { queued = true; });
	dialog.setStage(QStringLiteral("Opening media"), QStringLiteral("clip.mp4"));
	dialog.setItemProgress(2, 5);
	check(!queued, "stage and item updates do not pump queued events");
	check(progress->minimum() == 0 && progress->maximum() == 5 && progress->value() == 2 &&
	      progress->text() == QStringLiteral("2 of 5"), "progress reports the real completed item count");
	dialog.setItemProgress(-1, 5);
	check(progress->value() == 0, "a negative completed count cannot appear");
	dialog.setItemProgress(12, 5);
	check(progress->value() == 5 && progress->text() == QStringLiteral("5 of 5"),
	      "item completion is bounded without presenting an overall percentage");
	dialog.setStage(QStringLiteral("Preparing preview"));
	check(progress->maximum() == 0 && !progress->isTextVisible() && detail->isHidden(),
	      "a new stage clears stale item counts and file details");
	dialog.setItemProgress(4, 0);
	check(progress->maximum() == 0, "an unknown total keeps the progress indeterminate");
	dialog.reject();
	check(dialog.isVisible(), "reject cannot hide a running load");
	QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
	QApplication::sendEvent(&dialog, &escape);
	check(dialog.isVisible(), "Escape cannot leave a project transaction halfway through");
	dialog.close();
	check(dialog.isVisible(), "the title-bar close action cannot dismiss active loading");
	dialog.accept();
	check(dialog.isVisible(), "an unrelated accept action cannot dismiss active loading");
	QApplication::processEvents();
	check(queued, "the caller's event loop still dispatches queued work normally");
	dialog.finish();
	check(!dialog.isVisible() && dialog.result() == QDialog::Accepted, "completion explicitly closes the loading dialog");
	dialog.setStage(QStringLiteral("Late update"), QStringLiteral("stale.mp4"));
	dialog.setItemProgress(1, 10);
	check(stage->text() == QStringLiteral("Preparing preview") && progress->maximum() == 0,
	      "late updates cannot revive or overwrite a completed operation");
	dialog.finish();
	check(!dialog.isVisible(), "finishing twice is harmless");

	QWidget owner;
	owner.show();
	LoadingDialog owned(QStringLiteral("Adding video"), &owner);
	check(owned.windowModality() == Qt::WindowModal, "a visible owner is blocked without locking other windows");
	owned.finish();
	QWidget hiddenOwner;
	LoadingDialog hidden(QStringLiteral("Opening editor"), &hiddenOwner);
	check(hidden.windowModality() == Qt::ApplicationModal, "an editor still under construction gets application modality");
	hidden.finish();
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
