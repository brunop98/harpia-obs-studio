#pragma once

#include <QString>
#include <QStringList>

namespace harpia::local_send {

struct Launch {
	QString program;
	QStringList arguments;
	QString workingDirectory;
};

// Discovery is deliberately on demand. A configured path that no longer
// exists returns empty so the user can repair it in the sharing dialog.
QString findApplication(const QString &configuredPath = {});

// Prepare a GUI handoff, keeping each file as its own argument. LocalSend's
// desktop GUI accepts absolute file paths (also in v1.17); the terminal-only
// localsend-cli is not used. macOS receives open-document events via `open`.
bool makeLaunch(const QString &application, const QStringList &files, Launch *out, QString *error);

// True means the application handoff was requested. The user still chooses
// their phone and accepts the transfer in LocalSend.
bool sendFiles(const QString &application, const QStringList &files, QString *error);

} // namespace harpia::local_send
