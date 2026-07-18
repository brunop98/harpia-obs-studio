#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace harpia {

// Owns the lifetime of the libobs backend for the recorder: startup, plugin
// module loading, the video/audio graph, and clean shutdown. This mirrors the
// startup spine of the full OBS frontend (OBSApp::OBSInit /
// OBSBasic::ResetVideo/ResetAudio) but keeps only what a recorder needs.
//
// Usage:
//   ObsContext obs;
//   if (!obs.startup()) { /* fatal */ }
//   obs.resetVideo(1920, 1080, 30);
//   obs.resetAudio();
//   obs.loadModules();
//   ... record ...
//   obs.shutdown();   // also called by the destructor
class ObsContext {
public:
	ObsContext() = default;
	~ObsContext();

	ObsContext(const ObsContext &) = delete;
	ObsContext &operator=(const ObsContext &) = delete;

	// Initialize libobs (obs_startup) with the per-user module config dir.
	bool startup();

	// Register plugin search paths and load every module
	// (obs_load_all_modules2 + obs_post_load_modules). Call after startup and
	// after resetVideo/resetAudio.
	void loadModules();

	// (Re)configure the video graph. `baseWidth/baseHeight` is the capture
	// canvas; `outWidth/outHeight` is the encoded size (0 => same as base).
	// Returns an OBS_VIDEO_* result code; OBS_VIDEO_SUCCESS (0) on success.
	int resetVideo(uint32_t baseWidth, uint32_t baseHeight, int fpsNum, uint32_t outWidth = 0,
		       uint32_t outHeight = 0);

	// Configure the audio graph (48kHz stereo). Returns true on success.
	bool resetAudio();

	// After loadModules(): human-readable names of REQUIRED backend components
	// (screen capture source, H.264 encoder, AAC encoder, ffmpeg muxer output)
	// that failed to register — i.e. missing/blocked plugin libraries. Empty when
	// everything needed is present. Lets the app report a clear message instead
	// of failing cryptically at record time.
	std::vector<std::string> missingDependencies() const;

	// Tear down libobs. Safe to call multiple times.
	void shutdown();

	bool initialized() const { return initialized_; }

private:
	// Resolve the DLL/so name of the graphics module to use for this platform
	// (D3D11 on Windows, OpenGL elsewhere).
	static const char *renderModule();

	// Add platform plugin search paths (built-in bundle + env overrides).
	void addModulePaths();

	bool initialized_ = false;
	bool modulesLoaded_ = false;
};

} // namespace harpia
