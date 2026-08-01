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

// One selectable top-level window, as exposed by the window-capture source's
// "window" property. `value` is the opaque settings string to apply.
struct WindowOption {
	std::string name;  // human-facing title
	std::string value; // "window" setting value
};

// Optional sub-rectangle of the captured display to record. When active, a
// crop_filter is attached to the capture source so only this region is encoded.
struct CaptureRegion {
	bool enabled = false;
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;

	bool operator==(const CaptureRegion &o) const
	{
		return enabled == o.enabled && x == o.x && y == o.y && width == o.width &&
		       height == o.height;
	}
	bool operator!=(const CaptureRegion &o) const { return !(*this == o); }
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

	// Capture a single application window instead of a monitor. `windowValue` is
	// a value from enumerateWindows(). Returns false if window capture isn't
	// available on this platform or the source fails to create.
	bool startWindowCapture(const std::string &windowValue, bool captureCursor = true);

	// Current captured content size (e.g. the window's size). False if unknown.
	bool sourceSize(uint32_t &w, uint32_t &h) const;

	// Apply (or clear) a recording region via crop_filter. Safe to call while
	// capturing; pass an empty/disabled region to record the full display.
	void setRegion(const CaptureRegion &region);

	// Remove the source from channel 0 and release it.
	void stopCapture();

	obs_source_t *source() const { return source_; }

	// The default display-capture source id for the current platform.
	static const char *platformCaptureId();

	// The window-capture source id for the current platform, or nullptr if window
	// capture isn't wired for it.
	static const char *platformWindowCaptureId();

	// Enumerate the displays the platform capture source can target. Requires
	// modules to be loaded. Empty if the source exposes no monitor list.
	static std::vector<MonitorOption> enumerateMonitors();

	// Enumerate capturable top-level windows. Empty if unsupported.
	static std::vector<WindowOption> enumerateWindows();

private:
	obs_source_t *source_ = nullptr;
	obs_source_t *cropFilter_ = nullptr;
	CaptureRegion region_;
	// What the live source was built with, so an identical startCapture() call
	// can keep it instead of rebuilding. -1 = no live source.
	int monitorIndex_ = -1;
	bool captureCursor_ = true;
};

} // namespace harpia
