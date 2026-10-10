#pragma once

#include "core/AudioManager.hpp"
#include "ui/MainLayoutParams.hpp"
#include "core/CaptureManager.hpp"
#include "core/RecordingController.hpp"
#include "core/DiskSpace.hpp"
#include "core/FollowMouse.hpp"
#include "core/MultiArea.hpp"
#include "ui/AreaStyle.hpp"
#include "core/GlobalHotkeys.hpp"
#include "core/ModeCapabilities.hpp"
#include "core/ZoomMode.hpp"
#include "core/RegionWatch.hpp"
#include "core/RemoteControl.hpp"
#include "core/RemoteSession.hpp"
#include "core/WebcamRecorder.hpp"
#include "library/ClipLibrary.hpp"
#include "library/ThumbnailCache.hpp"
#include "model/FileNameTemplate.hpp"
#include "model/RegionStore.hpp"
#include "platform/IdleMonitor.hpp"
#include "platform/WindowList.hpp"
#include "ui/WindowPicker.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QPoint>
#include <QMainWindow>
#include <QSize>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct os_inhibit_info; // libobs sleep inhibitor (util/platform.h)
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QComboBox;
class QShortcut;
class QCheckBox;
class QSpinBox;
class QToolButton;
class QLabel;
class QTimer;
class QProgressBar;
class QScreen;
class QVBoxLayout;
class QHBoxLayout;
class QFrame;

namespace harpia {


class ObsContext;
class PresetStore;
class ClipLibraryWindow;
class RegionTool;
class MultiAreaOverlay;
class CountdownOverlay;
class ScreenBorderOverlay;
class AudioPanel;
class MouseFxOverlay;
class ErrorLogsPanel;
class RecorderControlsOverlay;
class WebcamPreview;
class StatusBadge;
class MainDevPanel;

// The PowerRec-inspired main window: a wide, compact, dark surface optimized for
// starting/stopping recordings in one or two clicks.
//
//   ┌───────────────────────────────────────────────────────────────┐
//   │ Preset ▾  + New   [Region] [Full screen]  ☑ Only while using  ⚙ │  toolbar
//   │                                                                 │
//   │            ●  Record        ■ Stop        00:00:00              │  controls
//   │                                                                 │
//   │  [thumb] [thumb] [thumb] [thumb] …  (recent recordings)         │  strip
//   └───────────────────────────────────────────────────────────────┘
// Also the recorder behind the local remote control (core/RemoteControl.hpp):
// the Unity Play Mode Recorder starts, pauses and stops recordings through it.
class MainWindow : public QMainWindow, public RemoteHandler {
	Q_OBJECT
public:
	MainWindow(ObsContext &obs, PresetStore &presets, QString defaultFolder, QWidget *parent = nullptr);
	~MainWindow() override;

	// The slow half of starting up: the capture source, the hardware probes,
	// the recent-clips scan. Call it AFTER show(), with the window painted --
	// none of it is needed to draw the window, and all of it used to happen
	// while the screen was still empty.
	void finishStartup();

	// The Developer Panel drives the live layout metrics (layoutParams/
	// setLayoutParams live in the private section below).
	friend class MainDevPanel;

private slots:
	void onPrimaryButton(); // Record/Stop toggle
	void onPauseButton();   // pause/resume while recording
	void onNewPreset();
	void showPresetMenu(const QPoint &pos); // right-click combo: edit/delete
	void onOpenClipLibrary();
	void onOpenErrorLogs();
	// Open the video editor with nothing loaded (status-bar button).
	void openBlankEditor();
	void onCaptureModeChanged();
	void onSaveRegionRequested();    // "Save Region…" from the region right-click
	void openSavedRegionsManager();  // rename/edit/delete saved regions
	void onMonitorChanged();    // display picked on the main window
	void onAppWindowChanged();  // app dropdown; first item = no auto-pause
	void onWebcamDeviceChanged(); // webcam dropdown; first item = no webcam
	void refreshWebcamRow(); // populate/reflect the webcam device combo
	// Fill the camera list. Separate from refreshWebcamRow() because this is
	// the part that probes DirectShow, and it only runs when someone opens the
	// dropdown or actually turns the webcam on.
	void reloadWebcamCombo();
	void onRegionChanged(const CaptureRegion &region);
	void onIdleSettingChanged();
	void onRegionLeaveSettingChanged();
	void onCountdownSettingChanged();
	void onAudioChanged();
	void onPresetChanged();
	void refreshRecentList();
	void refreshClipViews(); // recent strip + open Clip Library window
	void reloadPresetCombo();
	void showStripContextMenu(const QPoint &pos);
	// Asks, then sends them to the recycle bin (RecordingDelete.hpp); the strip
	// and an open Clip Library refresh.
	void deleteRecordings(const QStringList &paths);
	void openAudioExtract(const QString &path); // "Extract Audio Only", from either file menu
	void onThumbnailReady(const QString &path);
	void tickState();  // recording/paused state + timer
	void tickIdle();   // auto-pause/resume based on idle time
	void tickFocus(uint64_t foregroundPid); // auto-pause/resume based on target-app focus
	void refreshReadiness(); // validate settings, update warnings + Record button

private:
	// The foreground process's executable name, remembered by pid for a
	// moment: the name is an OpenProcess + image-path query, and the state
	// tick asked for it four times a second while the same app sat in front.
	uint64_t fgExePid_ = 0;
	QString fgExe_;
	qint64 fgExeMs_ = 0;

	const Preset &activePreset() const;
	QScreen *screenForActivePreset() const; // display the active preset captures (memoized)
	QScreen *resolveScreenForActivePreset() const; // the real (expensive) lookup
	mutable QScreen *screenCache_ = nullptr;
	mutable int screenCacheMonitor_ = -1;

	// Free-space label state: queried on a worker thread, cached ~3 s. See
	// updateDiskLabel for why the GUI thread must never touch QStorageInfo.
	qint64 diskLabelMs_ = 0;
	QString diskLabelFolder_;
	DiskStatus diskLabelStatus_;
	bool diskQueryBusy_ = false;

	// OS sleep inhibitor, held while a recording is running (created at start,
	// released when the output finishes). Null when idle. Global-scope type:
	// libobs declares it outside any namespace.
	::os_inhibit_info *sleepInhibit_ = nullptr;

	// Stop handoff. The muxer's own "stop" signal (recorder_.onFinished) is
	// what says the file is closed; finalizeAfterStop() runs from it with no
	// added delay. The old path -- notice via the 250 ms poll, then sleep
	// 700 ms "so the muxer finishes flushing" -- survives only as a fallback
	// for an output that dies without signalling.
	void finalizeAfterStop();
	// resetVideo + capture source + crop, extracted so the countdown can run
	// it early and hide its cost; every call inside dedupes, so re-running at
	// the countdown's zero mark is nearly free.
	void armVideoPipeline();
	// Startup: offer to remux recordings orphaned by a crash into real MP4s.
	void offerOrphanRecovery(const QStringList &orphans);
	// What armVideoPipeline() decided, for the filename tokens and the start log.
	uint32_t armedW_ = 0, armedH_ = 0;
	int armedFps_ = 0;
	bool stopHandled_ = false;             // finalize ran (or is scheduled to)
	std::atomic<bool> outputStopped_{false}; // set on the obs signal thread
	int lastPrimaryUiState_ = -1; // primary-button churn guard (see updateButtons)
	QSize canvasForActivePreset() const;    // that display's size in device px
	QStringList presetFolders() const;
	ClipLibrary::PresetByFolder presetFolderMap() const;
	QString buildOutputPath(const Preset &preset) const;
	// De-duplicate a preset name against the other presets ("Name (2)"…).
	std::string uniquePresetName(const std::string &wanted, const std::string &selfId) const;
	void startRecording();
	void applyLiveCapture(); // (re)bind the live capture to the current mode
	void beginRecordFlow(); // readiness gate -> countdown or immediate start
	void beginStart();      // enter Starting… and kick off the recording
	void beginStop();       // enter Stopping… and request finalize
	qint64 contentElapsedMs() const; // recorded content length (minus paused spans)
	// Stamp the pause clock exactly when a pause/resume happens (not on the
	// next timer tick), keeping the content-length accounting accurate.
	void notePauseTransition(bool paused);
	// Post-stop handling: optional short-recording discard, then MKV->MP4 remux.
	void finalizeStopped(const QString &recordedPath, const QString &finalPath,
			     const QString &webcamPath, const QString &markersPath, qint64 contentMs,
			     int minSeconds, bool needsRemux);
	// True if the user chose to discard (files already deleted).
	bool discardShortRecording(const QString &recordedPath, const QString &webcamPath,
				   const QString &markersPath, qint64 contentMs, int minSeconds);
	// Background MKV->MP4 remux; on success removes the mkv, else keeps it.
	void remuxInBackground(const QString &mkvPath, const QString &mp4Path);
	void logRecordingStart(const Preset &p, const QString &recordedPath, const QString &finalPath,
			       const QString &webcamPath, uint32_t baseW, uint32_t baseH, int fps);
	void updateButtons();
	// Open the editor for the active preset + persist. If initialPage is given
	// (e.g. "Webcam"), the editor opens with that settings page selected.
	void editActivePreset(const QString &initialPage = QString());
	void syncIdleControls();     // load idle toggle/spin from the active preset
	void updateStatusChip();     // reflect ready/recording/paused/warning in the chip
	void applyDarkTheme();
	// Progressive-disclosure responsiveness: as the window narrows, hide the
	// least essential sections first, always keeping the record controls usable.
	void applyResponsiveLayout(int width);
	QString elapsedString() const;
	void updateRegionToolVisibility(); // focus/record-driven overlay visibility
	// When focus last left Harpia (invalid while it is in front). The region
	// frame lingers kRegionLingerMs after that, then hides until focus is back.
	QElapsedTimer regionUnfocusedSince_;
	static constexpr int kRegionLingerMs = 4000;
	void updateFloatingControls();     // show/hide + sync the desktop Pause/Stop HUD
	void writeMarker(const QString &label); // append an Auto Paused/Resumed marker
	void refreshDriveLink(); // show/hide the status-bar Google Drive shortcut

	// Developer Panel: read/apply the live-tweakable layout metrics, and
	// (re)compute the recent-strip icon/grid/height from them.
	MainLayoutParams layoutParams() const { return layout_; }
	void setLayoutParams(const MainLayoutParams &p);
	// How the Multi-Area layout is drawn (Developer Panel → Areas).
	const AreaStyle &areaStyle() const { return areaStyle_; }
	void setAreaStyle(const AreaStyle &s);
	// Show the areas on screen with the current style: 0 off, 1 as when idle,
	// 2 as while recording. Uses the real layout, or sample areas without one.
	void setAreaStylePreview(int mode);
	void applyStripMetrics();
	void openDevPanel();

protected:
	void changeEvent(QEvent *event) override; // track window activation
	// Windows: WM_DEVICECHANGE forces an immediate hardware re-probe, so the
	// periodic probe can be lazy without missing a plugged-in camera or mic.
	bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
	// Refreshes the app list right before the app dropdown's popup opens.
	bool eventFilter(QObject *obj, QEvent *event) override;
	// Free space + destination folder, shown on the recording screen, and the
	// question asked before starting when there is little room left.
	void updateDiskLabel();
	bool confirmDiskSpace();
	void openOutputFolder();
	// Guard against silently losing a recording: closing mid-recording asks for
	// confirmation, stops cleanly, and only closes once the file is finalized.
	void closeEvent(QCloseEvent *event) override;
	void resizeEvent(QResizeEvent *event) override; // drives applyResponsiveLayout

private:
	// How the screen is captured (a global tool, not part of a preset).
	// Mirrors RecordMode in core/ModeCapabilities.hpp, which owns the rules for
	// what each mode supports. Kept as a separate enum only because this one is
	// woven through the window already; recordMode() converts.
	enum class CaptureMode { Monitor, Region, AudioOnly };
	RecordMode recordMode() const { return recordModeFromInt(int(captureMode_)); }
	// Enable, disable and explain every control that depends on the capture
	// mode. One pass over the whole window rather than a check at each site --
	// see ModeCapabilities.hpp for why.
	void applyModeCapabilities();
	CaptureMode captureMode_ = CaptureMode::Monitor;

	// Rebuild the capture dropdown (modes + saved regions + Manage). The last
	// real selection index, restored when the "Manage…" action is chosen.
	void reloadCaptureModeCombo();
	int prevCaptureIndex_ = 0;

	// Single-application (window) capture — overrides the monitor/region mode.
	bool appCaptureEnabled_ = false;
	QString appWindowValue_; // selected window's capture value

	ObsContext &obs_;
	PresetStore &presets_;
	CaptureManager capture_;
	RecordingController recorder_;
	WebcamRecorder webcam_;
	AudioManager audio_;
	FileNameTemplate nameTemplate_;
	std::unique_ptr<IdleMonitor> idle_;

	// Toolbar
	QComboBox *presetCombo_ = nullptr;
	QPushButton *editPresetButton_ = nullptr;
	QPushButton *newPresetButton_ = nullptr;
	QComboBox *captureModeCombo_ = nullptr;
	QComboBox *monitorCombo_ = nullptr; // which display to record (owns preset.monitorIndex)
	void reloadMonitorCombo();          // refresh the display list, keep selection
	void applyMonitorIndex(int index);  // persist + re-anchor capture/canvas/overlays
	QComboBox *idleCombo_ = nullptr;
	// Region-mode-only auto-stop. See RegionWatch.
	QComboBox *regionLeaveCombo_ = nullptr;
	QWidget *regionLeaveGroup_ = nullptr;
	bool regionLeaveNarrow_ = false; // the window is too narrow for this row
	RegionWatch regionWatch_;
	// Screen geometry resolved once when the watch arms; see tickRegionWatch.
	bool regionWatchArmed_ = false;
	QPoint regionWatchOrigin_;
	double regionWatchDpr_ = 1.0;
	void updateRegionLeaveVisibility();
	void tickRegionWatch();
	QTimer *regionWatchTimer_ = nullptr; // idle auto-pause timeout; first item = Off

	// Follow Mouse: the region pans to keep the cursor framed. Armed only while
	// actively recording a Custom Region with the preset option on, and its
	// 16 ms timer runs only then -- unlike the other timers here, which are
	// always on. Screen geometry is cached at arm time, same trick as the
	// region watch; screenForActivePreset() is far too expensive for 60 Hz.
	// Shared auto-resume gate (core/AutoPause.hpp): an auto-resume happens only
	// when no armed system still wants the pause. Each tick refreshes its own
	// last-known fact here; staleness is bounded by the ticks themselves.
	bool autoResumeBlocked() const;
	double lastIdleSecs_ = 0.0;
	bool lastFocusOk_ = true;
	bool lastCursorInside_ = true;

	// Recording hotkeys: global (RegisterHotKey) where the platform allows,
	// with the window-scoped shortcuts as the fallback. Both follow the same
	// configured keys (see rebindHotkeys).
	std::unique_ptr<GlobalHotkeys> hotkeys_;
	QShortcut *recordShortcut_ = nullptr;
	QShortcut *pauseShortcut_ = nullptr;
	void rebindHotkeys();
	// Release every global hotkey while a shortcut is being typed, and take
	// them back afterwards. A registered key never reaches the focused widget,
	// so without this a key field cannot be given any key already in use --
	// which is precisely what the conflict dialog asks for.
	void setHotkeysSuspended(bool on);
	bool hotkeysSuspended_ = false;

	FollowMouse followMouse_;
	QTimer *followTimer_ = nullptr;
	QSize followScreenDevicePx_;
	QPoint followOrigin_;      // screen top-left, logical px (cached at arm)
	double followDpr_ = 1.0;   // cached at arm
	QElapsedTimer followClock_;
	bool followApplying_ = false; // the follow tick is moving the overlay itself;
	                              // its regionChanged echo must not re-apply the
	                              // crop or restart the readiness debounce
	void tickFollowMouse();
	void syncFollowMouse(); // start/stop the timer to match the current state

	// The Follow Mouse shortcut. Unlike Zoom and Spotlight, following is on
	// from the first frame, so the key parks the camera rather than starting
	// it. Switching it off glides the region back to the rectangle that was
	// framed before the recording started -- followHome_ -- rather than
	// abandoning it wherever the cursor last dragged it.
	QShortcut *followShortcut_ = nullptr;
	bool followSuspended_ = false; // the key switched following off for this take
	QPoint followHome_;            // region top-left when following armed
	void rebindFollowHotkey();
	void onFollowToggle();

	// Multi-Area (core/MultiArea.hpp): the recording switches between fixed,
	// same-size areas as the mouse moves between them. Area 1 is the region;
	// the others come from the preset. Primed just before the recorder starts
	// (so the first frame is already the area under the mouse), armed once the
	// output is live, and area 1 is put back when the take is over.
	AreaSwitcher areaSwitch_;
	QTimer *areaTimer_ = nullptr;
	QElapsedTimer areaClock_;
	QPoint areaHome_;           // area 1's top-left when primed
	int areaStart_ = 0;         // the area the take starts on
	int areaMainMonitor_ = -1;  // the main capture's OBS monitor (the region's)
	int areaShownMonitor_ = -1; // the monitor being recorded right now
	QVector<AreaRef> areaTake_; // the take's layout (areas whose monitor could be captured)
	QVector<AreaRef> areaEdit_; // the layout being arranged
	bool areaPrimed_ = false;   // a take is using the layout; restore at the end
	bool areaArranging_ = false; // the Arrange overlays are up; the frame stays hidden
	// One overlay per monitor, keyed by OBS monitor index.
	std::map<int, std::unique_ptr<MultiAreaOverlay>> areaOverlays_;
	AreaStyle areaStyle_;
	int areaStylePreview_ = 0; // Developer Panel preview (see setAreaStylePreview)
	// OBS monitor index -> QScreen, memoized like screenForActivePreset.
	mutable std::map<int, QScreen *> monitorScreens_;
	QScreen *screenForMonitor(int monitorIndex) const;
	QScreen *resolveScreenForMonitor(int monitorIndex) const;
	bool multiAreaWanted() const; // preset + capture mode say Multi-Area applies
	int areaMainMonitor() const;  // the region's monitor
	QVector<AreaRef> multiAreaLayout() const; // area 1 first, each clamped to its screen
	// The monitor (of `monitors`) the cursor is on, and where on it in device
	// pixels; -1 when it is on none of them.
	int cursorOnMonitor(const QVector<int> &monitors, QPoint *devicePx) const;
	void moveRegionTo(QPoint topLeftDevicePx); // live, like the follow tick
	void placeFrame(int monitor, const QRect &deviceRect); // the frame, on any screen
	void showArea(int monitor, QPoint topLeftDevicePx);   // record this area now
	void primeMultiArea();
	void restoreAreaHome();
	void syncMultiArea();
	void tickMultiArea();
	void updateAreaOverlay();
	void onArrangeAreas();
	// The area from a window: point and click (WindowPicker), then optionally
	// keep the area on that window as it moves or resizes.
	void startWindowPick();
	void applyPickedWindow(const harpia::DesktopWindow &w);
	void setRegionFromDevicePx(const QRect &deviceRect); // a region on the active display
	void setFollowWindow(bool on);
	void followWindowTick();
	void forgetPickedWindow();
	void syncWindowMenu();
	void onAreasEdited(int monitor, const QVector<QPoint> &tops);
	void finishArrange();
	// Every area but the region, on every monitor. `confirm` asks first (the
	// region frame's menu); the Arrange bar confirms with a second click.
	void clearAllAreas(bool confirm);
	MultiAreaOverlay *areaOverlayFor(int monitor);
	// Put `layout` on the overlays (one per monitor it touches, plus every
	// other monitor when `allMonitors`); `active` is the area not to outline.
	void showAreaOverlays(const QVector<AreaRef> &layout, QSize size, int active, bool recording,
			      bool allMonitors);
	void hideAreaOverlays();

	// Automatic Zoom: the shortcut pushes the recorded picture in on the cursor
	// and pulls it back out. Full Screen only for now -- see ZoomMode.hpp for
	// why, and for why the core takes a canvas rather than a capture mode.
	// Shares its shape with Follow Mouse above: geometry cached at arm time, a
	// 60 Hz tick that only runs while there is something to animate.
	ZoomMode zoom_;
	// Custom Region recordings whose preset has Zoom on are positioned by the
	// scene transform instead of a crop filter, for the whole take. A crop
	// throws away everything outside the region, so a zoom inside one has
	// nowhere to travel; the transform keeps the whole screen available.
	//
	// Decided once, at arm time, rather than switched when the zoom starts:
	// swapping a crop for a transform mid-recording is two libobs calls with a
	// frame boundary between them, and whichever order they go in, one frame
	// can land showing the wrong thing. Presets without Zoom keep the crop and
	// are completely unaffected.
	bool regionByTransform_ = false;
	void applyRegionFraming(); // push the region to the crop or the transform
	// Give ZoomMode its canvas, source and resting framing. Does the monitor
	// lookup, so it runs once per recording -- never from the 60 Hz tick.
	void configureZoomCanvas();
	QTimer *zoomTimer_ = nullptr;
	QSize zoomCanvasDevicePx_;
	QPoint zoomOrigin_;      // screen top-left, logical px (cached at arm)
	double zoomDpr_ = 1.0;   // cached at arm
	QElapsedTimer zoomClock_;
	bool zoomArmed_ = false; // a Full Screen recording with zoom configured
	QShortcut *zoomShortcut_ = nullptr;
	void rebindZoomHotkey();

	// Spotlight: everything but a patch around the cursor goes dark. No state
	// of its own here -- the desktop overlay that DRAWS it owns that, because
	// it is the thing that has to fade and repaint. This side is the shortcut
	// and the marker. Also preset-owned, so it re-binds alongside the zoom key.
	QShortcut *spotlightShortcut_ = nullptr;
	void rebindSpotlightHotkey();
	void rebindPresetHotkeys(); // both of the preset-owned keys, in one call
	void onSpotlightToggle();
	void onZoomToggle();
	void tickZoom();
	// The border marking the zoomed area, driven from the same transform that
	// is on the scene so the two cannot disagree. Borrows the monitor-border
	// overlay, and hands it back when the zoom ends.
	void updateZoomBorder();
	void hideZoomBorder();
	bool zoomBorderShown_ = false;
	void syncZoom(); // arm/disarm to match the current recording state
	QWidget *idleGroup_ = nullptr;   // label + combo, hidden when very narrow
	QComboBox *countdownCombo_ = nullptr;
	QWidget *countdownGroup_ = nullptr; // label + combo, hidden when narrow

	// Toolbar row 2: single-application capture + webcam device. Dropdown-only —
	// each combo's first item means "off", so nothing toggles/jumps.
	QComboBox *appCombo_ = nullptr;
	void reloadAppCombo(); // refresh the window list (called as the popup opens)

	// Center controls
	QPushButton *primaryButton_ = nullptr; // Record/Stop toggle
	QPushButton *pauseButton_ = nullptr;
	QLabel *timerLabel_ = nullptr;

	// Inline webcam controls (shown only when the active preset uses a webcam)
	QWidget *webcamBox_ = nullptr;
	// Audio Only fills the webcam slot with big level meters. It is the one
	// thing worth watching while recording sound, and it occupies space that
	// would otherwise be a disabled camera preview.
	QWidget *audioMeterBox_ = nullptr;
	QProgressBar *bigDesktopMeter_ = nullptr;
	QProgressBar *bigMicMeter_ = nullptr;
	void updateBigMeters();
	QComboBox *webcamCombo_ = nullptr;
	QLabel *webcamWarn_ = nullptr;
	WebcamPreview *webcamPreview_ = nullptr;

	// Recording readiness
	QLabel *diskLabel_ = nullptr; // "12.4 GB free  ·  D:\\Recordings"
	QWidget *warningsBox_ = nullptr;
	QVBoxLayout *warningsLayout_ = nullptr;
	StatusBadge *statusBadge_ = nullptr; // passive Ready/Recording/Paused/Error dot
	QTimer *readinessTimer_ = nullptr;
	bool recordingBlocked_ = false;
	QString firstIssue_; // headline warning/error message shown in the chip

	// Readiness caches: hardware enumeration is expensive (it creates obs source
	// properties/instances), so probe it on a TTL instead of every tick, and only
	// rebuild the warnings UI when the set of warnings actually changes.
	qint64 hwProbeMs_ = 0;
	int hwMonitorCount_ = 0;
	bool hwCameraPresent_ = false;
	int hwFolderIssue_ = 0;   // 0 ok, 1 missing, 2 read-only, 3 low space
	bool hwEncoderOk_ = true; // selected codec has an available encoder
	std::vector<std::string> hwInputIds_;
	QStringList lastWarningSig_;
	QTimer *readinessDebounce_ = nullptr; // coalesces refreshes during drags

	// Stop watchdog: force-stop the output if the async stop hangs > 10s.
	qint64 stopRequestMs_ = 0;
	bool forcedStop_ = false;

	// Recent strip
	QPushButton *libraryButton_ = nullptr;
	QPushButton *errorLogsButton_ = nullptr;
	QPushButton *driveLinkButton_ = nullptr; // status-bar Google Drive shortcut
	QListWidget *recentStrip_ = nullptr;
	QWidget *recentSection_ = nullptr; // header + strip, hidden when narrow
	ThumbnailCache thumbnails_;
	QHash<QString, QListWidgetItem *> itemByPath_;
	QStringList lastRecentSig_; // last strip contents (path|mtime|size per clip)

	AudioPanel *audioPanel_ = nullptr;
	// Collapsible Audio foldout: a header toggle over the audio body.
	QToolButton *audioToggleButton_ = nullptr;
	QWidget *audioBody_ = nullptr;

	// Developer Panel plumbing: the layouts/widgets whose sizes are tweakable
	// live, plus the current metrics and the (lazily created) panel.
	MainLayoutParams layout_;
	QVBoxLayout *rootLayout_ = nullptr;
	QHBoxLayout *row1Layout_ = nullptr;
	QHBoxLayout *middleLayout_ = nullptr;
	QVBoxLayout *behaviorColLayout_ = nullptr;
	QHBoxLayout *controlsLayout_ = nullptr;
	QFrame *ctrlSeparator_ = nullptr;
	std::vector<QWidget *> behaviorRows_;
	std::vector<QLabel *> behaviorLabels_;
	MainDevPanel *devPanel_ = nullptr;

	QTimer *stateTimer_ = nullptr;
	QTimer *idleTimer_ = nullptr;
	QTimer *meterTimer_ = nullptr;

	std::unique_ptr<RegionStore> regionStore_; // saved capture regions (global)
	std::unique_ptr<ClipLibraryWindow> clipWindow_;
	std::unique_ptr<ErrorLogsPanel> errorLogsPanel_;
	std::unique_ptr<RegionTool> regionTool_;
	std::unique_ptr<WindowPicker> windowPicker_;
	quintptr pickedWindow_ = 0;      // the window the area was set from (0 = none)

	// ---- Remote control (the Unity Play Mode Recorder) -------------------
	// A remote start records the area it names (the Game view) with the
	// active preset's settings, straight away (no countdown). The area it
	// replaces is put back once that recording has finished, so the user's
	// own capture mode, region and display are never changed by it.
	QJsonObject remoteStatus() override;
	RemoteReply remoteStart(const RemoteStartRequest &req) override;
	RemoteReply remotePause() override;
	RemoteReply remoteResume() override;
	RemoteReply remoteStop(int discardShorterThanMs) override;
	RemoteReply remoteShowArea(const QRect &areaPx) override;
	void applyRemoteSettings(); // listen (or not) as the System settings say
	RecorderSnapshot recorderSnapshot() const;
	QVector<QRect> monitorRectsPx() const; // each display, physical px, in capture order
	void restoreAreaAfterRemote();
	// The remote's recording ended: silent delete if it asked, then the area back.
	bool remoteDiscardIfAsked();
	std::unique_ptr<RemoteControlServer> remoteServer_;
	RemoteSession remoteSession_;
	struct RemoteAreaBackup {
		QString modeData; // the capture combo's choice ("monitor", "region", "saved:<id>", ...)
		CaptureRegion region;
		int monitor = 0;
	};
	std::optional<RemoteAreaBackup> remoteArea_;
	QString remoteClient_;
	QString pickedWindowTitle_;
	bool followWindowOn_ = false;    // keep the area on it (off unless asked)
	QTimer *followWindowTimer_ = nullptr;
	std::unique_ptr<MouseFxOverlay> mouseFx_;
	std::unique_ptr<ScreenBorderOverlay> screenBorder_;
	std::unique_ptr<RecorderControlsOverlay> floatingControls_; // desktop Record/Pause/Stop HUD
	// "Hide until next recording" was chosen from the HUD's right-click menu.
	// Cleared when a recording starts, which is what "until" means.
	bool floatingDismissed_ = false;

	std::string activePresetId_;
	CaptureRegion currentRegion_;
	QSize canvasSize_;
	QString defaultFolder_;

	// Recording timer accounting (wall clock minus paused spans).
	qint64 recStartMs_ = 0;
	qint64 pausedAccumMs_ = 0;
	qint64 pauseStartMs_ = 0;
	bool wasPaused_ = false;
	bool autoPaused_ = false;
	// The off-region pause paused it, so the off-region rule may resume it.
	// Separate from autoPaused_ so the idle and region state machines cannot
	// resume each other's pauses.
	bool regionAutoPaused_ = false;
	bool focusPaused_ = false; // paused because the target app lost focus

	// Transitional UI states while the async output starts/finalizes, so the
	// button shows Starting…/Stopping… and can't be clicked repeatedly.
	bool starting_ = false;
	bool stopping_ = false;
	int spinPhase_ = 0;         // animated spinner frame index
	int lastPauseUiState_ = -1; // 0 idle / 1 recording / 2 paused — restyle only on change

	// Deferred-close bookkeeping: the user confirmed closing while a recording
	// (or its finalize/remux) was still in flight; close as soon as it's done.
	bool closePending_ = false;
	// A COUNT, not a flag: crash recovery can run several remuxes at once, and
	// a bool would be cleared by whichever finished first while the rest were
	// still writing.
	int remuxActive_ = 0;

	// Pre-recording countdown state.
	bool countingDown_ = false;
	int countdownRemaining_ = 0;
	std::unique_ptr<CountdownOverlay> countdownOverlay_;

	// Focus auto-pause (single-application capture only): the executable name of
	// the *selected* application, parsed from the app dropdown's window value at
	// Record time; matched against the foreground process's executable so all of
	// the app's windows/processes count as focused. Empty when the feature is
	// inactive. `ownPid_` is Harpia's own process id (its windows are neutral —
	// they never trigger the pause).
	QString targetExe_;
	uint64_t ownPid_ = 0;
	QString markersPath_;

	// Just-finished recording bookkeeping (captured at Stop): the file OBS
	// actually wrote (a temp .mkv when recording MP4 for a fast stop), the final
	// path shown to the user, the companion webcam file, the content length, and
	// the preset's minimum length.
	QString lastRecordedPath_;
	QString lastScreenPath_;
	QString lastWebcamPath_;
	qint64 lastContentMs_ = 0;
	int lastMinSeconds_ = 0;
};

} // namespace harpia
