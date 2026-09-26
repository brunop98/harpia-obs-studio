#pragma once

// The yt-dlp settings, as a widget so the same controls can sit in a dialog
// (from the download window's "Settings…") and as a page of the preset
// editor. Every change is saved as it is made, so there is no OK to forget.

#include "YtDlp.hpp"

#include <QDialog>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;

namespace harpia {

class YtDlpSettingsWidget : public QWidget {
	Q_OBJECT
public:
	explicit YtDlpSettingsWidget(QWidget *parent = nullptr);
	void reload();

signals:
	void changed();

private:
	void save();
	void runTest();

	QLineEdit *exePath_ = nullptr;
	QLabel *state_ = nullptr;
	QComboBox *cookiesBrowser_ = nullptr;
	QLineEdit *cookiesFile_ = nullptr;
	QLineEdit *downloadDir_ = nullptr;
	QCheckBox *preferMp4_ = nullptr;
	QLineEdit *extraArgs_ = nullptr;
	bool loading_ = false;
};

class YtDlpSettingsDialog : public QDialog {
	Q_OBJECT
public:
	explicit YtDlpSettingsDialog(QWidget *parent = nullptr);
};

} // namespace harpia
