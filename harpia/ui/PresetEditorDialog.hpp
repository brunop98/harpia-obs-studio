#pragma once

#include "model/Preset.hpp"

#include <QDialog>

class QLineEdit;
class QComboBox;
class QCheckBox;
class QSpinBox;

namespace harpia {

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
	void onResolutionModeChanged();
	void accept() override;

private:
	Preset result_; // seeded from the input; updated on accept

	QLineEdit *nameEdit_ = nullptr;
	QComboBox *formatCombo_ = nullptr;
	QComboBox *fpsCombo_ = nullptr;
	QComboBox *resolutionCombo_ = nullptr;
	QSpinBox *widthSpin_ = nullptr;
	QSpinBox *heightSpin_ = nullptr;
	QLineEdit *folderEdit_ = nullptr;
	QCheckBox *gpuCheck_ = nullptr;
	QSpinBox *idleSpin_ = nullptr;
	QLineEdit *templateEdit_ = nullptr;
};

} // namespace harpia
