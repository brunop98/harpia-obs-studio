#pragma once

#include "core/ShareExporter.hpp"

#include <QDialog>
#include <QString>

#include <thread>

class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;

namespace harpia {

// Modal window that produces an internet-sharing copy of a recording. It runs a
// ShareExporter on a background thread and shows a live progress page (bar, %,
// speed, ETA, growing output size, Cancel). On success it flips to a summary
// page (original vs optimized size, space saved, Open Folder / Copy File / Copy
// Path). The original recording is never modified.
class ShareExportDialog : public QDialog {
	Q_OBJECT
public:
	// Convenience: derive the `<name>_shared.mp4` path next to `inPath`, build the
	// dialog and run it modally.
	static void runModal(const QString &inPath, ShareExporter::Level level, QWidget *parent);

	ShareExportDialog(const QString &inPath, const QString &outPath, ShareExporter::Level level,
			  QWidget *parent = nullptr);
	~ShareExportDialog() override;

protected:
	void closeEvent(QCloseEvent *event) override; // closing mid-encode cancels

private slots:
	void onProgress(int percent, double speed, qint64 etaMs, qint64 outBytes);
	void onFinished(bool ok, bool canceled, const QString &error);
	void onCancel();

private:
	void startEncoding();
	void buildSummary();
	void joinWorker();

	QString inPath_;
	QString outPath_;
	ShareExporter::Level level_;
	qint64 originalBytes_ = 0;
	qint64 finalBytes_ = 0;

	ShareExporter *exporter_ = nullptr;
	std::thread worker_;
	bool canceling_ = false;
	bool done_ = false;

	QStackedWidget *stack_ = nullptr;

	// Progress page
	QLabel *titleLabel_ = nullptr;
	QProgressBar *bar_ = nullptr;
	QLabel *pctLabel_ = nullptr;
	QLabel *speedLabel_ = nullptr;
	QLabel *etaLabel_ = nullptr;
	QLabel *sizeLabel_ = nullptr;
	QPushButton *cancelBtn_ = nullptr;

	// Summary page
	QLabel *summaryLabel_ = nullptr;
};

} // namespace harpia
