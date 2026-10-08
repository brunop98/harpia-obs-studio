#pragma once

// The "System" page of the settings: things about this computer rather than a
// recording preset. For now the graphics card on laptops with two (see
// platform/OsIntegration.hpp for why it is a choice and not a default).
//
// Saves as you pick, like the Downloads page; Windows reads the choice when
// Harpia starts, so the page says a restart applies it.

#include "platform/OsIntegration.hpp"

#include <QWidget>

#include <functional>

class QComboBox;
class QLabel;

namespace harpia {

class SystemSettingsWidget : public QWidget {
public:
	using GetGpu = std::function<os_integration::GpuPreference()>;
	using SetGpu = std::function<bool(os_integration::GpuPreference)>;

	// The defaults read and write the real preference; a test passes its own.
	explicit SystemSettingsWidget(QWidget *parent = nullptr, GetGpu get = os_integration::gpuPreference,
				      SetGpu set = os_integration::setGpuPreference);

	QComboBox *gpuCombo() const { return gpuCombo_; }
	QLabel *statusLabel() const { return status_; }

private:
	QComboBox *gpuCombo_ = nullptr;
	QLabel *status_ = nullptr;
	SetGpu set_;
	os_integration::GpuPreference saved_ = os_integration::GpuPreference::Automatic;
};

} // namespace harpia
