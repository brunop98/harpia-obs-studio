#pragma once

#include <QWidget>
#include <map>
#include <string>
#include <vector>

class QCheckBox;
class QGridLayout;
class QProgressBar;
class QSlider;
class QVBoxLayout;

namespace harpia {

class AudioManager;

// Compact audio panel: a "Record PC Audio" toggle plus one "Record Mic" toggle
// per detected input device, each with a volume slider and a live level bar so
// the user can confirm audio is actually coming through. Changing a control
// immediately drives the live capture (via AudioManager) and emits changed()
// so the owner can persist the settings to the active preset.
class AudioPanel : public QWidget {
	Q_OBJECT
public:
	explicit AudioPanel(AudioManager &audio, QWidget *parent = nullptr);

	// Set the controls (and drive AudioManager) from stored preset values.
	void load(bool desktopOn, const std::vector<std::string> &micIds, double desktopVolume,
		  const std::map<std::string, double> &micVolumes);

	// Rebuild the microphone rows from the devices present RIGHT NOW, keeping
	// what is ticked and each volume. The list was built once at construction,
	// so a microphone plugged in afterwards simply did not exist as far as this
	// panel was concerned -- which is the normal case for a headset someone
	// puts on when they sit down to record.
	void rescanDevices();

	bool desktopOn() const;
	std::vector<std::string> enabledMicIds() const;
	// Every microphone the panel is showing, enabled or not. The recorder holds
	// a source open for each so they can be switched on mid-take.
	std::vector<std::string> knownMicIds() const;
	double desktopVolume() const;                    // linear 0..1
	std::map<std::string, double> micVolumes() const; // every row, keyed by id

signals:
	void changed();

public slots:
	void updateMeters(); // poll AudioManager peak levels into the bars

private:
	static int dbToPercent(float db);

	// Build one row per detected input device. Called at construction and again
	// by rescanDevices().
	void buildMicRows();

	AudioManager &audio_;
	QGridLayout *grid_ = nullptr;
	QCheckBox *pcCheck_ = nullptr;
	QSlider *pcSlider_ = nullptr;
	QProgressBar *pcMeter_ = nullptr;

	struct MicRow {
		std::string id;
		QString fullName; // the device's full name (the checkbox label is shortened)
		QCheckBox *check = nullptr;
		QSlider *slider = nullptr;
		QProgressBar *meter = nullptr;
	};
	std::vector<MicRow> micRows_;
	// Disable the unchecked rows once four mics are on (the mixer's channel
	// budget), instead of silently dropping a fifth enable.
	void syncMicCap();
};

} // namespace harpia
