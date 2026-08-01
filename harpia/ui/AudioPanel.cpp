#include "AudioPanel.hpp"

#include "core/AudioManager.hpp"

#include <QCheckBox>
#include <QGridLayout>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QSlider>

#include <algorithm>

namespace harpia {

namespace {

// Fixed width of the label column so every meter starts at the same x.
constexpr int kLabelColumn = 240;
// Displayed device names are capped at this many characters.
constexpr int kMaxNameChars = 30;
// Width of the per-source volume slider column.
constexpr int kSliderWidth = 110;

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

// A compact volume slider (0..100%, defaults to 100). Styled explicitly —
// the app palette's highlight is the record-red accent, which Fusion would
// otherwise paint into the groove and make full volume look like a warning.
QSlider *makeVolumeSlider(QWidget *parent)
{
	auto *s = new QSlider(Qt::Horizontal, parent);
	s->setRange(0, 100);
	s->setValue(100);
	s->setFixedWidth(kSliderWidth);
	s->setToolTip(QStringLiteral("Recording volume: 100%"));
	s->setStyleSheet(QStringLiteral(
		"QSlider::groove:horizontal { background:#202225; border:1px solid #303338;"
		" height:4px; border-radius:2px; }"
		"QSlider::sub-page:horizontal { background:#3d84b8; border-radius:2px; }"
		"QSlider::handle:horizontal { background:#cfd6de; width:10px; height:10px;"
		" margin:-4px 0; border-radius:5px; }"
		"QSlider::handle:horizontal:hover { background:#ffffff; }"));
	return s;
}

void updateVolumeTip(QSlider *s, int value)
{
	s->setToolTip(QStringLiteral("Recording volume: %1%").arg(value));
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
	// Grid keeps the label + slider columns a fixed width and lets the meter
	// fill the rest, so all bars line up regardless of label length.
	auto *grid = new QGridLayout(this);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setHorizontalSpacing(12);
	grid->setVerticalSpacing(2);
	grid->setColumnMinimumWidth(0, kLabelColumn);
	grid->setColumnStretch(2, 1);

	int r = 0;

	// PC (desktop/system) audio row.
	pcCheck_ = new QCheckBox(QStringLiteral("PC Audio"), this);
	pcCheck_->setFixedWidth(kLabelColumn);
	pcCheck_->setToolTip(QStringLiteral("System / desktop audio"));
	pcSlider_ = makeVolumeSlider(this);
	pcMeter_ = makeMeter(this);
	grid->addWidget(pcCheck_, r, 0);
	grid->addWidget(pcSlider_, r, 1);
	grid->addWidget(pcMeter_, r, 2);
	++r;

	connect(pcCheck_, &QCheckBox::toggled, this, [this](bool on) {
		audio_.setDesktopEnabled(on);
		emit changed();
	});
	connect(pcSlider_, &QSlider::valueChanged, this, [this](int v) {
		audio_.setDesktopVolume(v / 100.f);
		updateVolumeTip(pcSlider_, v);
		if (!pcSlider_->isSliderDown())
			emit changed(); // keyboard/wheel change — persist now
	});
	connect(pcSlider_, &QSlider::sliderReleased, this, [this]() { emit changed(); });

	// One row per detected input (mic) device — excluding the synthetic
	// "Default" entry so only real devices are shown.
	const std::vector<AudioDevice> devices = AudioManager::inputDevices();
	for (const AudioDevice &dev : devices) {
		if (dev.id == "default")
			continue;

		MicRow row;
		row.id = dev.id;
		const QString full = QString::fromStdString(dev.name);
		row.fullName = full;
		row.check = new QCheckBox(QStringLiteral("Mic: %1").arg(shortName(full)), this);
		row.check->setFixedWidth(kLabelColumn);
		row.check->setToolTip(full); // full name on hover
		row.slider = makeVolumeSlider(this);
		row.meter = makeMeter(this);

		grid->addWidget(row.check, r, 0);
		grid->addWidget(row.slider, r, 1);
		grid->addWidget(row.meter, r, 2);
		++r;

		const std::string id = dev.id;
		connect(row.check, &QCheckBox::toggled, this, [this, id](bool on) {
			audio_.setMicEnabled(id, on);
			syncMicCap();
			emit changed();
		});
		connect(row.slider, &QSlider::valueChanged, this,
			[this, id, s = row.slider](int v) {
				audio_.setMicVolume(id, v / 100.f);
				updateVolumeTip(s, v);
				if (!s->isSliderDown())
					emit changed();
			});
		connect(row.slider, &QSlider::sliderReleased, this, [this]() { emit changed(); });

		micRows_.push_back(row);
	}
}

void AudioPanel::load(bool desktopOn, const std::vector<std::string> &micIds, double desktopVolume,
		      const std::map<std::string, double> &micVolumes)
{
	{
		QSignalBlocker block(pcCheck_);
		pcCheck_->setChecked(desktopOn);
	}
	{
		QSignalBlocker block(pcSlider_);
		const int v = std::clamp(int(desktopVolume * 100.0 + 0.5), 0, 100);
		pcSlider_->setValue(v);
		updateVolumeTip(pcSlider_, v);
	}
	audio_.setDesktopVolume(float(desktopVolume));
	audio_.setDesktopEnabled(desktopOn);

	for (MicRow &row : micRows_) {
		const bool on = std::find(micIds.begin(), micIds.end(), row.id) != micIds.end();
		auto volIt = micVolumes.find(row.id);
		const double vol = volIt != micVolumes.end() ? volIt->second : 1.0;
		{
			QSignalBlocker block(row.check);
			row.check->setChecked(on);
		}
		{
			QSignalBlocker block(row.slider);
			const int v = std::clamp(int(vol * 100.0 + 0.5), 0, 100);
			row.slider->setValue(v);
			updateVolumeTip(row.slider, v);
		}
		audio_.setMicVolume(row.id, float(vol)); // before enable, so it applies on create
		audio_.setMicEnabled(row.id, on);
	}
	syncMicCap();
}

void AudioPanel::syncMicCap()
{
	// The mixer has four mic channels (AudioManager, channels 3..6). A fifth
	// enable used to be dropped with a log line and a checkbox that stayed
	// ticked -- lying about what the recording would contain. Instead, once
	// four are on, the remaining boxes disable with a tooltip that says why.
	int on = 0;
	for (const MicRow &row : micRows_)
		if (row.check->isChecked())
			++on;
	const bool full = on >= 4;
	for (MicRow &row : micRows_) {
		if (row.check->isChecked())
			continue; // an enabled row must always stay un-tickable
		row.check->setEnabled(!full);
		row.check->setToolTip(full ? QStringLiteral("Up to 4 microphones can record at once — "
							    "untick one to use this device.\n%1")
						     .arg(row.fullName)
					   : row.fullName);
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

double AudioPanel::desktopVolume() const
{
	return pcSlider_->value() / 100.0;
}

std::map<std::string, double> AudioPanel::micVolumes() const
{
	std::map<std::string, double> vols;
	for (const MicRow &row : micRows_)
		vols[row.id] = row.slider->value() / 100.0;
	return vols;
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
