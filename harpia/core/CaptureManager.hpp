#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct obs_source;
typedef struct obs_source obs_source_t;

namespace harpia {

// One selectable display, as exposed by the platform capture source's monitor
// property. `key` is the settings field to set (e.g. "monitor_id" on Windows,
// "screen" on X11); the value is either a string or an int depending on the
// source.
struct MonitorOption {
	std::string name;      // human-facing label from the source
	std::string key;       // settings key to apply
	bool isString = true;  // value kind
	std::string strValue;  // when isString
	long long intValue = 0; // when !isString
};

// Optional sub-rectangle of the captured display to record. When active, a
// crop_filter is attached to the capture source so only this region is encoded.
struct CaptureRegion {
	bool enabled = false;
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

// Creates and owns the screen-capture source and wires it into libobs' output
// channel 0 (the video the encoders see). For the MVP this captures a full
// monitor; a crop_filter hook is in place for the region-selection feature.
//
// The capture source id is platform-specific:
//   Windows: "monitor_capture"   Linux(X11): "xshm_input"
//   macOS:   "screen_capture"    Linux(Wayland): "pipewire-screen-capture-source"
class CaptureManager {
public:
	CaptureManager() = default;
	~CaptureManager();

	CaptureManager(const CaptureManager &) = delete;
	CaptureManager &operator=(const CaptureManager &) = delete;

	// Create the display capture source and bind it to output channel 0.
	// `monitorIndex` selects which display (0 = primary); `captureCursor`
	// controls whether the OS cursor is drawn into the capture. Returns false if
	// the capture plugin for this platform is unavailable.
	bool startCapture(int monitorIndex = 0, bool captureCursor = true);

	// Apply (or clear) a recording region via crop_filter. Safe to call while
	// capturing; pass an empty/disabled region to record the full display.
	void setRegion(const CaptureRegion &region);

	// Remove the source from channel 0 and release it.
	void stopCapture();

	obs_source_t *source() const { return source_; }

	// The default display-capture source id for the current platform.
	static const char *platformCaptureId();

	// Enumerate the displays the platform capture source can target. Requires
	// modules to be loaded. Empty if the source exposes no monitor list.
	static std::vector<MonitorOption> enumerateMonitors();

private:
	obs_source_t *source_ = nullptr;
	obs_source_t *cropFilter_ = nullptr;
	CaptureRegion region_;
};

} // namespace harpia
