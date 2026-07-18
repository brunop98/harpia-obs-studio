#pragma once

#include <QWidget>
#include <string>
#include <vector>

class QCheckBox;
class QProgressBar;
class QVBoxLayout;

namespace harpia {

class AudioManager;

// Compact audio panel: a "Record PC Audio" toggle plus one "Record Mic" toggle
// per detected input device, each with a live level bar so the user can confirm
// audio is actually coming through. Toggling a control immediately enables the
// corresponding live capture (via AudioManager) and emits changed() so the
// owner can persist the selection to the active preset.
class AudioPanel : public QWidget {
	Q_OBJECT
public:
	explicit AudioPanel(AudioManager &audio, QWidget *parent = nullptr);

	// Set the toggles (and drive AudioManager) from stored preset values.
	void load(bool desktopOn, const std::vector<std::string> &micIds);

	bool desktopOn() const;
	std::vector<std::string> enabledMicIds() const;

signals:
	void changed();

public slots:
	void updateMeters(); // poll AudioManager peak levels into the bars

private:
	static int dbToPercent(float db);

	AudioManager &audio_;
	QCheckBox *pcCheck_ = nullptr;
	QProgressBar *pcMeter_ = nullptr;

	struct MicRow {
		std::string id;
		QCheckBox *check = nullptr;
		QProgressBar *meter = nullptr;
	};
	std::vector<MicRow> micRows_;
};

} // namespace harpia
