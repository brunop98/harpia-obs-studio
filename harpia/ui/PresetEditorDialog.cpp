#include "PresetEditorDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace harpia {

PresetEditorDialog::PresetEditorDialog(const Preset &preset, QWidget *parent)
	: QDialog(parent), result_(preset)
{
	setWindowTitle(QStringLiteral("Edit Preset"));
	setModal(true);

	auto *form = new QFormLayout;

	nameEdit_ = new QLineEdit(QString::fromStdString(preset.name), this);
	form->addRow(QStringLiteral("Name"), nameEdit_);

	formatCombo_ = new QComboBox(this);
	formatCombo_->addItem(QStringLiteral("MP4"), int(RecordingFormat::MP4));
	formatCombo_->addItem(QStringLiteral("MKV"), int(RecordingFormat::MKV));
	formatCombo_->addItem(QStringLiteral("GIF"), int(RecordingFormat::GIF));
	formatCombo_->setCurrentIndex(formatCombo_->findData(int(preset.format)));
	form->addRow(QStringLiteral("Format"), formatCombo_);

	// Editable so any custom fps can be typed; quick values are offered.
	fpsCombo_ = new QComboBox(this);
	fpsCombo_->setEditable(true);
	fpsCombo_->setValidator(new QIntValidator(1, 240, fpsCombo_));
	for (int v : {5, 10, 30, 60})
		fpsCombo_->addItem(QString::number(v));
	fpsCombo_->setCurrentText(QString::number(preset.fps));
	form->addRow(QStringLiteral("Frame rate (fps)"), fpsCombo_);

	resolutionCombo_ = new QComboBox(this);
	resolutionCombo_->addItem(QStringLiteral("Native (match display)"), int(ResolutionMode::Native));
	resolutionCombo_->addItem(QStringLiteral("Downscale to…"), int(ResolutionMode::Scaled));
	resolutionCombo_->addItem(QStringLiteral("Custom size"), int(ResolutionMode::Custom));
	resolutionCombo_->setCurrentIndex(resolutionCombo_->findData(int(preset.resolutionMode)));
	form->addRow(QStringLiteral("Resolution"), resolutionCombo_);

	widthSpin_ = new QSpinBox(this);
	widthSpin_->setRange(16, 15360);
	widthSpin_->setValue(preset.width > 0 ? preset.width : 1920);
	heightSpin_ = new QSpinBox(this);
	heightSpin_->setRange(16, 8640);
	heightSpin_->setValue(preset.height > 0 ? preset.height : 1080);
	auto *sizeRow = new QHBoxLayout;
	sizeRow->addWidget(widthSpin_);
	sizeRow->addWidget(new QLabel(QStringLiteral("×"), this));
	sizeRow->addWidget(heightSpin_);
	form->addRow(QStringLiteral("Width × Height"), sizeRow);

	folderEdit_ = new QLineEdit(QString::fromStdString(preset.outputFolder), this);
	auto *browse = new QPushButton(QStringLiteral("Browse…"), this);
	connect(browse, &QPushButton::clicked, this, &PresetEditorDialog::browseFolder);
	auto *folderRow = new QHBoxLayout;
	folderRow->addWidget(folderEdit_, 1);
	folderRow->addWidget(browse);
	form->addRow(QStringLiteral("Output folder"), folderRow);

	gpuCheck_ = new QCheckBox(QStringLiteral("Use GPU compression while recording (NVENC/AMF/QSV)"), this);
	gpuCheck_->setChecked(preset.gpuCompression);
	form->addRow(QString(), gpuCheck_);

	idleSpin_ = new QSpinBox(this);
	idleSpin_->setRange(0, 3600);
	idleSpin_->setSuffix(QStringLiteral(" s"));
	idleSpin_->setSpecialValueText(QStringLiteral("Disabled"));
	idleSpin_->setValue(preset.idleTimeoutSeconds);
	form->addRow(QStringLiteral("Auto-pause after idle"), idleSpin_);

	templateEdit_ = new QLineEdit(QString::fromStdString(preset.filenameTemplate), this);
	form->addRow(QStringLiteral("Filename template"), templateEdit_);
	auto *tokenHelp = new QLabel(
		QStringLiteral("Tokens: {Year} {Month} {Day} {Hour} {Minute} {Second}"), this);
	tokenHelp->setStyleSheet(QStringLiteral("color: gray;"));
	form->addRow(QString(), tokenHelp);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &PresetEditorDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(buttons);

	connect(resolutionCombo_, &QComboBox::currentIndexChanged, this,
		&PresetEditorDialog::onResolutionModeChanged);
	onResolutionModeChanged();
	resize(460, sizeHint().height());
}

void PresetEditorDialog::browseFolder()
{
	const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose output folder"),
							      folderEdit_->text());
	if (!dir.isEmpty())
		folderEdit_->setText(dir);
}

void PresetEditorDialog::onResolutionModeChanged()
{
	const auto mode = ResolutionMode(resolutionCombo_->currentData().toInt());
	const bool needsSize = (mode != ResolutionMode::Native);
	widthSpin_->setEnabled(needsSize);
	heightSpin_->setEnabled(needsSize);
}

void PresetEditorDialog::accept()
{
	const QString name = nameEdit_->text().trimmed();
	if (name.isEmpty()) {
		QMessageBox::warning(this, windowTitle(), QStringLiteral("Please enter a preset name."));
		return;
	}
	if (folderEdit_->text().trimmed().isEmpty()) {
		QMessageBox::warning(this, windowTitle(), QStringLiteral("Please choose an output folder."));
		return;
	}

	result_.name = name.toStdString();
	result_.format = RecordingFormat(formatCombo_->currentData().toInt());
	result_.fps = qMax(1, fpsCombo_->currentText().toInt());
	result_.resolutionMode = ResolutionMode(resolutionCombo_->currentData().toInt());
	result_.width = widthSpin_->value();
	result_.height = heightSpin_->value();
	result_.outputFolder = folderEdit_->text().trimmed().toStdString();
	result_.gpuCompression = gpuCheck_->isChecked();
	result_.idleTimeoutSeconds = idleSpin_->value();
	result_.filenameTemplate = templateEdit_->text().trimmed().toStdString();

	QDialog::accept();
}

} // namespace harpia
