#pragma once

// Add ▸ Video from URL…: paste a link, see what it is, pick a quality, and
// the download lands on the timeline.
//
// Two steps on purpose. The first fetches the video's facts (title, channel,
// duration, thumbnail, the qualities actually on offer, the subtitle
// languages) and shows them, so what is about to be downloaded is confirmed
// before a byte of it is. The second downloads with a progress bar, and on
// success hands the file to the editor. yt-dlp does the work in a process;
// this dialog only runs it and reads what it prints (YtDlp.hpp).

#include "YtDlp.hpp"

#include <QDialog>
#include <QNetworkAccessManager>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProcess;
class QProgressBar;
class QPushButton;

namespace harpia {

class UrlDownloadDialog : public QDialog {
	Q_OBJECT
public:
	explicit UrlDownloadDialog(QWidget *parent = nullptr);
	~UrlDownloadDialog() override;

	// Pre-fill (from the clipboard, say). Fetches at once when it is a URL.
	void setUrl(const QString &url);

signals:
	// The finished file, and what yt-dlp said it was.
	void downloaded(const QString &path, const YtVideoInfo &info);

private:
	enum class Stage { Idle, Fetching, Ready, Downloading, Done };
	void buildUi();
	void fetchInfo();
	void showInfo(const YtVideoInfo &v);
	void startDownload();
	void onDownloadLine(const QString &line);
	void finishDownload(int exitCode);
	void setStage(Stage s);
	void openSettings();
	void refreshFolder();
	void loadThumbnail(const QString &url);

	YtDlpSettings settings_;
	YtVideoInfo info_;
	Stage stage_ = Stage::Idle;
	QProcess *proc_ = nullptr;
	QNetworkAccessManager nam_;
	QString printPathFile_;
	QString lastError_;
	QByteArray fetchOut_;

	QLineEdit *url_ = nullptr;
	QPushButton *fetch_ = nullptr;
	QLabel *thumb_ = nullptr;
	QLabel *title_ = nullptr;
	QLabel *meta_ = nullptr;
	QLabel *subsInfo_ = nullptr;
	QWidget *infoBox_ = nullptr;
	QWidget *optionsBox_ = nullptr;
	QComboBox *quality_ = nullptr;
	QCheckBox *audio_ = nullptr;
	QComboBox *subtitles_ = nullptr;
	QLabel *folder_ = nullptr;
	QProgressBar *progress_ = nullptr;
	QLabel *status_ = nullptr;
	QPushButton *download_ = nullptr;
	QPushButton *cancel_ = nullptr;
};

} // namespace harpia
