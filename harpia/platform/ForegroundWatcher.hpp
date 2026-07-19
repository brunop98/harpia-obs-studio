#pragma once

#include <cstdint>

namespace harpia {

// Reports which process owns the current foreground window, so recording can be
// auto-paused when the target application (and its child windows / modal dialogs,
// which share the same process) is no longer focused.
class ForegroundWatcher {
public:
	// Process id of the current foreground window's owner, or 0 if unknown /
	// unsupported on this platform. Windows-only for now; other platforms return
	// 0 (focus auto-pause is effectively disabled there).
	static uint64_t foregroundProcessId();
};

} // namespace harpia
