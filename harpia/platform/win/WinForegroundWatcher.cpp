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

std::string ForegroundWatcher::foregroundExecutable()
{
	HWND hwnd = GetForegroundWindow();
	if (!hwnd)
		return {};
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (!pid)
		return {};

	HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc)
		return {};

	std::string out;
	wchar_t path[MAX_PATH];
	DWORD len = MAX_PATH;
	if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
		// Keep just the file name, converted to UTF-8.
		const wchar_t *base = wcsrchr(path, L'\\');
		base = base ? base + 1 : path;
		char utf8[MAX_PATH * 3];
		const int n = WideCharToMultiByte(CP_UTF8, 0, base, -1, utf8, sizeof(utf8), nullptr, nullptr);
		if (n > 0)
			out = utf8;
	}
	CloseHandle(proc);
	return out;
}

} // namespace harpia
