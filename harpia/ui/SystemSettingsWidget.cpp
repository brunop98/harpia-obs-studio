#include "ui/SystemSettingsWidget.hpp"

#include "ui/InfoHint.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace harpia {

using os_integration::GpuPreference;

SystemSettingsWidget::SystemSettingsWidget(QWidget *parent, GetGpu get, SetGpu set)
	: QWidget(parent), set_(std::move(set))
{
	auto *lay = new QVBoxLayout(this);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->setSpacing(8);
	lay->addWidget(infoHint(QStringLiteral("Settings for this computer, not for a recording preset. "
					       "They apply the next time Harpia starts."),
				this),
		       0, Qt::AlignLeft);

	auto *form = new QFormLayout;
	form->setHorizontalSpacing(10);
	form->setVerticalSpacing(6);
	gpuCombo_ = new QComboBox(this);
	for (GpuPreference p : {GpuPreference::Automatic, GpuPreference::PowerSaving, GpuPreference::HighPerformance})
		gpuCombo_->addItem(QString::fromLatin1(os_integration::gpuPreferenceName(p)), int(p));
	gpuCombo_->setToolTip(QStringLiteral(
		"For laptops with two graphics cards: the same choice as Windows Settings > Display > "
		"Graphics. High performance can make recording and exporting faster; if recordings of the "
		"screen then come out black, set this back to Automatic."));
	form->addRow(QStringLiteral("Graphics card"), gpuCombo_);
	status_ = new QLabel(this);
	status_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	status_->setWordWrap(true);
	form->addRow(QString(), status_);
	lay->addLayout(form);

	saved_ = get ? get() : GpuPreference::Automatic;
	gpuCombo_->setCurrentIndex(gpuCombo_->findData(int(saved_)));
	connect(gpuCombo_, &QComboBox::activated, this, [this](int index) {
		const auto p = GpuPreference(gpuCombo_->itemData(index).toInt());
		if (p == saved_)
			return;
		if (set_ && set_(p)) {
			saved_ = p;
			status_->setText(QStringLiteral("Saved. Restart Harpia to use it."));
		} else {
			// Put the box back to what is really in effect, so it never shows
			// a choice that did not stick.
			gpuCombo_->setCurrentIndex(gpuCombo_->findData(int(saved_)));
			status_->setText(QStringLiteral("Could not save the setting."));
		}
	});
}

} // namespace harpia
