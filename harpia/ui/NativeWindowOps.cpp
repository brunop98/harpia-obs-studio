#include "NativeWindowOps.hpp"

#include <QWidget>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace harpia {

namespace native_window {

namespace {
Ops &testOps()
{
	static Ops ops;
	return ops;
}
} // namespace

void setOpsForTest(Ops ops)
{
	testOps() = std::move(ops);
}

std::optional<QRect> rect(const QWidget *w)
{
	if (testOps().rect)
		return testOps().rect(w);
#if defined(_WIN32)
	// internalWinId, not winId: winId() would create the native window, and
	// creating it early is one of the ways it ends up in the wrong place.
	const WId id = w ? w->internalWinId() : 0;
	RECT r;
	if (!id || !GetWindowRect(reinterpret_cast<HWND>(id), &r))
		return std::nullopt;
	// Our overlays are frameless: the window rectangle IS the client area.
	return QRect(int(r.left), int(r.top), int(r.right - r.left), int(r.bottom - r.top));
#else
	Q_UNUSED(w);
	return std::nullopt;
#endif
}

bool moveTo(QWidget *w, const QPoint &topLeft)
{
	if (testOps().moveTo)
		return testOps().moveTo(w, topLeft);
#if defined(_WIN32)
	const WId id = w ? w->internalWinId() : 0;
	return id && SetWindowPos(reinterpret_cast<HWND>(id), nullptr, topLeft.x(), topLeft.y(), 0, 0,
				  SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#else
	Q_UNUSED(w);
	Q_UNUSED(topLeft);
	return false;
#endif
}

} // namespace native_window

} // namespace harpia
