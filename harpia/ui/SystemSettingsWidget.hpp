#pragma once

// The "System" page of the settings: things about this computer rather than a
// recording preset.
//
//   * The graphics card on laptops with two (Windows; see
//     platform/OsIntegration.hpp for why it is a choice and not a default).
//     Windows reads it when Harpia starts, so the page says a restart applies it.
//   * Unity control: whether the Unity Play Mode Recorder may start recordings,
//     and the local port it talks to (core/RemoteSettings.hpp). Applied when
//     the settings window closes.
//
// Saves as you go, like the Downloads page.

#include "platform/OsIntegration.hpp"

#include <QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;

namespace harpia {

class SystemSettingsWidget : public QWidget {
public:
	using GetGpu = std::function<os_integration::GpuPreference()>;
	using SetGpu = std::function<bool(os_integration::GpuPreference)>;

	// With no get/set given, the graphics row reads and writes the real
	// preference -- and only appears where the platform has one. A test passes
	// its own to see the row anywhere.
	explicit SystemSettingsWidget(QWidget *parent = nullptr, GetGpu get = {}, SetGpu set = {});

	QComboBox *gpuCombo() const { return gpuCombo_; } // null when the row is not shown
	QLabel *statusLabel() const { return status_; }
	QCheckBox *remoteCheck() const { return remoteCheck_; }
	QSpinBox *remotePort() const { return remotePort_; }

private:
	void saveRemote();

	QComboBox *gpuCombo_ = nullptr;
	QLabel *status_ = nullptr;
	QCheckBox *remoteCheck_ = nullptr;
	QSpinBox *remotePort_ = nullptr;
	SetGpu set_;
	os_integration::GpuPreference saved_ = os_integration::GpuPreference::Automatic;
};

} // namespace harpia
