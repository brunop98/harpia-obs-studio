#pragma once

#include <memory>

namespace harpia {

// Cross-platform interface for querying how long the user has been idle (no
// keyboard or mouse input). Drives the "auto-pause while idle" feature: the UI
// polls currentIdleSeconds() on a timer and pauses/resumes recording when it
// crosses the preset's idle timeout.
//
// This is system-wide idle time, matching the requirement that recording
// pauses whenever the user stops interacting with the computer — regardless of
// which window has focus.
class IdleMonitor {
public:
	virtual ~IdleMonitor() = default;

	// Seconds since the last keyboard/mouse input, system-wide. Returns 0 if
	// idle time can't be determined on this platform.
	virtual double currentIdleSeconds() const = 0;

	// Construct the platform implementation (Windows: GetLastInputInfo;
	// Linux/macOS: stubs for now).
	static std::unique_ptr<IdleMonitor> create();
};

} // namespace harpia
