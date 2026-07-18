#include "AudioPanel.hpp"

#include "core/AudioManager.hpp"

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

namespace harpia {

namespace {

// A thin horizontal level bar (0..100), green fill, no text.
QProgressBar *makeMeter(QWidget *parent)
{
	auto *bar = new QProgressBar(parent);
	bar->setRange(0, 100);
	bar->setValue(0);
	bar->setTextVisible(false);
	bar->setFixedHeight(12);
	bar->setStyleSheet(QStringLiteral(
		"QProgressBar { background:#202225; border:1px solid #303338; border-radius:3px; }"
		"QProgressBar::chunk { background:#3fb950; border-radius:3px; }"));
	return bar;
}

} // namespace

AudioPanel::AudioPanel(AudioManager &audio, QWidget *parent) : QWidget(parent), audio_(audio)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(4);

	// PC (desktop/system) audio row.
	pcCheck_ = new QCheckBox(QStringLiteral("Record PC Audio"), this);
	pcMeter_ = makeMeter(this);
	auto *pcRow = new QHBoxLayout;
	pcCheck_->setMinimumWidth(220);
	pcRow->addWidget(pcCheck_);
	pcRow->addWidget(pcMeter_, 1);
	layout->addLayout(pcRow);

	connect(pcCheck_, &QCheckBox::toggled, this, [this](bool on) {
		audio_.setDesktopEnabled(on);
		emit changed();
	});

	// One row per detected input (mic) device.
	const std::vector<AudioDevice> devices = AudioManager::inputDevices();
	for (const AudioDevice &dev : devices) {
		MicRow row;
		row.id = dev.id;
		row.check = new QCheckBox(
			QStringLiteral("Record Mic: %1").arg(QString::fromStdString(dev.name)), this);
		row.check->setMinimumWidth(220);
		row.meter = makeMeter(this);

		auto *r = new QHBoxLayout;
		r->addWidget(row.check);
		r->addWidget(row.meter, 1);
		layout->addLayout(r);

		const std::string id = dev.id;
		connect(row.check, &QCheckBox::toggled, this, [this, id](bool on) {
			audio_.setMicEnabled(id, on);
			emit changed();
		});

		micRows_.push_back(row);
	}
}

void AudioPanel::load(bool desktopOn, const std::vector<std::string> &micIds)
{
	{
		QSignalBlocker block(pcCheck_);
		pcCheck_->setChecked(desktopOn);
	}
	audio_.setDesktopEnabled(desktopOn);

	for (MicRow &row : micRows_) {
		const bool on = std::find(micIds.begin(), micIds.end(), row.id) != micIds.end();
		QSignalBlocker block(row.check);
		row.check->setChecked(on);
		audio_.setMicEnabled(row.id, on);
	}
}

bool AudioPanel::desktopOn() const
{
	return pcCheck_->isChecked();
}

std::vector<std::string> AudioPanel::enabledMicIds() const
{
	std::vector<std::string> ids;
	for (const MicRow &row : micRows_) {
		if (row.check->isChecked())
			ids.push_back(row.id);
	}
	return ids;
}

int AudioPanel::dbToPercent(float db)
{
	// Map -60..0 dB to 0..100%.
	float pct = (db + 60.f) / 60.f * 100.f;
	return std::clamp((int)pct, 0, 100);
}

void AudioPanel::updateMeters()
{
	pcMeter_->setValue(pcCheck_->isChecked() ? dbToPercent(audio_.desktopPeakDb()) : 0);
	for (const MicRow &row : micRows_) {
		const int v = row.check->isChecked() ? dbToPercent(audio_.micPeakDb(row.id)) : 0;
		row.meter->setValue(v);
	}
}

} // namespace harpia
