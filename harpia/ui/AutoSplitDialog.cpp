#include "AutoSplitDialog.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace harpia {

AutoSplitDialog::AutoSplitDialog(const AutoSplitSettings &input, bool always, QWidget *parent)
	: QDialog(parent)
{
	const AutoSplitSettings initial = auto_split_settings::validated(input);
	setWindowTitle(QStringLiteral("Auto Split"));
	setObjectName(QStringLiteral("autoSplitDialog"));
	setMinimumWidth(440);
	auto *layout = new QVBoxLayout(this);
	auto *recording = new QGroupBox(QStringLiteral("Recording"), this);
	auto *recordingLayout = new QVBoxLayout(recording);
	enabled_ = new QCheckBox(QStringLiteral("&Auto split the recording file"), recording);
	enabled_->setObjectName(QStringLiteral("autoSplitEnabled"));
	enabled_->setChecked(initial.enabled);
	recordingLayout->addWidget(enabled_);

	auto *limits = new QGridLayout;
	limits->setContentsMargins(24, 0, 0, 0);
	time_ = new QRadioButton(QStringLiteral("When the recording &time reaches"), recording);
	time_->setObjectName(QStringLiteral("autoSplitTime"));
	size_ = new QRadioButton(QStringLiteral("When the file &size reaches"), recording);
	size_->setObjectName(QStringLiteral("autoSplitSize"));
	auto *mode = new QButtonGroup(this);
	mode->addButton(time_);
	mode->addButton(size_);
	minutes_ = new QSpinBox(recording);
	minutes_->setObjectName(QStringLiteral("autoSplitMinutes"));
	minutes_->setAccessibleName(QStringLiteral("Recording duration in minutes"));
	minutes_->setRange(1, auto_split_settings::maxMinutes);
	minutes_->setValue(initial.minutes);
	minutes_->setToolTip(QStringLiteral("Recorded minutes per part. Paused time is excluded."));
	sizeMB_ = new QSpinBox(recording);
	sizeMB_->setObjectName(QStringLiteral("autoSplitSizeMB"));
	sizeMB_->setAccessibleName(QStringLiteral("File size in megabytes"));
	sizeMB_->setRange(1, auto_split_settings::maxSizeMB);
	sizeMB_->setValue(initial.sizeMB);
	sizeMB_->setToolTip(QStringLiteral("Approximate size per part. 1 MB here is 1,048,576 bytes."));
	limits->addWidget(time_, 0, 0);
	limits->addWidget(minutes_, 0, 1);
	limits->addWidget(new QLabel(QStringLiteral("minutes"), recording), 0, 2);
	limits->addWidget(size_, 1, 0);
	limits->addWidget(sizeMB_, 1, 1);
	limits->addWidget(new QLabel(QStringLiteral("MB"), recording), 1, 2);
	limits->setColumnStretch(0, 1);
	recordingLayout->addLayout(limits);

	auto *scope = new QHBoxLayout;
	scope->setContentsMargins(24, 8, 0, 0);
	once_ = new QRadioButton(QStringLiteral("Only t&his time"), recording);
	once_->setObjectName(QStringLiteral("autoSplitOnce"));
	once_->setToolTip(QStringLiteral("Applies to the next successful recording, then restores your saved setting."));
	always_ = new QRadioButton(QStringLiteral("Al&ways"), recording);
	always_->setObjectName(QStringLiteral("autoSplitAlways"));
	always_->setToolTip(QStringLiteral("Save this setting for future recordings, including after restarting Harpia."));
	auto *scopeGroup = new QButtonGroup(this);
	scopeGroup->addButton(once_);
	scopeGroup->addButton(always_);
	scope->addWidget(once_);
	scope->addWidget(always_);
	scope->addStretch(1);
	recordingLayout->addLayout(scope);
	layout->addWidget(recording);

	auto *help = new QLabel(
		QStringLiteral("Recording continues automatically in a new file. "
			       "Parts split at video keyframes, so duration and size can exceed the selected limit. "
			       "Paused time is excluded.\n\n"
			       "Available for MP4, MKV, MOV and AVI video recordings. "
			       "A separate webcam recording stays in one file."), this);
	help->setWordWrap(true);
	help->setObjectName(QStringLiteral("autoSplitHelp"));
	layout->addWidget(help);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	buttons->setObjectName(QStringLiteral("autoSplitButtons"));
	buttons->button(QDialogButtonBox::Ok)->setDefault(true);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);

	time_->setChecked(initial.mode == AutoSplitSettings::Mode::Time);
	size_->setChecked(initial.mode == AutoSplitSettings::Mode::Size);
	once_->setChecked(!always);
	always_->setChecked(always);
	connect(enabled_, &QCheckBox::toggled, this, [this]() { syncEnabled(); });
	connect(time_, &QRadioButton::toggled, this, [this]() { syncEnabled(); });
	connect(size_, &QRadioButton::toggled, this, [this]() { syncEnabled(); });
	syncEnabled();
}

AutoSplitSettings AutoSplitDialog::settings() const
{
	AutoSplitSettings value;
	value.enabled = enabled_->isChecked();
	value.mode = size_->isChecked() ? AutoSplitSettings::Mode::Size : AutoSplitSettings::Mode::Time;
	value.minutes = minutes_->value();
	value.sizeMB = sizeMB_->value();
	return value;
}

bool AutoSplitDialog::always() const
{
	return always_->isChecked();
}

void AutoSplitDialog::syncEnabled()
{
	const bool enabled = enabled_->isChecked();
	time_->setEnabled(enabled);
	size_->setEnabled(enabled);
	minutes_->setEnabled(enabled && time_->isChecked());
	sizeMB_->setEnabled(enabled && size_->isChecked());
}

} // namespace harpia
