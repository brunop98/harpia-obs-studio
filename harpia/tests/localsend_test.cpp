// LocalSend's GUI takes file arguments on Windows/Linux and open-document
// events on macOS. Exercise the actual argument handoff with a private helper,
// never the user's installed LocalSend or a real network receiver.
#include "core/LocalSend.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

using namespace harpia::local_send;

namespace {
int failures = 0;
void check(bool value, const char *message)
{
	std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
	failures += !value;
}
bool write(const QString &path, const QByteArray &bytes)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString &path)
{
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool executable(const QString &path, const QByteArray &bytes)
{
	return write(path, bytes) && QFile::setPermissions(path,
		QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup);
}
QString fixtureApplication(const QString &folder)
{
#if defined(Q_OS_MACOS)
	const QString bundle = QDir(folder).filePath(QStringLiteral("LocalSend.app"));
	write(QDir(bundle).filePath(QStringLiteral("Contents/Info.plist")),
		"<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
		"<key>CFBundleExecutable</key><string>localsend_app</string>"
		"<key>CFBundleIdentifier</key><string>test.harpia.localsend</string></dict></plist>");
	executable(QDir(bundle).filePath(QStringLiteral("Contents/MacOS/localsend_app")), "#!/bin/sh\nexit 0\n");
	return bundle;
#elif defined(Q_OS_WIN)
	const QString target = QDir(folder).filePath(QStringLiteral("localsend_app.exe"));
	QFile::copy(QCoreApplication::applicationFilePath(), target);
	return target;
#else
	const QString target = QDir(folder).filePath(QStringLiteral("localsend_app"));
	QFile::copy(QCoreApplication::applicationFilePath(), target);
	QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
	return target;
#endif
}
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	// A copied test executable stands in for LocalSend. Capture precisely the
	// arguments seen by the new process, independently of the launch planner.
	if (QFileInfo(QCoreApplication::applicationFilePath()).completeBaseName() == QStringLiteral("localsend_app")) {
		QJsonObject capture;
		capture.insert(QStringLiteral("arguments"), QJsonArray::fromStringList(app.arguments().mid(1)));
		capture.insert(QStringLiteral("directory"), QDir::currentPath());
		return write(QCoreApplication::applicationFilePath() + QStringLiteral(".capture.json"),
			QJsonDocument(capture).toJson()) ? 0 : 1;
	}
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir temporary;
	check(temporary.isValid(), "all application and recording fixtures live in a private directory");
	if (!temporary.isValid())
		return 1;
	const QString folder = temporary.path() + QStringLiteral("/optional LocalSend");
	QDir().mkpath(folder);
	const QString application = fixtureApplication(folder);
	check(findApplication(application) == application, "a chosen LocalSend application is accepted without launching it");
	check(findApplication(temporary.path() + QStringLiteral("/missing-application")).isEmpty(),
	      "a missing configured application requests setup rather than selecting another program");
	check(findApplication(temporary.path()).isEmpty(), "a directory cannot be mistaken for the LocalSend executable");
	const QString ordinary = temporary.path() + QStringLiteral("/clip part001.mp4");
	const QString unicode = temporary.path() + QStringLiteral("/Férias São Paulo 🎬.mp4");
	const QString special = temporary.path() + QStringLiteral("/clip ' $(touch SHOULD_NOT_EXIST) & ; -n.mp4");
	check(write(ordinary, "first clip") && write(unicode, "second clip") && write(special, "third clip"),
	      "recordings with spaces, Unicode and shell metacharacters exist");
	QString error;
	Launch launch;
	check(makeLaunch(application, {ordinary, unicode, special, ordinary}, &launch, &error) && error.isEmpty(),
	      "multiple selected recordings produce one valid handoff");
#if defined(Q_OS_MACOS)
	check(launch.program == QStringLiteral("/usr/bin/open") &&
		launch.arguments == QStringList{QStringLiteral("-a"), application, QStringLiteral("--"), ordinary, unicode, special},
	      "Mac hands documents to the chosen bundle with open, preserving each path as an argument");
#else
	check(launch.program == application && launch.arguments == QStringList{ordinary, unicode, special},
	      "the GUI receives ordered, deduplicated literal file arguments");
#endif
	check(launch.workingDirectory == QFileInfo(application).absolutePath(),
	      "the process starts alongside its application rather than in the recording directory");
	error = QStringLiteral("old error");
	check(makeLaunch(application, {ordinary}, &launch, &error) && error.isEmpty(),
	      "a successful request clears stale errors");
	check(!makeLaunch(application, {}, &launch, &error) && !error.isEmpty() && launch.program.isEmpty(),
	      "an empty selection fails before opening an application and clears a stale launch plan");
	check(!makeLaunch(application, {ordinary, temporary.path() + QStringLiteral("/deleted.mp4")}, &launch, &error) &&
		!error.isEmpty() && launch.program.isEmpty(),
	      "a missing selected file rejects the complete handoff instead of silently omitting it");
	check(!makeLaunch(application, {temporary.path()}, &launch, &error),
	      "directories cannot accidentally share a whole recording folder");
	check(!makeLaunch(application, {QString()}, &launch, &error), "an empty file path is rejected");
	QString invalid = ordinary;
	invalid.append(QChar::Null);
	check(!makeLaunch(application, {invalid}, &launch, &error), "embedded NUL cannot truncate a selected file path");
	check(!makeLaunch(QString(), {ordinary}, &launch, &error) && error.contains(QStringLiteral("LocalSend")),
	      "an absent application returns a recommendation-ready LocalSend error");
	check(!makeLaunch(application, {ordinary}, nullptr, &error), "a null launch result cannot crash a request");
	check(!sendFiles(temporary.path() + QStringLiteral("/not-installed"), {ordinary}, &error),
	      "an absent LocalSend never starts a process");
	const QString commandLine = temporary.path() + QStringLiteral("/localsend-cli");
	executable(commandLine, "#!/bin/sh\nexit 0\n");
	check(findApplication(commandLine).isEmpty(), "the terminal-only LocalSend CLI is not treated as the GUI");

#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
	const QByteArray oldPath = qgetenv("PATH");
	qputenv("PATH", folder.toUtf8());
	check(findApplication() == application, "Linux discovers the optional GUI on PATH only when asked");
	qputenv("PATH", temporary.path().toUtf8());
	check(findApplication().isEmpty(), "a missing optional GUI remains absent without installation or a network request");
	qputenv("PATH", oldPath);

	check(sendFiles(application, {special, unicode, ordinary}, &error), "a real detached helper starts with the chosen files");
	const QString capturePath = application + QStringLiteral(".capture.json");
	QElapsedTimer clock;
	clock.start();
	QJsonObject captured;
	while (clock.elapsed() < 5000) {
		captured = QJsonDocument::fromJson(read(capturePath)).object();
		if (!captured.isEmpty())
			break;
		QThread::msleep(10);
	}
	check(captured.value(QStringLiteral("arguments")).toArray() == QJsonArray::fromStringList({special, unicode, ordinary}),
	      "the actual process receives exact Unicode and metacharacter arguments in selection order");
	check(captured.value(QStringLiteral("directory")).toString() == folder,
	      "the detached process receives the planned working directory");
	check(!QFileInfo::exists(QDir(folder).filePath(QStringLiteral("SHOULD_NOT_EXIST"))),
	      "shell-like file names are never interpreted as commands");
	const QString denied = temporary.path() + QStringLiteral("/not-executable");
	write(denied, "a file, not an executable");
	check(findApplication(denied).isEmpty(), "a nonexecutable file cannot be configured as LocalSend");
	const QString broken = temporary.path() + QStringLiteral("/broken-localsend");
	executable(broken, "#!/missing/harpia-test-interpreter\n");
	check(!sendFiles(broken, {ordinary}, &error) && error.contains(QStringLiteral("Could not open LocalSend")),
	      "an application that cannot start returns an actionable error");
#endif
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
