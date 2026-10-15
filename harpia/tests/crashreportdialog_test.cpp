// "Harpia crashed last time" (ui/CrashReportDialog).
//
//   * The summary is the report's opening lines -- the version, when, what
//     went wrong -- and none of the stack.
//   * A report with a 300-line stack: the window still fits the screen, the
//     buttons are inside it, and the whole report is in a box that scrolls.
//     (The message box this replaces grew with the stack until its buttons
//     were below the bottom of the screen, and could not be closed.)
//   * Copy report puts every line on the clipboard; Open logs folder closes
//     and says so; Close and Escape close without it.
#include "ui/CrashReportDialog.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QTest>
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

static QPushButton *buttonNamed(QDialog &d, const QString &text)
{
	for (QPushButton *b : d.findChildren<QPushButton *>())
		if (b->text() == text)
			return b;
	return nullptr;
}

static QString sampleReport(int frames)
{
	QString r = QStringLiteral("Harpia Recorder and Editor v0.1.406 crashed\n2026-10-15 01:30:02\n\n"
				   "Segmentation fault (invalid memory access)\n\nStack (crashing thread):\n");
	for (int i = 0; i < frames; ++i)
		r += QStringLiteral("  #%1 Qt6Widgets.dll!QWidget::event +0x%2  [module+0x58f65]\n").arg(i).arg(i * 16, 0, 16);
	return r;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	const QString report = sampleReport(300);

	std::printf("\n-- the summary --\n");
	const QString summary = CrashReportDialog::summaryOf(report);
	ok(summary.contains(QStringLiteral("v0.1.406 crashed")) && summary.contains(QStringLiteral("2026-10-15")) &&
		   summary.contains(QStringLiteral("Segmentation fault")),
	   "the version, when, and what went wrong");
	ok(!summary.contains(QStringLiteral("Stack")) && !summary.contains(QStringLiteral("#0")), "none of the stack");
	ok(CrashReportDialog::summaryOf(QStringLiteral("just one line")) == QStringLiteral("just one line"),
	   "a report without a stack: its lines");

	std::printf("\n-- a 300-line stack --\n");
	{
		CrashReportDialog d(report);
		d.show();
		QTest::qWait(100);
		const QRect avail = d.screen()->availableGeometry();
		ok(d.frameGeometry().height() <= avail.height() && d.height() <= avail.height() * 3 / 4 + 1,
		   "the window fits the screen");
		QPushButton *close = buttonNamed(d, QStringLiteral("Close"));
		const bool inside = close && close->isVisible() &&
				    d.rect().contains(QRect(close->mapTo(&d, QPoint(0, 0)), close->size()));
		ok(inside, "its buttons are inside it");
		ok(d.reportBox()->toPlainText() == report, "the whole report is in the box");
		ok(d.reportBox()->verticalScrollBar()->maximum() > 0, "and the box scrolls");
	}

	std::printf("\n-- the buttons --\n");
	{
		CrashReportDialog d(report);
		d.show();
		QApplication::clipboard()->clear();
		d.copyButton()->click();
		ok(QApplication::clipboard()->text() == report, "Copy report: every line on the clipboard");
		ok(d.copyButton()->text() == QStringLiteral("Copied"), "and the button says so");
		ok(d.isVisible(), "the window stays open");
	}
	{
		CrashReportDialog d(report);
		QTimer::singleShot(100, &d, [&d]() {
			if (QPushButton *b = buttonNamed(d, QStringLiteral("Open logs folder")))
				b->click();
		});
		d.exec();
		ok(d.openLogsChosen(), "Open logs folder: closes, and says so");
	}
	{
		CrashReportDialog d(report);
		QTimer::singleShot(100, &d, [&d]() {
			if (QPushButton *b = buttonNamed(d, QStringLiteral("Close")))
				b->click();
		});
		const int r = d.exec();
		ok(r == QDialog::Rejected && !d.openLogsChosen(), "Close: closes, no logs");
	}
	{
		CrashReportDialog d(report);
		QTimer::singleShot(100, &d, [&d]() { QTest::keyClick(&d, Qt::Key_Escape); });
		const int r = d.exec();
		ok(r == QDialog::Rejected && !d.openLogsChosen(), "Escape: closes, no logs");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
