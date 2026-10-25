#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QString>

class QCloseEvent;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTimer;

namespace harpia {

// Export remains visible until its worker has stopped and its owner has
// finished handling the output file. Cancel, Escape and the title-bar close
// button all request cancellation; none dismisses a running export.
class ExportProgressDialog : public QDialog {
	Q_OBJECT
public:
	ExportProgressDialog(const QString &outputPath, const QString &settingsSummary,
			     QWidget *parent = nullptr);

public slots:
	void setStage(const QString &stage);
	void setEncoding(const QString &description);
	void setFrameProgress(qint64 completed, qint64 total);
	void setProgress(int percent, qint64 etaMs, qint64 bytes);
	void setBytes(qint64 bytes);
	void setOutputPath(const QString &path);
	// index is zero-based, matching BatchExporter::progress.
	void setItemProgress(int index, int count);
	void finish(bool success);
	void reject() override;

signals:
	void cancelRequested();

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	void requestCancel();
	void refreshTiming();
	void refreshFrames();

	QLineEdit *filename_ = nullptr;
	QLineEdit *destination_ = nullptr;
	QLabel *item_ = nullptr;
	QLabel *stage_ = nullptr;
	QLabel *encoding_ = nullptr;
	QProgressBar *progress_ = nullptr;
	QLabel *elapsed_ = nullptr;
	QLabel *remaining_ = nullptr;
	QLabel *frames_ = nullptr;
	QLabel *speed_ = nullptr;
	QLabel *bytes_ = nullptr;
	QLabel *cancelHint_ = nullptr;
	QPushButton *cancel_ = nullptr;
	QTimer *timer_ = nullptr;
	QElapsedTimer elapsedClock_;
	QElapsedTimer stageClock_;
	qint64 completedFrames_ = 0;
	qint64 totalFrames_ = 0;
	qint64 maxBytes_ = 0;
	double smoothedEtaMs_ = -1;
	qint64 etaUpdatedMs_ = 0;
	int currentItem_ = -1;
	bool canceling_ = false;
	bool finished_ = false;
};

} // namespace harpia
