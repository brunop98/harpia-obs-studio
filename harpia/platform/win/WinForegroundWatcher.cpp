#include "platform/ForegroundWatcher.hpp"

#include <windows.h>

namespace harpia {

uint64_t ForegroundWatcher::foregroundProcessId()
{
	HWND hwnd = GetForegroundWindow();
	if (!hwnd)
		return 0;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	return (uint64_t)pid;
}

} // namespace harpia
