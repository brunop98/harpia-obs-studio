#include "platform/ForegroundWatcher.hpp"

namespace harpia {

// Not implemented on macOS yet; returning 0 disables focus auto-pause here.
uint64_t ForegroundWatcher::foregroundProcessId()
{
	return 0;
}

} // namespace harpia
