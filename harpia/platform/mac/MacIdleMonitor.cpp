#include "platform/IdleMonitor.hpp"

#include <CoreGraphics/CoreGraphics.h>

namespace harpia {

namespace {

// System-wide idle time on macOS via CoreGraphics. Reports seconds since the
// last input event of any kind across the whole session.
class MacIdleMonitor : public IdleMonitor {
public:
	double currentIdleSeconds() const override
	{
		return CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState,
							      kCGAnyInputEventType);
	}
};

} // namespace

std::unique_ptr<IdleMonitor> IdleMonitor::create()
{
	return std::make_unique<MacIdleMonitor>();
}

} // namespace harpia
