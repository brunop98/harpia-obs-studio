#include "platform/IdleMonitor.hpp"

namespace harpia {

namespace {

// Stub for macOS. A full implementation would use
// CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState,
// kCGAnyInputEventType). Returning 0 disables idle auto-pause for now.
class MacIdleMonitor : public IdleMonitor {
public:
	double currentIdleSeconds() const override { return 0.0; }
};

} // namespace

std::unique_ptr<IdleMonitor> IdleMonitor::create()
{
	return std::make_unique<MacIdleMonitor>();
}

} // namespace harpia
