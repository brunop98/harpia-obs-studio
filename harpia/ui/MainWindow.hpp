#pragma once

#include "core/CaptureManager.hpp"
#include "core/RecordingController.hpp"
#include "library/ClipLibrary.hpp"
#include "model/FileNameTemplate.hpp"
#include "platform/IdleMonitor.hpp"

#include <QMainWindow>
#include <QSize>
#include <memory>
#include <string>

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QComboBox;
class QLabel;
class QTimer;

namespace harpia {

class ObsContext;
class PresetStore;
class ClipLibraryWindow;
class RegionOverlay;

// The minimal recorder window: one big Record/Pause control, a Stop button, the
// 10 most-recent recordings (draggable into other apps), and a button to open
// the full Clip Library. Owns the recording session; the libobs backend and
// preset store are provided by main().
class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	MainWindow(ObsContext &obs, PresetStore &presets, QString defaultFolder, QWidget *parent = nullptr);
	~MainWindow() override;

private slots:
	void onPrimaryButton(); // start, or pause/resume when recording
	void onStopButton();
	void onOpenClipLibrary();
	void onManagePresets();
	void onSelectRegion();
	void onClearRegion();
	void refreshRecentList();
	void reloadPresetCombo();
	void tickState();       // reflect recording/paused state in the UI
	void tickIdle();        // auto-pause/resume based on idle time

private:
	const Preset &activePreset() const;
	QStringList presetFolders() const;
	ClipLibrary::PresetByFolder presetFolderMap() const;
	QString buildOutputPath(const Preset &preset) const;
	void startRecording();
	void updateButtons();

	ObsContext &obs_;
	PresetStore &presets_;
	CaptureManager capture_;
	RecordingController recorder_;
	FileNameTemplate nameTemplate_;
	std::unique_ptr<IdleMonitor> idle_;

	QPushButton *primaryButton_ = nullptr;
	QPushButton *stopButton_ = nullptr;
	QPushButton *libraryButton_ = nullptr;
	QPushButton *presetsButton_ = nullptr;
	QPushButton *regionButton_ = nullptr;
	QPushButton *fullScreenButton_ = nullptr;
	QComboBox *presetCombo_ = nullptr;
	QLabel *statusLabel_ = nullptr;
	QListWidget *recentList_ = nullptr;
	QTimer *stateTimer_ = nullptr;
	QTimer *idleTimer_ = nullptr;

	std::unique_ptr<ClipLibraryWindow> clipWindow_;
	std::unique_ptr<RegionOverlay> regionOverlay_;

	std::string activePresetId_;   // id of the preset selected in the combo
	CaptureRegion currentRegion_;  // active recording region (disabled = full screen)
	QSize canvasSize_;             // capture canvas resolution (primary screen)
	bool autoPaused_ = false;      // paused by the idle monitor (vs. manually)
	QString defaultFolder_;        // folder for brand-new presets
};

} // namespace harpia
