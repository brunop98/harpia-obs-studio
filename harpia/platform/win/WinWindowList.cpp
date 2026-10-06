#include "platform/WindowList.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After windows.h, which it needs.
#include <dwmapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "dwmapi.lib")
#endif

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <string>

namespace harpia {

namespace window_list {

namespace {

// Hidden by DWM though "visible" to USER: a Store app suspended in the
// background, a window on another virtual desktop. Nothing you can see.
bool cloaked(HWND h)
{
	DWORD c = 0;
	return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof(c))) && c != 0;
}

// The window as drawn: without the invisible resize border and the shadow.
bool visibleBounds(HWND h, RECT &r)
{
	if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
		return true;
	return GetWindowRect(h, &r) != 0; // composition off: the plain rectangle
}

QRect toQRect(const RECT &r)
{
	return QRect(int(r.left), int(r.top), int(r.right - r.left), int(r.bottom - r.top));
}

bool listable(HWND h)
{
	if (!IsWindowVisible(h) || IsIconic(h) || cloaked(h))
		return false;
	const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
	// Tool windows (palettes, tooltips, notification toasts) unless they ask
	// for a taskbar button; click-through overlays (ours, game overlays).
	if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW))
		return false;
	if (ex & WS_EX_TRANSPARENT)
		return false;
	wchar_t cls[64] = {0};
	GetClassNameW(h, cls, 64);
	for (const wchar_t *shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"})
		if (std::wcscmp(cls, shell) == 0)
			return false; // the desktop and the taskbar
	return true;
}

struct EnumState {
	QVector<DesktopWindow> out;
	DWORD self = 0;
};

BOOL CALLBACK enumWindow(HWND h, LPARAM lp)
{
	auto *st = reinterpret_cast<EnumState *>(lp);
	if (!listable(h))
		return TRUE;
	RECT r;
	if (!visibleBounds(h, r) || r.right - r.left < 8 || r.bottom - r.top < 8)
		return TRUE;
	DesktopWindow w;
	w.id = reinterpret_cast<quintptr>(h);
	w.bounds = toQRect(r);
	wchar_t title[256] = {0};
	const int n = GetWindowTextW(h, title, 256);
	w.title = QString::fromWCharArray(title, std::max(0, n));
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	w.ours = pid == st->self;
	st->out.push_back(w);
	return TRUE;
}

struct MonitorSearch {
	std::wstring name;
	QRect found;
};

BOOL CALLBACK enumMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp)
{
	auto *st = reinterpret_cast<MonitorSearch *>(lp);
	MONITORINFOEXW mi;
	std::memset(&mi, 0, sizeof(mi));
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoW(mon, &mi) && st->name == mi.szDevice) {
		st->found = toQRect(mi.rcMonitor);
		return FALSE; // found it
	}
	return TRUE;
}

} // namespace

bool available()
{
	return true;
}

QVector<DesktopWindow> windows()
{
	// EnumWindows walks the top-level windows in z-order, topmost first --
	// exactly the order "the first one under the pointer" needs.
	EnumState st;
	st.self = GetCurrentProcessId();
	EnumWindows(enumWindow, reinterpret_cast<LPARAM>(&st));
	return st.out;
}

std::optional<QRect> boundsOf(quintptr id)
{
	HWND h = reinterpret_cast<HWND>(id);
	if (!h || !IsWindow(h) || !IsWindowVisible(h) || IsIconic(h) || cloaked(h))
		return std::nullopt;
	RECT r;
	if (!visibleBounds(h, r))
		return std::nullopt;
	return toQRect(r);
}

QRect monitorRect(const QString &screenName)
{
	MonitorSearch st;
	st.name = screenName.toStdWString();
	EnumDisplayMonitors(nullptr, nullptr, enumMonitor, reinterpret_cast<LPARAM>(&st));
	return st.found;
}

std::optional<QPoint> cursorPos()
{
	POINT p;
	if (!GetCursorPos(&p))
		return std::nullopt;
	return QPoint(int(p.x), int(p.y));
}

} // namespace window_list

} // namespace harpia
