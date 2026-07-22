#pragma once

#include "TrackEditor.hpp"    // CutSegment (stored in EditorSnapshot)
#include "VoiceoverTrack.hpp" // VoiceoverClip (stored in EditorSnapshot)

#include <QDialog>
#include <QElapsedTimer>
#include <QRect>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>
#include <thread>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressDialog;
class QPushButton;
class QSlider;
class QStackedWidget;
class QTimer;

namespace harpia {

// A full snapshot of the editor's undoable state. Undo/redo restores one of
// these; snapshot-based history is simple and robust for this size of state.
struct EditorSnapshot {
	QVector<CutSegment> segments;
	qint64 trimStart = 0;
	qint64 trimEnd = 0;
	double speed = 1.0;
	bool cropEnabled = false;
	QRect cropRect;
	QVector<VoiceoverClip> voiceClips;

	bool operator==(const EditorSnapshot &o) const
	{
		return segments == o.segments && trimStart == o.trimStart && trimEnd == o.trimEnd &&
		       speed == o.speed && cropEnabled == o.cropEnabled && cropRect == o.cropRect &&
		       voiceClips == o.voiceClips;
	}
};

class AudioRecorder;
class DevPanel;
class FrameSeeker;
class LevelMeter;
class PreviewCanvas;
class VoiceoverTrack;
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

	// Called for every close path (title-bar X, Close button, Esc). When there
	// are unsaved edits, asks: Cancel (keep editing) or Close window (discard).
	void reject() override;

signals:
	void exported(const QString &path);

private slots:
	void onScrub(qint64 ms);
	void onHoverScrub(qint64 ms); // hover preview — never interrupts playback
	void onPreviewTick();
	void onCropToggled(bool on);
	void onSave();
	void onExportProgress(int pct, qint64 etaMs, qint64 bytes);
	void onExportFinished(bool ok, bool canceled, const QString &err);
	void onPlayPause();
	void onResetMarker(); // move the playhead back to the start
	void onPlayTick();
	void undo();
	void redo();
	void onSpeedChanged(int sliderValue); // slider moved (exponential mapping)
	void onSpeedSpinChanged(double value); // typed into the speed box
	void onSegmentsChanged();
	void onSegmentSelected(int index);
	void onVoiceoverRecordClicked();
	void onImportAudioClicked();
	void onAutoCut(); // source-track "Auto-cut on scene changes"
	void onSaveProject();
	void onOpenProject();

private:
	// Apply a speed value (multi-cut: to the selection; trim: global) and refresh
	// the count label. Callers keep the slider + spin box in sync.
	void applySpeed(double value);
	void syncSpeedControls(double value); // set slider + spin without re-applying

	// Undo/redo history (snapshot-based, coalesced via histTimer_).
	EditorSnapshot snapshot() const;
	void restoreSnapshot(const EditorSnapshot &s);
	void scheduleSnapshot();  // debounced: records after edits settle
	void captureSnapshot();   // histTimer_ fired — push if changed
	void updateUndoRedoButtons();

	// Voiceover helpers.
	void startVoiceoverCapture();     // actually opens the mic + (talk-along) plays
	void finishVoiceover();           // stop mic, create a clip from the take
	void updateVoiceoverAxis();       // keep the track's output-duration in sync
	qint64 outputDurationMs() const;  // trimmed/assembled output length
	qint64 currentOutputMs() const;   // output-time under the playhead right now
	QString voiceoverTempDir();       // per-session temp dir for takes (lazy)
	void onSceneDetected(const QVector<qint64> &cutMs, const QString &err); // auto-cut result

	void showFrame(qint64 ms);
	void joinExport();
	void startPlayback();
	void stopPlayback();
	void setEditMode(bool multiCut);
	bool multiCut() const;
	void updateInfoLabel();
	bool hasUnsavedEdits() const;

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
	DevPanel *devPanel_ = nullptr; // lazily created, non-modal
	QLabel *infoLabel_ = nullptr;
	QLabel *cursorTimeLabel_ = nullptr; // preview time under the cursor/playhead

	// Right-side inspector (fills the dead space beside portrait previews):
	// live properties of the selected cut / trim range. Collapsible.
	QWidget *inspector_ = nullptr;
	QPushButton *inspectorBtn_ = nullptr; // toolbar toggle (show/hide the panel)
	QLabel *inspTitle_ = nullptr;
	QLabel *inspInMs_ = nullptr;
	QLabel *inspOutMs_ = nullptr;
	QLabel *inspSrcLen_ = nullptr;
	QLabel *inspSpeed_ = nullptr;
	QLabel *inspOutLen_ = nullptr;
	QLabel *inspHint_ = nullptr;
	void updateInspector(); // refresh the inspector from the current selection
	QString baseInfo_; // static file info; extended with cut stats in Multi-Cut

	QPushButton *playBtn_ = nullptr;
	QPushButton *undoBtn_ = nullptr;
	QPushButton *redoBtn_ = nullptr;
	QVector<EditorSnapshot> history_;
	int histIndex_ = -1;    // current position in history_
	bool restoring_ = false; // guard: restoring must not schedule new snapshots
	QTimer *histTimer_ = nullptr;
	QSlider *speedSlider_ = nullptr;
	QDoubleSpinBox *speedSpin_ = nullptr; // editable numeric speed (text input)
	QLabel *speedLabel_ = nullptr;        // "(N cuts)" / "—" status next to it
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

	// Auto-cut (scene detection) runs on a worker thread.
	std::thread sceneThread_;
	std::atomic<bool> sceneCancel_{false};
	QProgressDialog *sceneProgress_ = nullptr;

	// ---- Voiceover (narration recorded over the video) ----
	VoiceoverTrack *voTrack_ = nullptr;
	AudioRecorder *voRecorder_ = nullptr;
	QComboBox *voDevice_ = nullptr;
	QPushButton *voRecordBtn_ = nullptr;
	QPushButton *voImportBtn_ = nullptr;
	LevelMeter *voMeter_ = nullptr;
	QCheckBox *voTalkAlong_ = nullptr; // play the video while capturing
	QCheckBox *voCountdown_ = nullptr; // 3-2-1 before capture
	QSlider *voOrigVol_ = nullptr;     // original-audio level at export
	QLabel *voOrigVolLabel_ = nullptr;
	QCheckBox *voDuck_ = nullptr;      // auto-duck original under narration
	QLabel *voStatus_ = nullptr;
	QTimer *voCountdownTimer_ = nullptr;
	int voCountdownLeft_ = 0;
	bool voRecording_ = false;
	qint64 voClipStartMs_ = 0; // output-time where the in-progress take begins
	QString voTempDir_;        // holds the session's WAV takes; removed on close
};

} // namespace harpia
