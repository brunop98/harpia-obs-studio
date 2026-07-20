#include "ExportOptionsDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace harpia {

ExportOptionsDialog::ExportOptionsDialog(const QString &defaultName, QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Export clip"));
	setModal(true);
	setMinimumWidth(360);

	auto *root = new QVBoxLayout(this);
	auto *form = new QFormLayout;

	nameEdit_ = new QLineEdit(defaultName, this);
	nameEdit_->setPlaceholderText(QStringLiteral("Output file name"));
	form->addRow(QStringLiteral("File name"), nameEdit_);

	formatCombo_ = new QComboBox(this);
	formatCombo_->addItem(QStringLiteral("GIF (animated)"), int(ClipExporter::Format::Gif));
	formatCombo_->addItem(QStringLiteral("MP4 (H.264)"), int(ClipExporter::Format::Mp4));
	formatCombo_->addItem(QStringLiteral("MKV (H.264)"), int(ClipExporter::Format::Mkv));
	formatCombo_->addItem(QStringLiteral("MOV (H.264)"), int(ClipExporter::Format::Mov));
	if (ClipExporter::webmAvailable())
		formatCombo_->addItem(QStringLiteral("WebM (VP9)"), int(ClipExporter::Format::WebM));
	form->addRow(QStringLiteral("Format"), formatCombo_);
	root->addLayout(form);

	// Video-only options.
	videoRow_ = new QWidget(this);
	auto *vform = new QFormLayout(videoRow_);
	vform->setContentsMargins(0, 0, 0, 0);
	qualityCombo_ = new QComboBox(videoRow_);
	qualityCombo_->addItem(QStringLiteral("High quality"), 18);
	qualityCombo_->addItem(QStringLiteral("Balanced"), 23);
	qualityCombo_->addItem(QStringLiteral("Small file"), 28);
	qualityCombo_->setCurrentIndex(1);
	vform->addRow(QStringLiteral("Quality"), qualityCombo_);
	audioCheck_ = new QCheckBox(QStringLiteral("Keep audio"), videoRow_);
	audioCheck_->setChecked(true);
	vform->addRow(QString(), audioCheck_);
	root->addWidget(videoRow_);

	// GIF-only options.
	gifRow_ = new QWidget(this);
	auto *gform = new QFormLayout(gifRow_);
	gform->setContentsMargins(0, 0, 0, 0);
	gifFpsSpin_ = new QSpinBox(gifRow_);
	gifFpsSpin_->setRange(1, 50);
	gifFpsSpin_->setValue(15);
	gifFpsSpin_->setSuffix(QStringLiteral(" fps"));
	gform->addRow(QStringLiteral("Frame rate"), gifFpsSpin_);
	gifWidthSpin_ = new QSpinBox(gifRow_);
	gifWidthSpin_->setRange(120, 1920);
	gifWidthSpin_->setSingleStep(20);
	gifWidthSpin_->setValue(640);
	gifWidthSpin_->setSuffix(QStringLiteral(" px wide"));
	gifWidthSpin_->setToolTip(QStringLiteral("Smaller width = smaller GIF. Height keeps the aspect ratio."));
	gform->addRow(QStringLiteral("Size"), gifWidthSpin_);
	root->addWidget(gifRow_);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
	buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("Export"));
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);

	connect(formatCombo_, &QComboBox::currentIndexChanged, this, &ExportOptionsDialog::onFormatChanged);
	onFormatChanged();
}

void ExportOptionsDialog::onFormatChanged()
{
	const bool gif = format() == ClipExporter::Format::Gif;
	gifRow_->setVisible(gif);
	videoRow_->setVisible(!gif);
}

QString ExportOptionsDialog::fileName() const
{
	return nameEdit_->text().trimmed();
}

ClipExporter::Format ExportOptionsDialog::format() const
{
	return ClipExporter::Format(formatCombo_->currentData().toInt());
}

int ExportOptionsDialog::gifFps() const
{
	return gifFpsSpin_->value();
}

int ExportOptionsDialog::gifWidth() const
{
	return gifWidthSpin_->value();
}

int ExportOptionsDialog::videoCrf() const
{
	return qualityCombo_->currentData().toInt();
}

bool ExportOptionsDialog::keepAudio() const
{
	return audioCheck_->isChecked();
}

} // namespace harpia
