#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class QObject;

namespace harpia::phone_sharing::native {

// Queries the platform service, rather than nearby devices. Call on the GUI
// thread; this does not start discovery or present a sharing window.
bool airDropSupported();

// Opens macOS's AirDrop recipient picker for readable local files. A true
// return means the picker was requested, not that the files were delivered.
// Immediate validation/presentation failures return false and set `error`.
// Terminal native results are queued to `receiver`: empty means the native
// service reported success; otherwise the message describes cancellation or
// failure. Destroying the receiver suppresses completion and releases this
// request's native delegate. Only one AirDrop request runs at a time.
// The receiver and caller must both belong to the GUI/main thread.
bool sendWithAirDrop(const QStringList &absoluteFiles, QObject *receiver,
		     std::function<void(const QString &error)> completion, QString *error);

} // namespace harpia::phone_sharing::native
