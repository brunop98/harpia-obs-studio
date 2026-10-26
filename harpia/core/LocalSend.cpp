#include "LocalSend.hpp"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>

namespace harpia::local_send {
namespace {

bool fail(QString *error, const QString &message)
{
	if (error)
		*error = message;
	return false;
}

#if defined(Q_OS_MACOS)
QString appBundle(const QString &path)
{
	const QFileInfo file(path);
	if (file.isDir() && file.fileName().endsWith(QStringLiteral(".app"), Qt::CaseInsensitive)) {
		const QDir bundle(file.absoluteFilePath());
		if (!QFileInfo::exists(bundle.filePath(QStringLiteral("Contents/Info.plist"))))
			return {};
		const QDir binaries(bundle.filePath(QStringLiteral("Contents/MacOS")));
		for (const QFileInfo &binary : binaries.entryInfoList(QDir::Files | QDir::Executable))
			if (binary.isExecutable())
				return file.absoluteFilePath();
		return {};
	}
	// A PATH entry or file picker can point inside a bundle. Send documents to
	// the bundle, since LocalSend's Mac GUI handles open-file events, not argv.
	QDir directory = file.dir();
	if (file.isFile() && directory.dirName() == QStringLiteral("MacOS")) {
		directory.cdUp();
		if (directory.dirName() == QStringLiteral("Contents")) {
			directory.cdUp();
			return appBundle(directory.absolutePath());
		}
	}
	return {};
}
#endif

QString usableApplication(const QString &path)
{
	if (path.isEmpty() || path.contains(QChar::Null))
		return {};
#if defined(Q_OS_MACOS)
	return appBundle(path);
#else
	const QFileInfo file(path);
	if (!file.exists() || !file.isFile() || !file.isExecutable())
		return {};
#if defined(Q_OS_WIN)
	// QProcess must start the application itself, never a command interpreter.
	if (file.suffix().compare(QStringLiteral("exe"), Qt::CaseInsensitive) != 0)
		return {};
#endif
	const QString name = file.completeBaseName().toLower();
	if (name == QStringLiteral("localsend-cli") || name == QStringLiteral("localsend_cli"))
		return {};
	return file.absoluteFilePath();
#endif
}

} // namespace

QString findApplication(const QString &configuredPath)
{
	if (!configuredPath.isEmpty())
		return usableApplication(configuredPath);

	QStringList candidates;
#if defined(Q_OS_MACOS)
	candidates << QStringLiteral("/Applications/LocalSend.app")
		   << QDir::home().filePath(QStringLiteral("Applications/LocalSend.app"));
#endif
	for (const QString &name : {QStringLiteral("localsend_app"), QStringLiteral("localsend"),
				    QStringLiteral("LocalSend")}) {
		const QString executable = QStandardPaths::findExecutable(name);
		if (!executable.isEmpty())
			candidates.append(executable);
	}
#if defined(Q_OS_WIN)
	for (const char *variable : {"ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"}) {
		const QString folder = qEnvironmentVariable(variable);
		if (!folder.isEmpty())
			candidates.append(QDir(folder).filePath(QStringLiteral("LocalSend/localsend_app.exe")));
	}
	const QString local = qEnvironmentVariable("LOCALAPPDATA");
	if (!local.isEmpty()) {
		candidates.append(QDir(local).filePath(QStringLiteral("Programs/LocalSend/localsend_app.exe")));
		candidates.append(QDir(local).filePath(QStringLiteral("LocalSend/localsend_app.exe")));
	}
	candidates.append(QDir::home().filePath(QStringLiteral("scoop/apps/localsend/current/localsend_app.exe")));
#endif
	for (const QString &candidate : candidates) {
		const QString application = usableApplication(candidate);
		if (!application.isEmpty())
			return application;
	}
	return {};
}

bool makeLaunch(const QString &application, const QStringList &files, Launch *out, QString *error)
{
	if (out)
		*out = {};
	if (error)
		error->clear();
	if (!out)
		return fail(error, QStringLiteral("Could not prepare the LocalSend handoff."));
	const QString executable = usableApplication(application);
	if (executable.isEmpty())
		return fail(error, QStringLiteral("LocalSend was not found. Install LocalSend or choose its application."));
	if (files.isEmpty())
		return fail(error, QStringLiteral("Choose at least one file to send to your phone."));

	QStringList absoluteFiles;
	QSet<QString> seen;
	for (const QString &path : files) {
		if (path.isEmpty() || path.contains(QChar::Null))
			return fail(error, QStringLiteral("A selected file has an invalid path."));
		const QFileInfo file(path);
		if (!file.exists() || !file.isFile() || !file.isReadable())
			return fail(error, QStringLiteral("This file is missing or cannot be read:\n%1").arg(path));
		const QString absolute = file.absoluteFilePath();
		if (!seen.contains(absolute)) {
			seen.insert(absolute);
			absoluteFiles.append(absolute);
		}
	}
	Launch launch;
#if defined(Q_OS_MACOS)
	launch.program = QStringLiteral("/usr/bin/open");
	launch.arguments = {QStringLiteral("-a"), executable, QStringLiteral("--")};
	launch.arguments.append(absoluteFiles);
#else
	launch.program = executable;
	launch.arguments = absoluteFiles;
#endif
	launch.workingDirectory = QFileInfo(executable).absolutePath();
	*out = launch;
	return true;
}

bool sendFiles(const QString &application, const QStringList &files, QString *error)
{
	Launch launch;
	if (!makeLaunch(application, files, &launch, error))
		return false;
	QProcess process;
	process.setProgram(launch.program);
	process.setArguments(launch.arguments);
	process.setWorkingDirectory(launch.workingDirectory);
	if (!process.startDetached())
		return fail(error, QStringLiteral("Could not open LocalSend: %1\n\nChoose its application or install it again.")
				   .arg(process.errorString()));
	return true;
}

} // namespace harpia::local_send
