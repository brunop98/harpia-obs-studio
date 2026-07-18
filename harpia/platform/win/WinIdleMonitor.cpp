#include "platform/IdleMonitor.hpp"

#include <windows.h>

namespace harpia {

namespace {

// System-wide idle detection using GetLastInputInfo, which reports the tick of
// the last keyboard/mouse input across the whole session (not just our window).
class WinIdleMonitor : public IdleMonitor {
public:
	double currentIdleSeconds() const override
	{
		LASTINPUTINFO lii {};
		lii.cbSize = sizeof(lii);
		if (!GetLastInputInfo(&lii))
			return 0.0;

		// GetTickCount and dwTime are both 32-bit millisecond counters that
		// wrap together, so the unsigned subtraction stays correct across wrap.
		DWORD now = GetTickCount();
		DWORD idleMs = now - lii.dwTime;
		return (double)idleMs / 1000.0;
	}
};

} // namespace

std::unique_ptr<IdleMonitor> IdleMonitor::create()
{
	return std::make_unique<WinIdleMonitor>();
}

} // namespace harpia
