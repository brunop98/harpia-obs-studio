#include "MacPlatform.hpp"
#include "MacCoordinates.hpp"
#include "platform/WindowList.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <QScreen>
#include <QStringList>
#include <QThread>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <Security/Security.h>

namespace harpia::mac_platform {
namespace {
std::atomic<bool> screenCaptureKitAuthorized{false};
std::atomic<unsigned> screenAuthorizationGeneration{0};
std::mutex screenAuthorizationMutex;
QString lastScreenProbe = QStringLiteral("not requested in this process");
#if defined(HARPIA_TEST_SETTINGS)
std::function<bool()> testScreenPreflight;
std::function<void(std::function<void(bool, QString)>)> testScreenProbe;
#endif

bool quartzScreenPreflight()
{
#if defined(HARPIA_TEST_SETTINGS)
	if (testScreenPreflight)
		return testScreenPreflight();
#endif
	return CGPreflightScreenCaptureAccess();
}

void probeScreenCapture(std::function<void(bool, QString)> completion)
{
#if defined(HARPIA_TEST_SETTINGS)
	if (testScreenProbe) {
		testScreenProbe(std::move(completion));
		return;
	}
#endif
	// This is an explicit user request, never a startup/readiness poll. SCK
	// checks TCC for the running process and prompts when macOS permits it.
	[SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:NO
		completionHandler:^(SCShareableContent *content, NSError *error) {
			const bool granted = content != nil && error == nil;
			QString details;
			if (error)
				details = QStringLiteral("domain=%1 code=%2: %3")
					.arg(QString::fromUtf8(error.domain.UTF8String))
					.arg(qlonglong(error.code))
					.arg(QString::fromUtf8(error.localizedDescription.UTF8String));
			else if (!content)
				details = QStringLiteral("ScreenCaptureKit returned no shareable content");
			completion(granted, details);
		}];
}

QString string(NSString *value)
{
	return value ? QString::fromUtf8(value.UTF8String) : QString();
}

QString displayUuid(CGDirectDisplayID display)
{
	CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(display);
	if (!uuid)
		return {};
	CFStringRef text = CFUUIDCreateString(kCFAllocatorDefault, uuid);
	char utf8[128] = {};
	const bool copied = text && CFStringGetCString(text, utf8, sizeof(utf8), kCFStringEncodingUTF8);
	if (text)
		CFRelease(text);
	CFRelease(uuid);
	return copied ? QString::fromUtf8(utf8) : QString();
}

PermissionStatus avStatus(AVAuthorizationStatus status)
{
	switch (status) {
	case AVAuthorizationStatusAuthorized: return PermissionStatus::Granted;
	case AVAuthorizationStatusDenied: return PermissionStatus::Denied;
	case AVAuthorizationStatusRestricted: return PermissionStatus::Restricted;
	case AVAuthorizationStatusNotDetermined: return PermissionStatus::NotDetermined;
	}
	return PermissionStatus::Denied;
}

void complete(QPointer<QObject> receiver, std::function<void(bool)> callback, bool granted)
{
	if (auto *application = QCoreApplication::instance())
		QMetaObject::invokeMethod(application, [receiver, callback = std::move(callback), granted]() {
			if (receiver && callback)
				callback(granted);
		}, Qt::QueuedConnection);
}

void requestAv(AVMediaType mediaType, QObject *receiver, std::function<void(bool)> callback)
{
	QPointer<QObject> guard(receiver);
	if (auto *application = QCoreApplication::instance())
		QMetaObject::invokeMethod(application, [guard, mediaType, callback = std::move(callback)]() {
			if (!guard)
				return;
			[AVCaptureDevice requestAccessForMediaType:mediaType completionHandler:^(BOOL granted) {
				complete(guard, callback, granted == YES);
			}];
		}, Qt::QueuedConnection);
}
} // namespace

QVector<Camera> cameras()
{
	@autoreleasepool {
		NSArray<AVCaptureDeviceType> *types;
		if (@available(macOS 14.0, *)) {
			types = @[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal,
				AVCaptureDeviceTypeDeskViewCamera, AVCaptureDeviceTypeContinuityCamera];
		} else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
			types = @[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternalUnknown,
				AVCaptureDeviceTypeDeskViewCamera];
#pragma clang diagnostic pop
		}
		QVector<Camera> out;
		for (AVMediaType media in @[AVMediaTypeVideo, AVMediaTypeMuxed]) {
			AVCaptureDeviceDiscoverySession *discovery = [AVCaptureDeviceDiscoverySession
				discoverySessionWithDeviceTypes:types mediaType:media position:AVCaptureDevicePositionUnspecified];
			for (AVCaptureDevice *device in discovery.devices) {
				const QString deviceId = string(device.uniqueID);
				if (deviceId.isEmpty() || std::any_of(out.cbegin(), out.cend(),
					[&deviceId](const Camera &camera) { return camera.id == deviceId; }))
					continue;
				out.append({deviceId, string(device.localizedName)});
			}
		}
		return out;
	}
}

QVector<Display> displays()
{
	Q_ASSERT(!QCoreApplication::instance() ||
		 QThread::currentThread() == QCoreApplication::instance()->thread());
	@autoreleasepool {
		uint32_t count = 0;
		if (CGGetActiveDisplayList(0, nullptr, &count) != kCGErrorSuccess || count == 0)
			return {};
		std::vector<CGDirectDisplayID> ids(count);
		if (CGGetActiveDisplayList(count, ids.data(), &count) != kCGErrorSuccess)
			return {};
		QVector<Display> out;
		for (uint32_t i = 0; i < count; ++i) {
			Display entry;
			entry.id = ids[i];
			entry.uuid = displayUuid(ids[i]);
			if (entry.uuid.isEmpty())
				continue;
			const CGRect bounds = CGDisplayBounds(ids[i]);
			entry.logicalBounds = QRectF(bounds.origin.x, bounds.origin.y, bounds.size.width,
						     bounds.size.height).toAlignedRect();
			CGDisplayModeRef mode = CGDisplayCopyDisplayMode(ids[i]);
			entry.pixelSize = mode
				? QSize(int(CGDisplayModeGetPixelWidth(mode)), int(CGDisplayModeGetPixelHeight(mode)))
				: QSize(int(CGDisplayPixelsWide(ids[i])), int(CGDisplayPixelsHigh(ids[i])));
			if (mode)
				CGDisplayModeRelease(mode);
			for (NSScreen *screen in NSScreen.screens)
				if ([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == ids[i]) {
					entry.name = string(screen.localizedName);
					break;
				}
			if (entry.name.isEmpty())
				entry.name = QStringLiteral("Display %1").arg(ids[i]);
			out.append(entry);
		}
		const CGDirectDisplayID primary = CGMainDisplayID();
		std::sort(out.begin(), out.end(), [primary](const Display &a, const Display &b) {
			if ((a.id == primary) != (b.id == primary))
				return a.id == primary;
			return a.uuid < b.uuid;
		});
		return out;
	}
}

std::optional<Display> displayForScreen(const QScreen *screen)
{
	if (!screen)
		return std::nullopt;
	const QVector<Display> native = displays();
	QVector<Display> matching;
	for (const Display &display : native)
		if (display.logicalBounds == screen->geometry())
			matching.append(display);
	if (matching.size() == 1)
		return matching.front();
	for (const Display &display : matching)
		if (display.name == screen->name())
			return display;
	// Mirrored displays may share both coordinates and names. Their content is
	// identical; prefer the main display, as Qt exposes only one mirror screen.
	for (const Display &display : matching)
		if (display.id == CGMainDisplayID())
			return display;
	return std::nullopt;
}

QScreen *screenForDisplay(const QString &uuid)
{
	for (QScreen *screen : QGuiApplication::screens())
		if (const auto display = displayForScreen(screen); display && display->uuid == uuid)
			return screen;
	return nullptr;
}

std::optional<QRect> physicalRegionForWindow(quintptr windowId, const QString &screenName)
{
	const QScreen *match = nullptr;
	for (QScreen *screen : QGuiApplication::screens())
		if (screen->name() == screenName) {
			if (match)
				return std::nullopt; // same-model displays may have identical names
			match = screen;
		}
	return physicalRegionForWindow(windowId, match);
}

std::optional<QRect> physicalRegionForWindow(quintptr windowId, const QScreen *screen)
{
	const auto bounds = window_list::boundsOf(windowId);
	if (bounds)
		if (const auto display = displayForScreen(screen))
			return physicalRegion(*bounds, display->logicalBounds, display->pixelSize);
	return std::nullopt;
}

quint32 nativeWindowId(QWidget *widget)
{
	// Query only: winId() would create native windows for hidden dialogs while
	// scanning exclusions, producing more WinIdChange events and SCK refreshes.
	if (!widget || !widget->internalWinId())
		return 0;
	NSView *view = reinterpret_cast<NSView *>(widget->internalWinId());
	NSWindow *window = view.window;
	return window && window.windowNumber > 0 ? quint32(window.windowNumber) : 0;
}

quint64 foregroundProcessId()
{
	@autoreleasepool {
		return quint64(NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier);
	}
}

QString foregroundApplicationId()
{
	@autoreleasepool {
		return string(NSWorkspace.sharedWorkspace.frontmostApplication.bundleIdentifier);
	}
}

PermissionStatus screenCaptureStatus()
{
	return quartzScreenPreflight() || screenCaptureKitAuthorized.load()
		? PermissionStatus::Granted : PermissionStatus::Denied;
}

void invalidateScreenCaptureAuthorization()
{
	std::lock_guard<std::mutex> lock(screenAuthorizationMutex);
	++screenAuthorizationGeneration;
	screenCaptureKitAuthorized.store(false);
}

QString screenCaptureDiagnostics()
{
	QStringList lines;
	QString probeDetails;
	{
		std::lock_guard<std::mutex> lock(screenAuthorizationMutex);
		probeDetails = lastScreenProbe;
	}
	lines << QStringLiteral("Running executable: %1").arg(QCoreApplication::applicationFilePath())
	      << QStringLiteral("Bundle path: %1").arg(string(NSBundle.mainBundle.bundlePath))
	      << QStringLiteral("Bundle identifier: %1").arg(string(NSBundle.mainBundle.bundleIdentifier))
	      << QStringLiteral("Quartz preflight: %1").arg(quartzScreenPreflight() ? "granted" : "not granted")
	      << QStringLiteral("ScreenCaptureKit verified in this process: %1")
		     .arg(screenCaptureKitAuthorized.load() ? "yes" : "no")
	      << QStringLiteral("Last ScreenCaptureKit check: %1").arg(probeDetails);
	SecCodeRef code = nullptr;
	const OSStatus selfStatus = SecCodeCopySelf(kSecCSDefaultFlags, &code);
	if (selfStatus != errSecSuccess || !code) {
		lines << QStringLiteral("Could not read process signing identity: %1").arg(selfStatus);
		return lines.join(QLatin1Char('\n'));
	}
	lines << QStringLiteral("Running code signature validity status: %1 (0 = valid)")
		     .arg(SecCodeCheckValidity(code, kSecCSDefaultFlags, nullptr));
	CFDictionaryRef info = nullptr;
	const OSStatus infoStatus = SecCodeCopySigningInformation(code,
		kSecCSSigningInformation | kSecCSRequirementInformation, &info);
	if (infoStatus == errSecSuccess && info) {
		const auto identifier = (CFStringRef)CFDictionaryGetValue(info, kSecCodeInfoIdentifier);
		const auto team = (CFStringRef)CFDictionaryGetValue(info, kSecCodeInfoTeamIdentifier);
		const auto certificates = (CFArrayRef)CFDictionaryGetValue(info, kSecCodeInfoCertificates);
		const auto hash = (CFDataRef)CFDictionaryGetValue(info, kSecCodeInfoUnique);
		lines << QStringLiteral("Signing identifier: %1").arg(string((__bridge NSString *)identifier))
		      << QStringLiteral("Team identifier: %1").arg(team ? string((__bridge NSString *)team) : QStringLiteral("none"))
		      << QStringLiteral("Signing mode: %1").arg(certificates && CFArrayGetCount(certificates)
			     ? "certificate" : "ad-hoc (identity may change when rebuilt)");
		if (hash)
			lines << QStringLiteral("Code directory hash: %1").arg(QString::fromLatin1(
				QByteArray(reinterpret_cast<const char *>(CFDataGetBytePtr(hash)),
					   qsizetype(CFDataGetLength(hash))).toHex()));
		const auto requirement = (SecRequirementRef)CFDictionaryGetValue(info, kSecCodeInfoDesignatedRequirement);
		CFStringRef requirementText = nullptr;
		if (requirement && SecRequirementCopyString(requirement, kSecCSDefaultFlags, &requirementText) == errSecSuccess && requirementText) {
			lines << QStringLiteral("Designated requirement: %1").arg(string((__bridge NSString *)requirementText));
			CFRelease(requirementText);
		}
		CFRelease(info);
	} else {
		lines << QStringLiteral("Could not read signing details: %1").arg(infoStatus);
	}
	CFRelease(code);
	return lines.join(QLatin1Char('\n'));
}

#if defined(HARPIA_TEST_SETTINGS)
void setScreenCaptureTestBackend(std::function<bool()> preflight,
    std::function<void(std::function<void(bool, QString)>)> probe)
{
	testScreenPreflight = std::move(preflight);
	testScreenProbe = std::move(probe);
	invalidateScreenCaptureAuthorization();
}
#endif

PermissionStatus cameraStatus()
{
	return avStatus([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]);
}

PermissionStatus microphoneStatus()
{
	return avStatus([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]);
}

void requestScreenCapture(QObject *receiver, std::function<void(bool)> callback)
{
	QPointer<QObject> guard(receiver);
	const unsigned generation = screenAuthorizationGeneration.load();
	if (auto *application = QCoreApplication::instance())
		QMetaObject::invokeMethod(application, [guard, generation, callback = std::move(callback)]() {
			if (!guard)
				return;
			probeScreenCapture([guard, generation, callback](bool granted, const QString &details) {
				bool current;
				{
					std::lock_guard<std::mutex> lock(screenAuthorizationMutex);
					current = generation == screenAuthorizationGeneration.load();
					if (current) {
						screenCaptureKitAuthorized.store(granted);
						lastScreenProbe = granted ? QStringLiteral("granted") : details;
					}
				}
				qInfo().noquote() << QStringLiteral("[harpia] ScreenCaptureKit authorization: granted=%1 current=%2; %3")
					.arg(granted ? "yes" : "no", current ? "yes" : "no", details);
				complete(guard, callback, current && granted);
			});
		}, Qt::QueuedConnection);
}

void requestCamera(QObject *receiver, std::function<void(bool)> callback)
{
	requestAv(AVMediaTypeVideo, receiver, std::move(callback));
}

void requestMicrophone(QObject *receiver, std::function<void(bool)> callback)
{
	requestAv(AVMediaTypeAudio, receiver, std::move(callback));
}

} // namespace harpia::mac_platform
