// Export failures expose a short summary and a complete, copyable diagnostic
// report. Cancellation stays silent rather than being presented as a failure.
#include "editor/VideoEditorWindow.hpp"
#include "ui/EditorLog.hpp"

#include <QApplication>
#include <QClipboard>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static QString loggedReport;
static EditorLogLevel loggedLevel = EditorLogLevel::Info;

static void ok(bool condition, const char *what)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		++failures;
}

static void captureLog(EditorLogLevel level, const QString &line)
{
	loggedLevel = level;
	loggedReport = line;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	VideoEditorWindow editor{QString()};
	const auto previousSink = editorLogSink();
	editorLogSink() = captureLog;

	const QString report = QStringLiteral(
		"Timeline rendering failed [TL-EFFECT-FRAME].\n\n"
		"Reason: Pixel component test.emptyFrame returned an empty image.\n"
		"Timeline time: 00:00:01.200 (frame 36)\n"
		"Track: V1; clip: Opening <take>\n"
		"Source: /media/opening.mp4");
	QApplication::clipboard()->clear();
	bool observed = false;
	QTimer inspectDialog;
	inspectDialog.setInterval(10);
	QObject::connect(&inspectDialog, &QTimer::timeout, [&]() {
		auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
		if (!box)
			return;
		inspectDialog.stop();
		observed = true;
		ok(box->windowTitle() == QStringLiteral("Export failed"), "the failure dialog identifies the failed export");
		ok(box->icon() == QMessageBox::Critical, "an export failure is presented as an error");
		ok(box->text() == report.section(QLatin1Char('\n'), 0, 0), "the main message is only the short summary");
		ok(box->textFormat() == Qt::PlainText, "paths and clip names cannot be interpreted as markup");
		ok(box->informativeText().contains(QStringLiteral("Reason: Pixel component")),
		   "the underlying reason is visible before expanding the details");
		ok(box->detailedText() == report, "the full report retains the code, time, source and underlying reason");
		ok(box->textInteractionFlags().testFlag(Qt::TextSelectableByMouse), "the diagnostic message can be selected");
		QPushButton *copy = nullptr;
		for (QAbstractButton *button : box->buttons())
			if (button->text() == QStringLiteral("Copy details"))
				copy = qobject_cast<QPushButton *>(button);
		ok(copy && box->buttonRole(copy) == QMessageBox::ActionRole, "Copy details is available as a dialog action");
		if (copy)
			copy->click();
		else
			box->accept();
	});
	inspectDialog.start();
	const bool invoked = QMetaObject::invokeMethod(&editor, "onExportFinished", Qt::DirectConnection,
		Q_ARG(bool, false), Q_ARG(bool, false), Q_ARG(QString, report));
	inspectDialog.stop();
	ok(invoked && observed, "the real export completion slot displays the diagnostic dialog");
	ok(QApplication::clipboard()->text() == report, "Copy details places the exact complete report on the clipboard");
	ok(loggedLevel == EditorLogLevel::Error &&
		   loggedReport == QStringLiteral("[Editor] Export failed: ") + report,
	   "the full diagnostic report is also logged at error severity");

	loggedReport.clear();
	bool cancellationDialog = false;
	QTimer dismissUnexpected;
	dismissUnexpected.setInterval(10);
	QObject::connect(&dismissUnexpected, &QTimer::timeout, [&]() {
		if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
			cancellationDialog = true;
			box->accept();
		}
	});
	dismissUnexpected.start();
	const bool canceledInvoked = QMetaObject::invokeMethod(&editor, "onExportFinished", Qt::DirectConnection,
		Q_ARG(bool, false), Q_ARG(bool, true), Q_ARG(QString, report));
	dismissUnexpected.stop();
	QApplication::processEvents();
	ok(canceledInvoked && !cancellationDialog && !QApplication::activeModalWidget(),
	   "canceling an export does not show an error dialog");
	ok(loggedReport.isEmpty(), "canceling is not logged as an export failure");
	ok(QApplication::clipboard()->text() == report, "cancellation leaves the clipboard untouched");

	editorLogSink() = previousSink;
	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
