#pragma once

#include "core/AudioManager.hpp"
#include "core/CaptureManager.hpp"
#include "core/RecordingController.hpp"
#include "library/ClipLibrary.hpp"
#include "library/ThumbnailCache.hpp"
#include "model/FileNameTemplate.hpp"
#include "platform/IdleMonitor.hpp"

#include <QHash>
#include <QMainWindow>
#include <QSize>
#include <memory>
#include <string>

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QComboBox;
class QCheckBox;
class QSpinBox;
class QLabel;
class QTimer;
class QScreen;

namespace harpia {

class ObsContext;
class PresetStore;
class ClipLibraryWindow;
class RegionTool;
class AudioPanel;
class MouseFxOverlay;

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
	void onPrimaryButton(); // start, or pause/resume when recording
	void onStopButton();
	void onNewPreset();
	void showPresetMenu(const QPoint &pos); // right-click combo: edit/delete
	void onOpenPresetFolder();
	void onOpenClipLibrary();
	void onCaptureModeChanged();
	void onRegionChanged(const CaptureRegion &region);
	void onIdleSettingChanged();
	void onAudioChanged();
	void onPresetChanged();
	void refreshRecentList();
	void reloadPresetCombo();
	void showStripContextMenu(const QPoint &pos);
	void onThumbnailReady(const QString &path);
	void tickState(); // recording/paused state + timer
	void tickIdle();  // auto-pause/resume based on idle time

private:
	const Preset &activePreset() const;
	QScreen *screenForActivePreset() const; // display the active preset captures
	QSize canvasForActivePreset() const;    // that display's size in device px
	QStringList presetFolders() const;
	ClipLibrary::PresetByFolder presetFolderMap() const;
	QString buildOutputPath(const Preset &preset) const;
	void startRecording();
	void updateButtons();
	void syncIdleControls();     // load idle toggle/spin from the active preset
	void applyDarkTheme();
	QString elapsedString() const;
	void updateRegionToolVisibility(); // focus/record-driven overlay visibility

protected:
	void changeEvent(QEvent *event) override; // track window activation

private:
	// How the screen is captured (a global tool, not part of a preset).
	enum class CaptureMode { Monitor, Region };
	CaptureMode captureMode_ = CaptureMode::Monitor;

	ObsContext &obs_;
	PresetStore &presets_;
	CaptureManager capture_;
	RecordingController recorder_;
	AudioManager audio_;
	FileNameTemplate nameTemplate_;
	std::unique_ptr<IdleMonitor> idle_;

	// Toolbar
	QComboBox *presetCombo_ = nullptr;
	QPushButton *newPresetButton_ = nullptr;
	QComboBox *captureModeCombo_ = nullptr;
	QCheckBox *idleToggle_ = nullptr;
	QSpinBox *idleSpin_ = nullptr;
	QPushButton *openFolderButton_ = nullptr;

	// Center controls
	QPushButton *primaryButton_ = nullptr;
	QPushButton *stopButton_ = nullptr;
	QLabel *timerLabel_ = nullptr;

	// Recent strip
	QPushButton *libraryButton_ = nullptr;
	QListWidget *recentStrip_ = nullptr;
	ThumbnailCache thumbnails_;
	QHash<QString, QListWidgetItem *> itemByPath_;

	AudioPanel *audioPanel_ = nullptr;

	QTimer *stateTimer_ = nullptr;
	QTimer *idleTimer_ = nullptr;
	QTimer *meterTimer_ = nullptr;

	std::unique_ptr<ClipLibraryWindow> clipWindow_;
	std::unique_ptr<RegionTool> regionTool_;
	std::unique_ptr<MouseFxOverlay> mouseFx_;

	std::string activePresetId_;
	CaptureRegion currentRegion_;
	QSize canvasSize_;
	QString defaultFolder_;

	// Recording timer accounting (wall clock minus paused spans).
	qint64 recStartMs_ = 0;
	qint64 pausedAccumMs_ = 0;
	qint64 pauseStartMs_ = 0;
	bool wasPaused_ = false;
	bool autoPaused_ = false; // paused by the idle monitor (vs. manually)
};

} // namespace harpia
