#pragma once

#include "TrackEditor.hpp"        // CutSegment (stored in EditorSnapshot)
#include "VoiceoverTrack.hpp"     // VoiceoverClip (stored in EditorSnapshot)
#include "shader/ShaderEffect.hpp" // ShaderState / ShaderParam (post-processing)

#include <QDialog>
#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QProgressDialog;
class QPushButton;
class QSlider;
class QStackedWidget;
class QTimer;
class QFileSystemWatcher;
class QDragEnterEvent;
class QDropEvent;
class QEvent;
class QShowEvent;

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
	QVector<ShaderState> effects; // post-processing stack (name + params), in order

	bool operator==(const EditorSnapshot &o) const
	{
		return segments == o.segments && trimStart == o.trimStart && trimEnd == o.trimEnd &&
		       speed == o.speed && cropEnabled == o.cropEnabled && cropRect == o.cropRect &&
		       voiceClips == o.voiceClips && effects == o.effects;
	}
};

// One live effect in the post-processing stack: the persisted state (name +
// param values) plus the parsed uniform defs and the wrapped GLSL, cached so the
// controls and the GPU chain can be rebuilt without re-reading the file.
struct EditorEffect {
	QString name;
	QMap<QString, double> params;
	QVector<ShaderParam> defs;
	QString wrapped; // wrapped fragment shader (for the renderer + export)
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
class ThumbnailCache;
class ShaderRenderer;
class TimelineView;

// One video the editor can cut from. The first source is the file the editor
// was launched on; more are added via the sidebar / drag-and-drop. Each owns
// its own decoder and background filmstrip so switching the active source is
// instant and cuts from any source preview/play correctly.
struct EditorSource {
	int id = 0; // stable, monotonic
	QString path;
	QString name;
	qint64 durationMs = 0;
	int width = 0;
	int height = 0;
	std::unique_ptr<FrameSeeker> seeker;
	TimelineThumbs *thumbs = nullptr; // parented to the window
	QVector<QImage> thumbCache;       // last emitted filmstrip
};

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
	// libraryFolders: the app's recording output folders, so the Sources panel's
	// "Library" tab can offer existing recordings to add as sources.
	explicit VideoEditorWindow(const QString &inPath, const QStringList &libraryFolders = {},
				   QWidget *parent = nullptr);
	~VideoEditorWindow() override;

	bool isValid() const { return valid_; }

	// Called for every close path (title-bar X, Close button, Esc). When there
	// are unsaved edits, asks: Cancel (keep editing) or Close window (discard).
	void reject() override;

protected:
	// Drag-and-drop video files onto the editor to add them as sources.
	void dragEnterEvent(QDragEnterEvent *e) override;
	void dropEvent(QDropEvent *e) override;
	void showEvent(QShowEvent *e) override;              // first-show: place Sources panel
	bool eventFilter(QObject *watched, QEvent *e) override; // sync toggle when panel closed

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
	void onAddSource();          // "Add video…" button / sidebar
	void onSourceRowChanged();   // sidebar selection → setActiveSource
	void onSourceDoubleClicked(QListWidgetItem *item); // append whole clip as a cut
	void onRemoveSource();       // remove the selected source (if unused)
	void refreshLibrary();       // rescan the library folders into the Library tab
	void onLibraryDoubleClicked(QListWidgetItem *item); // add a library clip as a source
	void onThumbReady(const QString &path);             // library thumbnail decoded

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

	void showFrame(int sourceId, qint64 ms);
	void joinExport();
	void startPlayback();
	void stopPlayback();
	// The three editor modes map 1:1 onto stack_ page indices.
	enum class EditMode { Trim = 0, MultiCut = 1, Full = 2 };
	void setEditMode(EditMode mode);
	EditMode mode() const;
	bool multiCut() const;   // mode() == MultiCut
	bool fullEdit() const;   // mode() == Full
	// Full-mode timeline preview: resolve an output-time to the topmost video
	// clip's (source, source-ms) and show it. Returns false if nothing is there.
	void onTimelineScrub(qint64 outMs);
	void onTimelineHoverScrub(qint64 outMs);
	bool timelineFrameAt(qint64 outMs, int *sourceId, qint64 *srcMs) const;
	void addActiveSourceToTimeline(); // "Add to timeline" for the active source
	void updateInfoLabel();
	bool hasUnsavedEdits() const;

	QString inPath_;
	bool valid_ = false;

	// ---- Sources (multi-video mixing) ----
	std::vector<EditorSource> sources_;
	int activeSourceId_ = -1;
	int nextSourceId_ = 0;
	QListWidget *sourceList_ = nullptr;      // "Sources" tab of the floating panel
	QListWidget *libraryList_ = nullptr;     // "Library" tab (existing recordings)
	QStringList libraryFolders_;             // app recording folders to scan
	ThumbnailCache *thumbCache_ = nullptr;   // async thumbnails for the Library tab
	QWidget *sourcesPanel_ = nullptr;        // floating, toggleable tool window
	QPushButton *sourcesBtn_ = nullptr;      // toolbar toggle
	bool sourcesPlaced_ = false;             // has the panel been positioned yet?
	bool sourcesFirstShown_ = false;         // gate the one-time initial show
	void showSourcesPanel();                 // position (first time) + show + raise
	EditorSource *sourceById(int id);
	EditorSource *activeSource();
	FrameSeeker *seekerFor(int id); // nullptr if unknown
	int addSource(const QString &path); // opens + starts filmstrip; -1 on failure
	void setActiveSource(int id);       // swap what the Source track/preview shows
	void refreshSourceList();           // rebuild the sidebar rows
	bool sourceInUse(int id) const;     // any cut references this source?

	FrameSeeker *seeker_ = nullptr; // non-owning: the ACTIVE source's decoder
	PreviewCanvas *canvas_ = nullptr;
	Timeline *timeline_ = nullptr;
	TrackEditor *tracks_ = nullptr;
	TimelineView *timelineView_ = nullptr; // "Full editing" multi-track timeline
	QStackedWidget *stack_ = nullptr;
	QPushButton *trimModeBtn_ = nullptr;
	QPushButton *cutModeBtn_ = nullptr;
	QPushButton *fullModeBtn_ = nullptr;
	QCheckBox *cropToggle_ = nullptr;
	QPushButton *audioHeader_ = nullptr; // voiceover disclosure (hidden in Full mode)
	QWidget *audioBody_ = nullptr;
	bool audioExpandedBeforeFull_ = false; // restore audio foldout when leaving Full
	bool timelineSeeded_ = false;          // seeded the timeline with the first source once
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

	// ---- Post-processing effect stack (preview + baked into export) ----
	// An ordered list of GLSL effects drives both the live preview (via
	// shaderRenderer_, a GUI-thread offscreen GL chain) and export (the same
	// wrapped GLSL layers are handed to ClipExporter). Shaders are `.frag` files
	// in a user-writable folder; each declares its own tunable //@param controls.
	std::unique_ptr<ShaderRenderer> shaderRenderer_;
	QVector<EditorEffect> effects_;             // the stack, applied in order
	QWidget *effectsBox_ = nullptr;             // container listing the stack (rebuilt)
	QLabel *shaderError_ = nullptr;             // compile errors / "no GPU" notice
	QPushButton *addEffectBtn_ = nullptr;       // "Add effect ▾" (menu of shaders)
	QPushButton *effectsBtn_ = nullptr;         // toolbar toggle (opens the panel)
	QFileSystemWatcher *shaderWatch_ = nullptr; // live-reload the active .frag files
	QString shadersDir_;                        // the user-writable shaders folder
	bool effectsRebuilding_ = false;            // guard while rebuilding controls
	QString shadersDirPath();                   // ensure + return the folder (seeds presets)
	QStringList availableShaders();             // .frag stems in the folder
	bool loadShaderFile(const QString &name, EditorEffect *out, QString *err); // parse+wrap
	void addEffect(const QString &name);        // append a layer + recompile + preview
	void removeEffect(int index);
	void moveEffect(int index, int delta);      // reorder within the stack
	void recompileChain();                      // push effects_ to the GPU renderer
	void rebuildEffectsUI();                    // (re)build the stack's widgets
	void updateShaderWatch();                   // watch every active layer's .frag
	QImage runShader(const QImage &img, qint64 ms);     // run the chain over one frame
	void setPreviewFrame(const QImage &img, qint64 ms); // stash raw + show filtered
	void refreshPreviewFrame();                 // re-filter the stashed frame (effect changed)
	QImage lastPreviewRaw_;                     // last decoded (unfiltered) preview frame
	qint64 lastPreviewMs_ = 0;

	QPushButton *playBtn_ = nullptr;
	QPushButton *undoBtn_ = nullptr;
	QPushButton *redoBtn_ = nullptr;

	// Every button in the editor shares a Dev-tunable height. Collected once at
	// construction (before the Developer Panel exists) so its own buttons are
	// never resized.
	QList<QPushButton *> uiButtons_;
	// Apply the Dev-tunable window chrome (button height, text sizes, speed
	// slider/value widths) live.
	void applyChrome(const struct EditorChromeParams &p);
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
	int pendingSource_ = -1; // source for the pending preview frame

	// Looping playback. Simple Trim: the trimmed region at the global speed
	// (playAnchorMs_ = source ms at clock zero). Multi-Cut: the assembled output
	// (playAnchorMs_ = OUTPUT-time ms at clock zero; playSeg_ tracks the segment
	// currently being decoded so segment changes trigger one seek).
	QTimer *playTimer_ = nullptr;
	bool playing_ = false;
	QElapsedTimer playClock_;
	qint64 playAnchorMs_ = 0;
	int playSeg_ = -1;
	int playSourceId_ = -1; // source of the segment currently being decoded

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
