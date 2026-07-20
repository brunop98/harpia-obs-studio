#pragma once

#include "core/AudioManager.hpp"
#include "core/CaptureManager.hpp"
#include "core/RecordingController.hpp"
#include "core/WebcamRecorder.hpp"
#include "library/ClipLibrary.hpp"
#include "library/ThumbnailCache.hpp"
#include "model/FileNameTemplate.hpp"
#include "model/RegionStore.hpp"
#include "platform/IdleMonitor.hpp"

#include <QHash>
#include <QMainWindow>
#include <QSize>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QComboBox;
class QCheckBox;
class QSpinBox;
class QLabel;
class QTimer;
class QScreen;
class QVBoxLayout;

namespace harpia {

class ObsContext;
class PresetStore;
class ClipLibraryWindow;
class RegionTool;
class CountdownOverlay;
class ScreenBorderOverlay;
class AudioPanel;
class MouseFxOverlay;
class ErrorLogsPanel;
class WebcamPreview;
class StatusBadge;

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
class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	MainWindow(ObsContext &obs, PresetStore &presets, QString defaultFolder, QWidget *parent = nullptr);
	~MainWindow() override;

private slots:
	void onPrimaryButton(); // Record/Stop toggle
	void onPauseButton();   // pause/resume while recording
	void onNewPreset();
	void showPresetMenu(const QPoint &pos); // right-click combo: edit/delete
	void onOpenClipLibrary();
	void onOpenErrorLogs();
	void onCaptureModeChanged();
	void onSaveRegionRequested();    // "Save Region…" from the region right-click
	void openSavedRegionsManager();  // rename/edit/delete saved regions
	void onAppCaptureToggled(bool on);       // single-application capture toggle
	void onAppWindowChanged();               // window picked in the app dropdown
	void onWebcamDeviceChanged();
	void onWebcamEnableToggled(bool on);     // toolbar webcam enable toggle
	void refreshWebcamRow(); // populate/reflect the webcam toggle + device combo
	void onRegionChanged(const CaptureRegion &region);
	void onIdleSettingChanged();
	void onAudioChanged();
	void onPresetChanged();
	void refreshRecentList();
	void refreshClipViews(); // recent strip + open Clip Library window
	void reloadPresetCombo();
	void showStripContextMenu(const QPoint &pos);
	void onThumbnailReady(const QString &path);
	void tickState();  // recording/paused state + timer
	void tickIdle();   // auto-pause/resume based on idle time
	void tickFocus(uint64_t foregroundPid); // auto-pause/resume based on target-app focus
	void refreshReadiness(); // validate settings, update warnings + Record button

private:
	const Preset &activePreset() const;
	QScreen *screenForActivePreset() const; // display the active preset captures
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
	QString elapsedString() const;
	void updateRegionToolVisibility(); // focus/record-driven overlay visibility
	void writeMarker(const QString &label); // append an Auto Paused/Resumed marker

protected:
	void changeEvent(QEvent *event) override; // track window activation
	// Guard against silently losing a recording: closing mid-recording asks for
	// confirmation, stops cleanly, and only closes once the file is finalized.
	void closeEvent(QCloseEvent *event) override;

private:
	// How the screen is captured (a global tool, not part of a preset).
	enum class CaptureMode { Monitor, Region };
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
	QCheckBox *idleToggle_ = nullptr;
	QSpinBox *idleSpin_ = nullptr;

	// Toolbar row 2: single-application capture + webcam enable/device.
	QCheckBox *appCaptureToggle_ = nullptr;
	QComboBox *appCombo_ = nullptr;
	QCheckBox *webcamEnableToggle_ = nullptr;

	// Center controls
	QPushButton *primaryButton_ = nullptr; // Record/Stop toggle
	QPushButton *pauseButton_ = nullptr;
	QLabel *timerLabel_ = nullptr;

	// Inline webcam controls (shown only when the active preset uses a webcam)
	QWidget *webcamBox_ = nullptr;
	QComboBox *webcamCombo_ = nullptr;
	QLabel *webcamWarn_ = nullptr;
	WebcamPreview *webcamPreview_ = nullptr;

	// Recording readiness
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
	std::vector<std::string> hwInputIds_;
	QStringList lastWarningSig_;

	// Recent strip
	QPushButton *libraryButton_ = nullptr;
	QPushButton *errorLogsButton_ = nullptr;
	QListWidget *recentStrip_ = nullptr;
	ThumbnailCache thumbnails_;
	QHash<QString, QListWidgetItem *> itemByPath_;

	AudioPanel *audioPanel_ = nullptr;

	QTimer *stateTimer_ = nullptr;
	QTimer *idleTimer_ = nullptr;
	QTimer *meterTimer_ = nullptr;

	std::unique_ptr<RegionStore> regionStore_; // saved capture regions (global)
	std::unique_ptr<ClipLibraryWindow> clipWindow_;
	std::unique_ptr<ErrorLogsPanel> errorLogsPanel_;
	std::unique_ptr<RegionTool> regionTool_;
	std::unique_ptr<MouseFxOverlay> mouseFx_;
	std::unique_ptr<ScreenBorderOverlay> screenBorder_;

	std::string activePresetId_;
	CaptureRegion currentRegion_;
	QSize canvasSize_;
	QString defaultFolder_;

	// Recording timer accounting (wall clock minus paused spans).
	qint64 recStartMs_ = 0;
	qint64 pausedAccumMs_ = 0;
	qint64 pauseStartMs_ = 0;
	bool wasPaused_ = false;
	bool autoPaused_ = false;  // paused by the idle monitor (vs. manually)
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
	bool remuxActive_ = false;

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
