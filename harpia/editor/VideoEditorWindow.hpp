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
class QStackedWidget;
class QTimer;

namespace harpia {

class FrameSeeker;
class PreviewCanvas;
class Timeline;
class TrackEditor;
class TimelineThumbs;
class ClipExporter;

// A lightweight built-in video editor with two modes:
//  - Simple Trim: one start/end range on a timeline (with live frame preview
//    under the handles), a single playback speed, optional crop.
//  - Multi-Cut: drag on a Source track to select sections to keep; each cut
//    becomes a segment on an Output track (reorder by dragging, delete, and a
//    per-cut playback speed via the speed slider). Play previews the assembled
//    output; export concatenates the cuts (audio pitch-corrected per speed).
// Exports to a new file (MP4/GIF/MKV/MOV/WebM) — the original is never
// modified. Emits exported() with the new file's path so the owner can refresh
// the Clips Library.
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
	void onSegmentsChanged();
	void onSegmentSelected(int index);

private:
	void showFrame(qint64 ms);
	void joinExport();
	void startPlayback();
	void stopPlayback();
	void setEditMode(bool multiCut);
	bool multiCut() const;
	void updateInfoLabel();

	QString inPath_;
	bool valid_ = false;

	std::unique_ptr<FrameSeeker> seeker_;
	TimelineThumbs *stripThumbs_ = nullptr; // background filmstrip decoder
	PreviewCanvas *canvas_ = nullptr;
	Timeline *timeline_ = nullptr;
	TrackEditor *tracks_ = nullptr;
	QStackedWidget *stack_ = nullptr;
	QPushButton *trimModeBtn_ = nullptr;
	QPushButton *cutModeBtn_ = nullptr;
	QCheckBox *cropToggle_ = nullptr;
	QLabel *infoLabel_ = nullptr;
	QString baseInfo_; // static file info; extended with cut stats in Multi-Cut

	QPushButton *playBtn_ = nullptr;
	QSlider *speedSlider_ = nullptr;
	QLabel *speedLabel_ = nullptr;
	double speed_ = 1.0;

	QTimer *previewTimer_ = nullptr;
	qint64 pendingMs_ = -1;

	// Looping playback. Simple Trim: the trimmed region at the global speed
	// (playAnchorMs_ = source ms at clock zero). Multi-Cut: the assembled output
	// (playAnchorMs_ = OUTPUT-time ms at clock zero; playSeg_ tracks the segment
	// currently being decoded so segment changes trigger one seek).
	QTimer *playTimer_ = nullptr;
	bool playing_ = false;
	QElapsedTimer playClock_;
	qint64 playAnchorMs_ = 0;
	int playSeg_ = -1;

	ClipExporter *exporter_ = nullptr;
	std::thread exportThread_;
	QProgressDialog *progress_ = nullptr;
	QString outPath_;
};

} // namespace harpia
