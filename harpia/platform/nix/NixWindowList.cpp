#include "platform/WindowList.hpp"

// Not wired on this platform yet: no window list, so "Pick a window…" hides
// itself (window_list::available() is false) and nothing else changes.

namespace harpia {

namespace window_list {

bool available()
{
	return false;
}

QVector<DesktopWindow> windows()
{
	return {};
}

std::optional<QRect> boundsOf(quintptr)
{
	return std::nullopt;
}

QRect monitorRect(const QString &)
{
	return QRect();
}

std::optional<QPoint> cursorPos()
{
	return std::nullopt;
}

} // namespace window_list

} // namespace harpia
