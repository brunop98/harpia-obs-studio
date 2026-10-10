#include "ui/SystemSettingsWidget.hpp"

#include "core/RemoteSettings.hpp"
#include "ui/InfoHint.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

namespace harpia {

using os_integration::GpuPreference;

SystemSettingsWidget::SystemSettingsWidget(QWidget *parent, GetGpu get, SetGpu set) : QWidget(parent)
{
	if (!get && !set && os_integration::gpuPreferenceSupported()) {
		get = os_integration::gpuPreference;
		set = os_integration::setGpuPreference;
	}
	set_ = std::move(set);

	auto *lay = new QVBoxLayout(this);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->setSpacing(8);
	lay->addWidget(infoHint(QStringLiteral("Settings for this computer, not for a recording preset."), this), 0,
		       Qt::AlignLeft);

	auto *form = new QFormLayout;
	form->setHorizontalSpacing(10);
	form->setVerticalSpacing(6);

	if (get && set_) {
		gpuCombo_ = new QComboBox(this);
		for (GpuPreference p :
		     {GpuPreference::Automatic, GpuPreference::PowerSaving, GpuPreference::HighPerformance})
			gpuCombo_->addItem(QString::fromLatin1(os_integration::gpuPreferenceName(p)), int(p));
		gpuCombo_->setToolTip(QStringLiteral(
			"For laptops with two graphics cards: the same choice as Windows Settings > Display > "
			"Graphics. High performance can make recording and exporting faster; if recordings of the "
			"screen then come out black, set this back to Automatic. Applies the next time Harpia "
			"starts."));
		form->addRow(QStringLiteral("Graphics card"), gpuCombo_);
		status_ = new QLabel(this);
		status_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
		status_->setWordWrap(true);
		form->addRow(QString(), status_);

		saved_ = get();
		gpuCombo_->setCurrentIndex(gpuCombo_->findData(int(saved_)));
		connect(gpuCombo_, &QComboBox::activated, this, [this](int index) {
			const auto p = GpuPreference(gpuCombo_->itemData(index).toInt());
			if (p == saved_)
				return;
			if (set_ && set_(p)) {
				saved_ = p;
				status_->setText(QStringLiteral("Saved. Restart Harpia to use it."));
			} else {
				// Put the box back to what is really in effect, so it never
				// shows a choice that did not stick.
				gpuCombo_->setCurrentIndex(gpuCombo_->findData(int(saved_)));
				status_->setText(QStringLiteral("Could not save the setting."));
			}
		});
	}

	// Unity control.
	const RemoteSettings rs = RemoteSettings::load();
	remoteCheck_ = new QCheckBox(QStringLiteral("Let Unity start recordings (Play Mode Recorder)"), this);
	remoteCheck_->setChecked(rs.enabled);
	remoteCheck_->setToolTip(QStringLiteral(
		"With the Harpia Play Mode Recorder package in a Unity project, entering Play Mode records the "
		"Game view with this preset's settings; pausing pauses, leaving Play Mode saves. Only programs "
		"on this computer can reach it, and web pages are refused."));
	form->addRow(QStringLiteral("Unity"), remoteCheck_);
	remotePort_ = new QSpinBox(this);
	remotePort_->setRange(1024, 65535);
	remotePort_->setValue(rs.port);
	remotePort_->setToolTip(QStringLiteral(
		"The local port Unity talks to (127.0.0.1). Change it only if another program uses it, and set "
		"the same number in Unity's Preferences > Harpia Recorder."));
	remotePort_->setEnabled(rs.enabled);
	form->addRow(QStringLiteral("Port"), remotePort_);
	auto *note = new QLabel(QStringLiteral("Applied when you close Settings."), this);
	note->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	form->addRow(QString(), note);
	connect(remoteCheck_, &QCheckBox::toggled, this, [this](bool on) {
		remotePort_->setEnabled(on);
		saveRemote();
	});
	connect(remotePort_, &QSpinBox::valueChanged, this, [this](int) { saveRemote(); });

	lay->addLayout(form);
}

void SystemSettingsWidget::saveRemote()
{
	RemoteSettings rs;
	rs.enabled = remoteCheck_->isChecked();
	rs.port = remotePort_->value();
	rs.save();
}

} // namespace harpia
