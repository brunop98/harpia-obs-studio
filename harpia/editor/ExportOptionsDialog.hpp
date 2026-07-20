#pragma once

#include "ClipExporter.hpp"

#include <QDialog>
#include <QString>

class QLineEdit;
class QComboBox;
class QSpinBox;
class QCheckBox;
class QWidget;

namespace harpia {

// Collects the export choices (file name, format, and format-specific options)
// before processing. Trim range and crop come from the editor window, not here.
class ExportOptionsDialog : public QDialog {
	Q_OBJECT
public:
	ExportOptionsDialog(const QString &defaultName, QWidget *parent = nullptr);

	QString fileName() const;               // base name, no extension
	ClipExporter::Format format() const;
	int gifFps() const;
	double speed() const; // playback speed multiplier (1.0 = normal)
	int videoCrf() const;
	bool keepAudio() const;

private slots:
	void onFormatChanged();

private:
	QLineEdit *nameEdit_ = nullptr;
	QComboBox *formatCombo_ = nullptr;
	QComboBox *qualityCombo_ = nullptr; // video CRF
	QComboBox *speedCombo_ = nullptr;   // playback speed
	QCheckBox *audioCheck_ = nullptr;
	QSpinBox *gifFpsSpin_ = nullptr;
	QWidget *gifRow_ = nullptr;
	QWidget *videoRow_ = nullptr;
};

} // namespace harpia
