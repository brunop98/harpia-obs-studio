#pragma once

#include <cstdint>

struct obs_source;
typedef struct obs_source obs_source_t;

namespace harpia {

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
	// `monitorIndex` selects which display (0 = primary). Returns false if the
	// capture plugin for this platform is unavailable.
	bool startCapture(int monitorIndex = 0);

	// Apply (or clear) a recording region via crop_filter. Safe to call while
	// capturing; pass an empty/disabled region to record the full display.
	void setRegion(const CaptureRegion &region);

	// Remove the source from channel 0 and release it.
	void stopCapture();

	obs_source_t *source() const { return source_; }

	// The default display-capture source id for the current platform.
	static const char *platformCaptureId();

private:
	obs_source_t *source_ = nullptr;
	obs_source_t *cropFilter_ = nullptr;
	CaptureRegion region_;
};

} // namespace harpia
