#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QString>

#include <memory>
#include <thread>

class QCheckBox;
class QLabel;
class QProgressDialog;
class QPushButton;
class QSlider;
class QTimer;

namespace harpia {

class FrameSeeker;
class PreviewCanvas;
class Timeline;
class ClipExporter;

// A lightweight built-in video editor: trim on a timeline (with live frame
// preview under the handles), optionally crop the image, then export a new file
// (MP4/GIF/MKV/MOV/WebM) — the original is never modified. Emits exported() with
// the new file's path so the owner can refresh the Clips Library.
class VideoEditorWindow : public QDialog {
	Q_OBJECT
public:
	explicit VideoEditorWindow(const QString &inPath, QWidget *parent = nullptr);
	~VideoEditorWindow() override;

	bool isValid() const { return valid_; }

signals:
	void exported(const QString &path);

private slots:
	void onScrub(qint64 ms);
	void onPreviewTick();
	void onCropToggled(bool on);
	void onSave();
	void onExportProgress(int pct, qint64 etaMs, qint64 bytes);
	void onExportFinished(bool ok, bool canceled, const QString &err);
	void onPlayPause();
	void onPlayTick();
	void onSpeedChanged(int sliderValue);

private:
	void showFrame(qint64 ms);
	void joinExport();
	void startPlayback();
	void stopPlayback();

	QString inPath_;
	bool valid_ = false;

	std::unique_ptr<FrameSeeker> seeker_;
	PreviewCanvas *canvas_ = nullptr;
	Timeline *timeline_ = nullptr;
	QCheckBox *cropToggle_ = nullptr;
	QLabel *infoLabel_ = nullptr;

	QPushButton *playBtn_ = nullptr;
	QSlider *speedSlider_ = nullptr;
	QLabel *speedLabel_ = nullptr;
	double speed_ = 1.0;

	QTimer *previewTimer_ = nullptr;
	qint64 pendingMs_ = -1;

	// Looping playback of the trimmed region at the current speed.
	QTimer *playTimer_ = nullptr;
	bool playing_ = false;
	QElapsedTimer playClock_;
	qint64 playAnchorMs_ = 0; // source ms at playClock_ == 0

	ClipExporter *exporter_ = nullptr;
	std::thread exportThread_;
	QProgressDialog *progress_ = nullptr;
	QString outPath_;
};

} // namespace harpia
