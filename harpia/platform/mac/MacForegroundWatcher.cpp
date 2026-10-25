#include "platform/ForegroundWatcher.hpp"
#include "MacPlatform.hpp"

namespace harpia {

uint64_t ForegroundWatcher::foregroundProcessId()
{
	return mac_platform::foregroundProcessId();
}


std::string ForegroundWatcher::foregroundExecutable()
{
	// Bundle identifiers identify an application across processes and launches,
	// unlike executable leaf names. The Mac app chooser uses the same identity.
	return mac_platform::foregroundApplicationId().toStdString();
}

} // namespace harpia
