#pragma once

#include <QStringList>
#include <QWidget>

class QComboBox;
class QLabel;
class QCheckBox;
class QTextEdit;

namespace harpia {

// Viewer for the runtime logs written by Logger. Lists the current and previous
// sessions' log files, shows the full path, lets the user filter by
// Errors/Warnings/Info, highlights the most recent error, and offers
// copy/clear/open-file/open-folder actions. Reads the on-disk files directly.
class ErrorLogsPanel : public QWidget {
	Q_OBJECT
public:
	explicit ErrorLogsPanel(QWidget *parent = nullptr);

public slots:
	void refresh(); // reload the session list + current file

protected:
	void showEvent(QShowEvent *) override;

private slots:
	void onSessionChanged();
	void applyFilter();
	void openLogFile();
	void openLogFolder();
	void copyToClipboard();
	void clearLogs();

private:
	void loadSelectedFile();

	QComboBox *sessionCombo_ = nullptr;
	QLabel *pathLabel_ = nullptr;
	QCheckBox *errorCheck_ = nullptr;
	QCheckBox *warningCheck_ = nullptr;
	QCheckBox *infoCheck_ = nullptr;
	QTextEdit *view_ = nullptr;

	QStringList lines_; // raw lines of the currently-selected file
};

} // namespace harpia
