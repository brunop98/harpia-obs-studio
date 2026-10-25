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

quint64 foregroundProcessId();
QString foregroundApplicationId();

PermissionStatus screenCaptureStatus();
PermissionStatus cameraStatus();
PermissionStatus microphoneStatus();
// Requests run on the GUI thread. Completion is always queued to the GUI and
// is suppressed if the receiver was destroyed. Screen Recording's public query
// cannot distinguish "not requested" from "denied"; false reports Denied.
void requestScreenCapture(QObject *receiver, std::function<void(bool)> completion);
void requestCamera(QObject *receiver, std::function<void(bool)> completion);
void requestMicrophone(QObject *receiver, std::function<void(bool)> completion);

} // namespace harpia::mac_platform
