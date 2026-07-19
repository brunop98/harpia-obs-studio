#pragma once

#include <cstdint>
#include <string>

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

	// Executable file name (e.g. "claude.exe", UTF-8, no path) of the foreground
	// window's process, or empty if unknown/unsupported. Focus auto-pause matches
	// the target by executable rather than pid because multi-process applications
	// (browsers, Electron apps) own several pids that all belong to "the app".
	static std::string foregroundExecutable();
};

} // namespace harpia
