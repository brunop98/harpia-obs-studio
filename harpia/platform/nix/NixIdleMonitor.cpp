#include "platform/IdleMonitor.hpp"

namespace harpia {

namespace {

// Stub for Linux/BSD. A full implementation would query the X11
// ScreenSaver extension (XScreenSaverQueryInfo -> idle) on X11, or use
// org.freedesktop.ScreenSaver / libinput on Wayland. Returning 0 disables the
// idle auto-pause on this platform for now.
class NixIdleMonitor : public IdleMonitor {
public:
	double currentIdleSeconds() const override { return 0.0; }
};

} // namespace

std::unique_ptr<IdleMonitor> IdleMonitor::create()
{
	return std::make_unique<NixIdleMonitor>();
}

} // namespace harpia
