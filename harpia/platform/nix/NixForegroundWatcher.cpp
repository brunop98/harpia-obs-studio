#include "platform/ForegroundWatcher.hpp"

namespace harpia {

// Not implemented on Linux (X11/Wayland focus + process mapping varies widely).
// Returning 0 disables focus auto-pause here rather than pausing incorrectly.
uint64_t ForegroundWatcher::foregroundProcessId()
{
	return 0;
}


std::string ForegroundWatcher::foregroundExecutable()
{
	return {};
}

} // namespace harpia
