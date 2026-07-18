#pragma once

#include "model/Preset.hpp"

#include <QColor>
#include <QDialog>

class QLineEdit;
class QComboBox;
class QCheckBox;
class QSpinBox;
class QLabel;
class QSlider;
class QPushButton;

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

private slots:
	void browseFolder();
	void updateValidation();
	void updateMousePreview();
	void accept() override;

private:
	void pickColor(QColor &target, QPushButton *button);

private:
	Preset result_; // seeded from the input; updated on accept

	QLineEdit *nameEdit_ = nullptr;
	QComboBox *formatCombo_ = nullptr;
	QComboBox *codecCombo_ = nullptr;
	QComboBox *fpsCombo_ = nullptr;
	QComboBox *frameRateModeCombo_ = nullptr;
	QSpinBox *bitrateSpin_ = nullptr;
	QComboBox *monitorCombo_ = nullptr;
	QLineEdit *folderEdit_ = nullptr;
	QCheckBox *gpuCheck_ = nullptr;
	QSpinBox *idleSpin_ = nullptr;
	QLineEdit *templateEdit_ = nullptr;
	QLabel *validationLabel_ = nullptr;

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
