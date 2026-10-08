// platform/OsIntegration.hpp off Windows: no taskbar id to set, no power
// throttling to opt out of, and no per-app graphics preference to keep.
#include "platform/OsIntegration.hpp"

namespace harpia::os_integration {

QString applyAtStartup()
{
	return {};
}

bool gpuPreferenceSupported()
{
	return false;
}

GpuPreference gpuPreference()
{
	return GpuPreference::Automatic;
}

bool setGpuPreference(GpuPreference)
{
	return false;
}

} // namespace harpia::os_integration
