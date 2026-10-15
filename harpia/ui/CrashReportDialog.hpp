#pragma once

// "Harpia crashed last time": what happened, in a few lines, and the whole
// report -- every line of the stack -- in a box that scrolls.
//
// It used to be a QMessageBox with the report as its text. That grows with
// the report, and a full stack is taller than a screen, so the buttons ended
// up below the bottom of it and the window could not be closed. This one is
// never taller than most of the screen it opens on, and its report can be
// copied in one click to send to whoever fixes it.

#include <QDialog>
#include <QString>

class QPlainTextEdit;
class QPushButton;

namespace harpia {

class CrashReportDialog : public QDialog {
public:
	explicit CrashReportDialog(const QString &report, QWidget *parent = nullptr);

	// "Open logs folder" was chosen (the dialog closes with it).
	bool openLogsChosen() const { return openLogs_; }

	// The report's opening lines -- the app and version, when, and what went
	// wrong -- without the stack ("Stack ..." and everything after it).
	static QString summaryOf(const QString &report);

	QPlainTextEdit *reportBox() const { return box_; }
	QPushButton *copyButton() const { return copy_; }

private:
	QPlainTextEdit *box_ = nullptr;
	QPushButton *copy_ = nullptr;
	bool openLogs_ = false;
};

} // namespace harpia
