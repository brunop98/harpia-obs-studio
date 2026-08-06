#pragma once

#include "core/AudioManager.hpp"
#include "core/ModeCapabilities.hpp"
#include "model/Preset.hpp"

#include <QColor>
#include <QDialog>
#include <QList>
#include <QStringList>

class QLineEdit;
class QComboBox;
class QCheckBox;

namespace harpia {
class AudioPanel;
}
using harpia::AudioPanel;
class QSpinBox;
class QKeySequenceEdit;
class QLabel;
class QSlider;
class QPushButton;
class QListWidget;

namespace harpia {

class MousePreview;
class SpotlightPreview;

// Modal editor for a single recording Preset. Construct with the preset to edit
// (or a fresh default for "new"), exec(), and on Accepted read back result().
class PresetEditorDialog : public QDialog {
	Q_OBJECT
public:
	// `audio` is the live capture: the Audio page drives it directly, so the
	// level bars are real and a microphone can be heard before recording rather
	// than after. The caller re-asserts its own state afterwards, whichever
	// button was pressed.
	PresetEditorDialog(const Preset &preset, AudioManager &audio, QWidget *parent = nullptr);

	// Cancel asks before throwing away real edits, and says nothing when there
	// are none.
	void reject() override;

	// The edited preset (valid after the dialog is accepted).
	Preset result() const { return result_; }

	// Select a settings page by its nav title (e.g. "Webcam"). No-op if unknown.
	void showPage(const QString &title);

private slots:
	void browseFolder();
	void updateValidation();
	void updateMousePreview();
	void updateFilenamePreview();
	void accept() override;
	// The four shortcuts this dialog owns, checked against each other before
	// anything is saved. Puts the conflict dialog up when two of them share a
	// key and writes the user's fix back into the editors. False means the save
	// was abandoned -- either they cancelled, or they left a duplicate, which
	// the dialog does not permit.
	bool resolveShortcutConflicts();

private:
	void pickColor(QColor &target, QPushButton *button);

private:
	Preset result_;   // seeded from the input; updated on accept
	Preset original_; // exactly what was passed in, for the Cancel comparison

	// Read every page's widgets into `out`. Used by accept() to produce the
	// result, and by reject() to work out whether anything was actually edited.
	void collectInto(Preset &out) const;

	QListWidget *nav_ = nullptr; // page selector, for showPage()
	QLineEdit *nameEdit_ = nullptr;
	QComboBox *formatCombo_ = nullptr;
	QComboBox *codecCombo_ = nullptr;
	QComboBox *fpsCombo_ = nullptr;
	QComboBox *frameRateModeCombo_ = nullptr;
	QComboBox *bitrateCombo_ = nullptr;
	QComboBox *audioBitrateCombo_ = nullptr;
	QSpinBox *bitrateSpin_ = nullptr;
	QLineEdit *folderEdit_ = nullptr;
	QCheckBox *gpuCheck_ = nullptr;
	QSpinBox *idleSpin_ = nullptr;
	QComboBox *countdownCombo_ = nullptr;
	QSpinBox *minLengthSpin_ = nullptr;

	// Recording border (Full Screen)
	QCheckBox *borderCheck_ = nullptr;
	QPushButton *borderColorBtn_ = nullptr;
	QSpinBox *borderThicknessSpin_ = nullptr;
	QColor borderColor_;
	// Move handle for the capture region (Custom Region only)
	QCheckBox *regionHandleCheck_ = nullptr;
	QLineEdit *templateEdit_ = nullptr;
	QLabel *templatePreview_ = nullptr;
	QLineEdit *driveLinkEdit_ = nullptr;
	QLabel *validationLabel_ = nullptr;

	// Audio page
	// The same panel the main window uses, so "PC audio, mics, volumes, levels"
	// looks and behaves identically in both places -- and there is one
	// implementation of it rather than two that drift.
	AudioPanel *audioPanel_ = nullptr;
	// Rows (label + description + control) that GIF hides, held so the format
	// gate can take them off the page rather than grey them out.
	QWidget *codecRow_ = nullptr;
	QWidget *frameRateModeRow_ = nullptr;
	QWidget *bitrateRow_ = nullptr;
	QWidget *audioSection_ = nullptr; // the panel + its rescan button
	QWidget *audioBitrateRow_ = nullptr;
	// What this preset records. Fixed for the life of the dialog -- the capture
	// mode is chosen on the main window -- and decides which pages exist.
	RecordMode mode_ = RecordMode::Monitor;
	QLabel *audioGifNote_ = nullptr; // "GIF has no audio track", shown only for GIF
	// Hotkeys (app-wide QSettings, edited here for want of a better home)
	QKeySequenceEdit *recordKeyEdit_ = nullptr;
	QKeySequenceEdit *pauseKeyEdit_ = nullptr;


	// Mouse section
	QCheckBox *mouseCursorCheck_ = nullptr;
	QCheckBox *mouseAreaCheck_ = nullptr;
	QPushButton *highlightColorBtn_ = nullptr;
	QSlider *highlightSizeSlider_ = nullptr;
	QCheckBox *mouseClicksCheck_ = nullptr;
	QPushButton *leftColorBtn_ = nullptr;
	QPushButton *rightColorBtn_ = nullptr;
	MousePreview *mousePreview_ = nullptr;
	// Follow Mouse (Custom Region only)
	QCheckBox *followCheck_ = nullptr;
	QComboBox *followProfileCombo_ = nullptr;
	QSlider *followPaddingSlider_ = nullptr;
	QSlider *followSmoothSlider_ = nullptr;
	QComboBox *followAxisCombo_ = nullptr;
	QKeySequenceEdit *followShortcutEdit_ = nullptr;
	bool followProfileApplying_ = false; // combo is writing the sliders, not the user

	// Spotlight page
	QCheckBox *spotCheck_ = nullptr;
	QCheckBox *spotStartOnCheck_ = nullptr;
	QKeySequenceEdit *spotShortcutEdit_ = nullptr;
	QSlider *spotSizeSlider_ = nullptr;
	QSlider *spotDarkSlider_ = nullptr;
	QSlider *spotRoundSlider_ = nullptr;
	SpotlightPreview *spotPreview_ = nullptr;
	// The width in device pixels of the display this preset records, resolved
	// once at construction. The preview needs it to show the patch at its true
	// relative size, and the preset's monitor cannot change from this dialog.
	int spotScreenW_ = 1920;
	void syncSpotlightPreview();

	// Zoom page (Full Screen only)
	QCheckBox *zoomCheck_ = nullptr;
	QKeySequenceEdit *zoomShortcutEdit_ = nullptr;
	QSlider *zoomPercentSlider_ = nullptr;
	QSlider *zoomAnimSlider_ = nullptr;
	QSlider *zoomFollowSmoothSlider_ = nullptr;
	QSlider *zoomFollowPadSlider_ = nullptr;
	QComboBox *zoomFollowAxisCombo_ = nullptr;

	QColor highlightColor_;
	QColor leftColor_;
	QColor rightColor_;

	// Webcam section
	QCheckBox *webcamCheck_ = nullptr;
	QComboBox *webcamDeviceCombo_ = nullptr;
	QComboBox *webcamResCombo_ = nullptr;
	QComboBox *webcamFpsCombo_ = nullptr;
	QCheckBox *webcamCustomFolderCheck_ = nullptr;
	QLineEdit *webcamFolderEdit_ = nullptr;
};

} // namespace harpia
