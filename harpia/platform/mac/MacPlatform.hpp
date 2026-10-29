#pragma once

#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

class QObject;
class QScreen;
class QWidget;

namespace harpia::mac_platform {

enum class PermissionStatus { NotDetermined, Granted, Denied, Restricted };

struct Display {
	quint32 id = 0;
	QString uuid;
	QString name;
	// Quartz global coordinates use logical points with the main display's
	// top-left as origin. Native capture pixels are local to each display.
	QRect logicalBounds;
	QSize pixelSize;
};

struct Camera {
	QString id;
	QString name;
};
// Device metadata only: no AVCaptureSession/Input and no permission request.
// This lets a first-run user choose a camera before explicitly granting access.
QVector<Camera> cameras();

// GUI-thread display snapshots; no OBS source or permission prompt is created.
// The main display comes first. Other displays sort by stable display UUID.
QVector<Display> displays();
QScreen *screenForDisplay(const QString &uuid);
std::optional<Display> displayForScreen(const QScreen *screen);

// WindowList's Mac implementation uses logical Quartz desktop coordinates for
// picking. Convert a chosen window to this display's native, even pixel crop.
std::optional<QRect> physicalRegionForWindow(quintptr windowId, const QString &screenName);
std::optional<QRect> physicalRegionForWindow(quintptr windowId, const QScreen *screen);
quint32 nativeWindowId(QWidget *widget);

// AppKit input passthrough for top-level overlay windows. A shaped Qt window
// mask is not a reliable cross-application input region on macOS; during a
// recording the region frame uses this to stay visible without intercepting
// clicks anywhere inside its rectangular native window.
bool setWindowInputPassthrough(QWidget *widget, bool passthrough);
bool windowInputPassthrough(const QWidget *widget);

quint64 foregroundProcessId();
QString foregroundApplicationId();

PermissionStatus screenCaptureStatus();
// Forget a prior successful ScreenCaptureKit check after a stream fails or a
// user explicitly retries authorization. This does not change macOS settings.
void invalidateScreenCaptureAuthorization();
QString screenCaptureDiagnostics();
PermissionStatus cameraStatus();
PermissionStatus microphoneStatus();
// Requests run on the GUI thread. Completion is always queued to the GUI and
// is suppressed if the receiver was destroyed. The read-only status uses Quartz
// plus successful SCK authorization observed in this process. An explicit screen
// request uses ScreenCaptureKit, so a false Quartz preflight is not a final denial.
void requestScreenCapture(QObject *receiver, std::function<void(bool)> completion);
void requestCamera(QObject *receiver, std::function<void(bool)> completion);
void requestMicrophone(QObject *receiver, std::function<void(bool)> completion);

#if defined(HARPIA_TEST_SETTINGS)
// Native endpoints only: tests exercise the real asynchronous request/status
// path without querying the CI user's privacy settings or raising OS prompts.
void setScreenCaptureTestBackend(std::function<bool()> preflight,
    std::function<void(std::function<void(bool, QString)>)> probe);
#endif

} // namespace harpia::mac_platform
