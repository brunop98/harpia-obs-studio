#include "AudioPanel.hpp"

#include "core/AudioManager.hpp"

#include <QCheckBox>
#include <QGridLayout>
#include <QProgressBar>
#include <QSignalBlocker>

#include <algorithm>

namespace harpia {

namespace {

// Fixed width of the label column so every meter starts at the same x.
constexpr int kLabelColumn = 210;
// Displayed device names are capped at this many characters.
constexpr int kMaxNameChars = 20;

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

// Cap a device name at kMaxNameChars, ending with an ellipsis if trimmed.
QString shortName(const QString &full)
{
	if (full.size() <= kMaxNameChars)
		return full;
	return full.left(kMaxNameChars - 1) + QChar(0x2026); // …
}

} // namespace

AudioPanel::AudioPanel(AudioManager &audio, QWidget *parent) : QWidget(parent), audio_(audio)
{
	// Grid keeps the label column a fixed width and lets the meter fill the rest,
	// so all bars line up regardless of label length.
	auto *grid = new QGridLayout(this);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setHorizontalSpacing(12);
	grid->setVerticalSpacing(2);
	grid->setColumnMinimumWidth(0, kLabelColumn);
	grid->setColumnStretch(1, 1);

	int r = 0;

	// PC (desktop/system) audio row.
	pcCheck_ = new QCheckBox(QStringLiteral("PC Audio"), this);
	pcCheck_->setFixedWidth(kLabelColumn);
	pcCheck_->setToolTip(QStringLiteral("System / desktop audio"));
	pcMeter_ = makeMeter(this);
	grid->addWidget(pcCheck_, r, 0);
	grid->addWidget(pcMeter_, r, 1);
	++r;

	connect(pcCheck_, &QCheckBox::toggled, this, [this](bool on) {
		audio_.setDesktopEnabled(on);
		emit changed();
	});

	// One row per detected input (mic) device — excluding the synthetic
	// "Default" entry so only real devices are shown.
	const std::vector<AudioDevice> devices = AudioManager::inputDevices();
	for (const AudioDevice &dev : devices) {
		if (dev.id == "default")
			continue;

		MicRow row;
		row.id = dev.id;
		const QString full = QString::fromStdString(dev.name);
		row.check = new QCheckBox(QStringLiteral("Mic: %1").arg(shortName(full)), this);
		row.check->setFixedWidth(kLabelColumn);
		row.check->setToolTip(full); // full name on hover
		row.meter = makeMeter(this);

		grid->addWidget(row.check, r, 0);
		grid->addWidget(row.meter, r, 1);
		++r;

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
