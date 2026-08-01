#pragma once

#include "model/Preset.hpp"

#include <QColor>
#include <QDialog>
#include <QList>
#include <QStringList>

class QLineEdit;
class QComboBox;
class QCheckBox;
class QSpinBox;
class QLabel;
class QSlider;
class QPushButton;
class QListWidget;

namespace harpia {

class MousePreview;

// Modal editor for a single recording Preset. Construct with the preset to edit
// (or a fresh default for "new"), exec(), and on Accepted read back result().
class PresetEditorDialog : public QDialog {
	Q_OBJECT
public:
	explicit PresetEditorDialog(const Preset &preset, QWidget *parent = nullptr);

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

private:
	void pickColor(QColor &target, QPushButton *button);

private:
	Preset result_; // seeded from the input; updated on accept

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
	QCheckBox *desktopAudioCheck_ = nullptr;
	QList<QCheckBox *> micChecks_;
	QStringList micIds_;

	// Mouse section
	QCheckBox *mouseCursorCheck_ = nullptr;
	QCheckBox *mouseAreaCheck_ = nullptr;
	QPushButton *highlightColorBtn_ = nullptr;
	QSlider *highlightSizeSlider_ = nullptr;
	QCheckBox *mouseClicksCheck_ = nullptr;
	QPushButton *leftColorBtn_ = nullptr;
	QPushButton *rightColorBtn_ = nullptr;
	MousePreview *mousePreview_ = nullptr;
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
