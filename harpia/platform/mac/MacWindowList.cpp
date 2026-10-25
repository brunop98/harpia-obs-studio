#include "platform/WindowList.hpp"
#include "MacPlatform.hpp"

#include <QByteArray>
#include <QGuiApplication>
#include <QScreen>

#include <CoreGraphics/CoreGraphics.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>

// Quartz exposes windows in desktop points. Keep picking/hit-testing in that
// shared space; MacPlatform::physicalRegionForWindow converts the selected
// window into a display-local pixel crop after choosing the target display.

namespace harpia {

namespace window_list {

namespace {
QString text(CFStringRef value)
{
	if (!value)
		return {};
	const CFIndex capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
	QByteArray utf8(int(capacity), '\0');
	return CFStringGetCString(value, utf8.data(), capacity, kCFStringEncodingUTF8)
		? QString::fromUtf8(utf8.constData()) : QString();
}

int64_t integer(CFDictionaryRef info, const void *key)
{
	int64_t result = 0;
	if (CFNumberRef value = static_cast<CFNumberRef>(CFDictionaryGetValue(info, key)))
		CFNumberGetValue(value, kCFNumberSInt64Type, &result);
	return result;
}

std::optional<DesktopWindow> window(CFDictionaryRef info)
{
	if (integer(info, kCGWindowLayer) != 0)
		return std::nullopt;
	CFBooleanRef onScreen = static_cast<CFBooleanRef>(CFDictionaryGetValue(info, kCGWindowIsOnscreen));
	if (onScreen && !CFBooleanGetValue(onScreen))
		return std::nullopt;
	double alpha = 1.0;
	if (CFNumberRef value = static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCGWindowAlpha)))
		CFNumberGetValue(value, kCFNumberDoubleType, &alpha);
	if (alpha <= 0.0)
		return std::nullopt;
	CGRect bounds = CGRectZero;
	CFDictionaryRef rectangle = static_cast<CFDictionaryRef>(CFDictionaryGetValue(info, kCGWindowBounds));
	if (!rectangle || !CGRectMakeWithDictionaryRepresentation(rectangle, &bounds) ||
	    bounds.size.width < 8.0 || bounds.size.height < 8.0)
		return std::nullopt;
	DesktopWindow result;
	const int64_t nativeId = integer(info, kCGWindowNumber);
	if (nativeId <= 0 || nativeId > UINT32_MAX)
		return std::nullopt;
	result.id = quintptr(nativeId);
	result.bounds = QRectF(bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height).toAlignedRect();
	result.title = text(static_cast<CFStringRef>(CFDictionaryGetValue(info, kCGWindowName)));
	if (result.title.isEmpty())
		result.title = text(static_cast<CFStringRef>(CFDictionaryGetValue(info, kCGWindowOwnerName)));
	result.ours = integer(info, kCGWindowOwnerPID) == int(getpid());
	return result.id ? std::optional<DesktopWindow>(result) : std::nullopt;
}
} // namespace

bool available()
{
	return mac_platform::screenCaptureStatus() == mac_platform::PermissionStatus::Granted;
}

QVector<DesktopWindow> windows()
{
	QVector<DesktopWindow> result;
	CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
						   kCGNullWindowID);
	if (!list)
		return result;
	for (CFIndex i = 0; i < CFArrayGetCount(list); ++i)
		if (const auto entry = window(static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i))))
			result.append(*entry);
	CFRelease(list);
	return result;
}

std::optional<QRect> boundsOf(quintptr id)
{
	if (!id || id > quintptr(UINT32_MAX))
		return std::nullopt;
	CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, CGWindowID(id));
	if (!list)
		return std::nullopt;
	std::optional<QRect> result;
	if (CFArrayGetCount(list) > 0)
		if (const auto entry = window(static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, 0))))
			result = entry->bounds;
	CFRelease(list);
	return result;
}

QRect monitorRect(const QString &screenName)
{
	for (QScreen *screen : QGuiApplication::screens())
		if (screen->name() == screenName)
			return screen->geometry();
	return {};
}

std::optional<QPoint> cursorPos()
{
	CGEventRef event = CGEventCreate(nullptr);
	if (!event)
		return std::nullopt;
	const CGPoint point = CGEventGetLocation(event);
	CFRelease(event);
	return QPoint(int(std::lround(point.x)), int(std::lround(point.y)));
}

} // namespace window_list

} // namespace harpia
