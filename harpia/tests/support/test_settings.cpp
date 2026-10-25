// Linked directly into test executables, never the application. A native Mac
// preferences domain cannot be redirected with QSettings::setPath(), so code
// under test also uses the explicit INI recorderSettings() factory.
#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

namespace {
void initializeTestSettings()
{
	static QTemporaryDir settings;
	if (!settings.isValid())
		qFatal("Could not create an isolated Harpia test settings directory");
	qputenv("HARPIA_TEST_SETTINGS_ROOT", settings.path().toUtf8());
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
	QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
#ifndef Q_OS_MACOS
	QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settings.path());
	QSettings::setPath(QSettings::NativeFormat, QSettings::SystemScope, settings.path());
#endif
	if (QCoreApplication::organizationName().isEmpty())
		QCoreApplication::setOrganizationName(QStringLiteral("HarpiaTests"));
}
Q_COREAPP_STARTUP_FUNCTION(initializeTestSettings)
} // namespace
