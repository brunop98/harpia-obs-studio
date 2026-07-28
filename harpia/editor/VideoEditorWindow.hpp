#pragma once

#include "TrackEditor.hpp"        // CutSegment (stored in EditorSnapshot)
#include "VoiceoverTrack.hpp"     // VoiceoverClip (stored in EditorSnapshot)
#include "shader/ShaderEffect.hpp"    // ShaderState / ShaderParam (post-processing)
#include "timeline/TimelineModel.hpp" // TlClip / TlTransform (Full-editing timeline)
#include "DevPanel.hpp"                 // EditorChromeParams / EditorInspectorParams
#include "timeline/TimelineView.hpp"    // ClipboardEntry (timeline copy/paste)

#include <QDateTime>
#include <QDialog>
#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QProgressDialog;
class QPushButton;
class QSlider;
class QSplitter;
class QStackedWidget;
class QTimer;
class QFileSystemWatcher;
class QSpinBox;
class QPlainTextEdit;
class QFontComboBox;
class QVBoxLayout;
class QLineEdit;
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
	TimelineModel timeline;       // "Full editing" multi-track timeline

	bool operator==(const EditorSnapshot &o) const
	{
		return segments == o.segments && trimStart == o.trimStart && trimEnd == o.trimEnd &&
		       speed == o.speed && cropEnabled == o.cropEnabled && cropRect == o.cropRect &&
		       voiceClips == o.voiceClips && effects == o.effects && timeline == o.timeline;
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
class KeyframeEditor;
class TransformEvaluator;
class AudioPreview;

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

	// The floating keyframe editor, created the first time it is asked for and
	// then kept, so it remembers its size and place.
	KeyframeEditor *keyEditor_ = nullptr;
	void openKeyframeEditor();
	void refreshKeyframeEditor(); // follow the selection / playhead

	// Transform rows (Zoom / Pos X / Pos Y / Rotation / Opacity) in order, so a
	// script running on the clip can mark the ones it drives.
	QVector<QLabel *> poseLabels_;
	void markScriptDrivenRows(const TlClip &c);

	// Preview request pacing (see requestPreview in the .cpp): render the first
	// request at once, then at most one per timer interval, always the newest.
	void requestPreview(int sourceId, qint64 ms);
	void renderPendingPreview();

	// The Unsaved Changes prompt. Returns true when the caller may close.
	bool confirmDiscardOnClose();
	QString autosaveProjectPath() const; // "" when there is no project path yet

	// Undo/redo history (snapshot-based, coalesced via histTimer_).
	EditorSnapshot snapshot() const;
	void restoreSnapshot(const EditorSnapshot &s);
	void scheduleSnapshot();  // debounced: records after edits settle
	void captureSnapshot(); // histTimer_ fired — push if changed
	void commitSnapshot();  // capture immediately (a finished, discrete action)
	void updateUndoRedoButtons();

	// Voiceover helpers.
	void startVoiceoverCapture();     // actually opens the mic + (talk-along) plays
	void finishVoiceover();           // stop mic, create a clip from the take
	void updateVoiceoverAxis();       // keep the track's output-duration in sync
	qint64 outputDurationMs() const;  // trimmed/assembled output length
	qint64 currentOutputMs() const;   // output-time under the playhead right now
	QString voiceoverTempDir();       // per-session temp dir for takes (lazy)
	void onSceneDetected(const QVector<qint64> &cutMs, const QString &err); // auto-cut result

	// Show/hide the right-hand properties panel, opening it wide enough that its
	// own controls are not clipped.
	void showInspector(bool on);
	// Size the preview/editing split to what the current mode actually needs.
	void applyModeSplit();

	// Timeline clipboard. Held by the window, not the view, so it survives
	// selection changes and outlives any one clip.
	void copySelectedClips(bool cut);
	void pasteClips();
	QVector<TimelineView::ClipboardEntry> clipboard_;
	QSplitter *hsplit_ = nullptr;   // preview+editing | inspector
	QSplitter *vsplit_ = nullptr;   // preview / editing area
	QWidget *bottomPane_ = nullptr; // mode bar + mode stack + audio + buttons

	void refreshPreviewAtPlayhead(); // re-render at the current position
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
	// Full-mode timeline preview: composite every visible track at an output time
	// and push the result (through the effect chain) into the preview canvas.
	void onTimelineScrub(qint64 outMs);
	void onTimelineHoverScrub(qint64 outMs);
	void showTimelineFrame(qint64 outMs);
	QSize timelineCanvasSize() const; // primary source's resolution (fallback 1920x1080)
	void addActiveSourceToTimeline(); // "Add to timeline" for the active source
	// ---- Audio on the timeline (Full editing) ----
	// Register an audio-only file as a source (decoded to a session WAV so the
	// waveform, trimming and the export mix all work uniformly). -1 on failure.
	int addAudioSource(const QString &path);
	// The 48k WAV proxy for a source's audio, decoding it on first use. Empty if
	// that source has no audio. Used for clip waveforms.
	QString audioProxyFor(int sourceId);
	QHash<int, QString> audioProxy_;
	// ---- Still images as clips ----
	// Registered as sources with no decoder; the frame providers serve the cached
	// QImage for every timestamp. -1 on failure.
	int addImageSource(const QString &path);
	QHash<int, QImage> stillImages_; // sourceId -> decoded still
	void addImageClip();
	void addEffectClip(); // toolbar: drop an effect clip on an effect track

	// Effect-clip Inspector (the selected effect's type, parameters, name).
	void buildEffectInspector(QVBoxLayout *into);
	void syncEffectInspector();
	void rebuildEffectParams();
	QWidget *fxBox_ = nullptr;
	QComboBox *fxType_ = nullptr;
	QLineEdit *fxName_ = nullptr;
	QCheckBox *fxEnabled_ = nullptr;
	QWidget *fxParamBox_ = nullptr;
	QFormLayout *fxParamForm_ = nullptr;
	QVector<QDoubleSpinBox *> fxParamSpins_;
	QStringList fxParamKeys_;
	int fxParamsForType_ = -1;
	bool syncingFx_ = false;             // "Add image…" -> pick a file + drop a clip

	void addAudioClipFromSource(int sourceId); // put a source's audio on an audio lane
	void onAddAudioClicked();                  // "Add audio ▾" menu
	QPushButton *addAudioBtn_ = nullptr;
	QPushButton *addImageBtn_ = nullptr;
	QPushButton *addFxClipBtn_ = nullptr; // "Add effect" -> an effect CLIP
	QString sessionAudioDir(); // per-session temp dir for decoded proxies

	// ---- Direct manipulation of the selected clip in the preview ----
	// Drag repositions, wheel zooms about the cursor. When the clip is
	// keyframed (or auto-key is on) the edit lands on a keyframe at the
	// playhead; otherwise it updates the clip's static pose.
	void onPreviewTransformDrag(double dxNorm, double dyNorm);
	void onPreviewTransformZoom(double factor, double cursorXNorm, double cursorYNorm);
	void applySelectedClipTransform(const TlTransform &tf);
	void syncPreviewTransformTarget(); // arm/disarm + refresh the outline
	qint64 timelinePlayheadMs() const;
	// Playhead clamped inside the selected clip: where an edit applies AND is
	// previewed, so the two can never disagree.
	qint64 timelineEditMs() const;
	double timelineFps() const; // project frame rate (the first source's)

	// ---- Preview quality ----
	// Compositing, decoding and the shader chain all scale with the rendered
	// size, so dropping it is the cheapest way to make a heavy timeline scrub
	// smoothly. The project's real size is untouched — this only affects what is
	// drawn on screen, never the export.
	enum class PreviewQuality { Auto = 0, Full, Half, Quarter };
	PreviewQuality previewQuality_ = PreviewQuality::Auto;
	QComboBox *previewQualityCombo_ = nullptr;
	// Size to render the preview at, for a given project canvas.
	QSize previewRenderSize(QSize canvas) const;
	bool autoKeyframe_ = false; // record a keyframe on every transform edit
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
	void revealInspector(); // "Show in inspector": open the panel + refresh it
	QString baseInfo_; // static file info; extended with cut stats in Multi-Cut

	// ---- Full-editing clip inspector (transform + keyframes + text style) ----
	// Built once and kept in sync with the selected clip (shown only in Full
	// editing when a clip is selected), so dragging a slider never rebuilds it.
	QWidget *clipBox_ = nullptr;
	QDoubleSpinBox *posXSpin_ = nullptr;
	QDoubleSpinBox *posYSpin_ = nullptr;
	QDoubleSpinBox *zoomSpin_ = nullptr;
	QDoubleSpinBox *rotationSpin_ = nullptr;  // degrees about the clip's centre
	QDoubleSpinBox *opacitySpin_ = nullptr;
	QDoubleSpinBox *clipSpeedSpin_ = nullptr; // per-clip playback speed
	QCheckBox *autoKeyChk_ = nullptr;
	QLabel *keyInfo_ = nullptr;
	QWidget *textBox_ = nullptr; // text-clip style controls
	QPlainTextEdit *textEdit_ = nullptr;
	QFontComboBox *fontCombo_ = nullptr;
	QSpinBox *fontSizeSpin_ = nullptr;
	QCheckBox *boldChk_ = nullptr;
	QCheckBox *italicChk_ = nullptr;
	QComboBox *alignCombo_ = nullptr;
	QPushButton *textColorBtn_ = nullptr;
	QDoubleSpinBox *outlineWSpin_ = nullptr;
	QPushButton *outlineColorBtn_ = nullptr;
	QCheckBox *boxChk_ = nullptr;
	QPushButton *boxColorBtn_ = nullptr;
	QSpinBox *boxPadXSpin_ = nullptr;
	QSpinBox *boxPadYSpin_ = nullptr;
	QSpinBox *boxRadiusSpin_ = nullptr;
	QDoubleSpinBox *boxOpacitySpin_ = nullptr;
	QPushButton *addTextBtn_ = nullptr; // bottom controls row, Full mode only
	QPushButton *snapBtn_ = nullptr;    // magnet toggle, Full mode only
	QPushButton *fitBtn_ = nullptr;     // zoom-to-fit, Full mode only
	bool syncingClip_ = false;          // guard while pushing values into the UI

	// ---- Project inspector (metadata for the whole edit, not one clip) ----
	QString projectPath_;      // "" until saved/opened
	QString projectAuthor_;
	QDateTime projectCreated_;
	QDateTime lastAutosave_;
	QTimer *autosaveTimer_ = nullptr;
	QWidget *projectBox_ = nullptr;
	QLabel *pjName_ = nullptr;
	QLabel *pjLocation_ = nullptr;
	QLabel *pjCreated_ = nullptr;
	QLabel *pjSaved_ = nullptr;
	QLabel *pjVersion_ = nullptr;
	QLabel *pjFormat_ = nullptr;   // resolution @ fps
	QLabel *pjSize_ = nullptr;
	QLabel *pjAutosave_ = nullptr;
	QLineEdit *pjAuthor_ = nullptr;
	QCheckBox *autosaveChk_ = nullptr;
	void buildProjectInspector(QVBoxLayout *into);
	void refreshProjectInspector();
	void onSaveProjectAs();
	QString saveProjectTo(const QString &path, bool quiet);
	void doAutosave();
	void revealProjectFolder();
	// A collapsible "▾ Title" section; returns the body to fill in.
	QWidget *addSection(QVBoxLayout *into, const QString &title, bool expanded);

	// ---- Per-clip transform scripting ----
	// The GUI-thread evaluator (the exporter makes its own on its worker). Script
	// files live beside the shaders, with the same //@param convention.
	std::unique_ptr<TransformEvaluator> scriptEval_;
	// The stack: scripts run top-to-bottom, list order IS evaluation order, and
	// the list reorders by drag (InternalMove).
	QListWidget *scriptList_ = nullptr;
	QPushButton *addScriptBtn_ = nullptr;
	int scriptSel_ = -1; // row whose parameters scriptParamBox_ is showing
	bool scriptOrderSyncPending_ = false; // one deferred read per drop, not several
	QWidget *scriptParamBox_ = nullptr;
	QLabel *scriptError_ = nullptr;
	QFileSystemWatcher *scriptWatch_ = nullptr;
	// Clip inspector: a picture clip and an audio clip need different controls,
	// so each group shows only for the kind of clip that has them.
	// Text style presets: a named TlText (style only — a preset never carries
	// the caption's words), stored as readable JSON beside the other user files.
	QComboBox *textPresetCombo_ = nullptr;
	QString textPresetsPath() const;
	QMap<QString, TlText> loadTextPresets() const;
	void saveTextPresets(const QMap<QString, TlText> &presets) const;
	void refreshTextPresets();
	void applyTextPreset(const QString &name);
	void saveTextPresetFromSelection();
	void deleteSelectedTextPreset();

	QWidget *videoClipBox_ = nullptr;
	QWidget *audioClipBox_ = nullptr;
	QDoubleSpinBox *clipVolSpin_ = nullptr;
	QSpinBox *clipFadeInSpin_ = nullptr;
	QSpinBox *clipFadeOutSpin_ = nullptr;
	QComboBox *clipFadeInCurve_ = nullptr;
	QComboBox *clipFadeOutCurve_ = nullptr;

	QString scriptsDir_;
	// Set whenever the timeline or the compiled set changes; makes the per-frame
	// "is every script compiled?" sweep run on change instead of every frame.
	bool scriptScanDirty_ = true;
	QHash<QString, QVector<ShaderParam>> scriptParamDefs_; // per script name
	// Which script scriptParamBox_ currently holds controls for, and handles on
	// those controls. Keeping them lets a refresh update values in place instead
	// of tearing the widgets down — which otherwise happens under the user's
	// cursor mid-drag, and once per mouse-move while reframing in the preview.
	QString scriptParamsBuiltFor_; // "<row>/<script name>"
	QHash<QString, QSlider *> scriptParamSliders_;
	QHash<QString, QLabel *> scriptParamValues_;
	QHash<QString, QCheckBox *> scriptParamChecks_;
	QString scriptsDirPath();          // ensure + seed the folder
	QStringList availableScripts();
	void refreshScriptList(); // selected clip's stack -> the list widget
	bool ensureScriptCompiled(const QString &name, QString *err); // compile into scriptEval_
	void addScriptToClip(const QString &name);   // append to the selected clip's stack
	void removeScriptFromClip(int index);
	void applyScriptOrderFromList();             // after a drag: list order -> clip
	void rebuildScriptParams();        // controls for the selected stack entry
	void reloadScriptsFromDisk();      // live reload: recompile + repaint

	void buildClipInspector(QVBoxLayout *into);

	// Inverse Selection (Spotlight): a project-level effect, so its panel sits
	// beside the project section rather than the clip one.
	void buildSpotlightInspector(QVBoxLayout *into);
	void syncSpotlightInspector();
	void editSpotlight(const std::function<void(SpotlightSpec &)> &fn);
	int selectedMaskRow() const;
	QWidget *spotBox_ = nullptr;
	QCheckBox *spotOn_ = nullptr;
	QCheckBox *spotInvert_ = nullptr;
	QListWidget *spotList_ = nullptr;
	QComboBox *spotShape_ = nullptr;
	QComboBox *spotPreset_ = nullptr;
	QDoubleSpinBox *spotDim_ = nullptr;
	QDoubleSpinBox *spotBlur_ = nullptr;
	QPushButton *spotColor_ = nullptr;
	QDoubleSpinBox *spotX_ = nullptr;
	QDoubleSpinBox *spotY_ = nullptr;
	QDoubleSpinBox *spotW_ = nullptr;
	QDoubleSpinBox *spotH_ = nullptr;
	QDoubleSpinBox *spotRot_ = nullptr;
	QDoubleSpinBox *spotRadius_ = nullptr;
	QWidget *spotMaskBox_ = nullptr;
	bool syncingSpot_ = false;
	void syncClipInspector();                       // selected clip -> controls
	void editSelectedClip(const std::function<void(TlClip &)> &fn); // controls -> clip
	void addTextClip();                             // new text clip at the playhead
	void addKeyframeAtPlayhead();
	void removeKeyframeAtPlayhead();
	void stepKeyframe(int dir); // move the playhead to the prev/next keyframe

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
	// Inspector sizing, live-tunable from the Dev panel.
	void applyInspectorParams(const struct EditorInspectorParams &p);
	EditorInspectorParams inspectorParams_;
	QVBoxLayout *insContentLayout_ = nullptr;
	QVector<EditorSnapshot> history_;
	int histIndex_ = -1;    // current position in history_
	bool restoring_ = false; // guard: restoring must not schedule new snapshots
	QTimer *histTimer_ = nullptr;
	QSlider *speedSlider_ = nullptr;
	QDoubleSpinBox *speedSpin_ = nullptr; // editable numeric speed (text input)
	QLabel *speedLabel_ = nullptr;        // "(N cuts)" / "—" status next to it
	QLabel *speedCaption_ = nullptr;      // the "Speed" caption (hidden in Full editing)
	QPushButton *resetCropBtn_ = nullptr; // hides with the Crop toggle
	double speed_ = 1.0;

	QTimer *previewTimer_ = nullptr;
	qint64 pendingMs_ = -1;
	int pendingSource_ = -1; // source for the pending preview frame

	// Looping playback. Simple Trim: the trimmed region at the global speed
	// (playAnchorMs_ = source ms at clock zero). Multi-Cut: the assembled output
	// (playAnchorMs_ = OUTPUT-time ms at clock zero; playSeg_ tracks the segment
	// currently being decoded so segment changes trigger one seek).
	// ---- Background power saving ----
	// Preview playback is the only thing in the editor that keeps working with no
	// input, so it is what costs battery while the user is in another app. On
	// losing application focus it pauses; on returning it resumes, but only if it
	// was us that paused it. Recording and exporting are never touched.
	void setPowerSaving(bool on);
	bool powerSaving_ = false;
	bool resumeOnFocus_ = false;
	bool powerSaveEnabled_ = true; // Dev panel: "Pause playback in the background"

	// ---- Preview audio (Full editing) ----
	// The timeline is pre-mixed to one PCM buffer on a worker thread and played
	// through AudioPreview, which then acts as the playback clock. Mixing is
	// cached and only redone when something audible about the timeline changes.
	void startPreviewAudio(qint64 fromMs); // mix if needed, then play
	void invalidateAudioMix();             // an edit changed what it should sound like
	void onAudioMixReady();                // worker finished
	QString audioMixKey() const;           // what the cached mix was built from

	AudioPreview *audioPreview_ = nullptr;
	QPushButton *muteBtn_ = nullptr;
	std::thread audioMixThread_;
	std::atomic<bool> audioMixCancel_{false};
	std::atomic<bool> audioMixRunning_{false};
	std::vector<float> pendingMix_; // handed over on the GUI thread when done
	QString audioMixKeyBuilt_;      // key the current buffer was mixed from
	QString audioMixKeyWanted_;     // key the running/next mix is for
	bool audioMixValid_ = false;
	bool startAudioWhenReady_ = false;
	qint64 audioStartMs_ = 0;

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
