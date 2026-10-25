#pragma once

#include <QSettings>

namespace harpia {

// Explicit organization/application constructors always select NativeFormat;
// QSettings::setDefaultFormat() only changes the default constructor. Keep the
// application's native store, and give test binaries an explicitly INI store.
inline QSettings recorderSettings(QSettings::Format format = QSettings::NativeFormat)
{
#ifdef HARPIA_TEST_SETTINGS
	Q_UNUSED(format);
	if (qEnvironmentVariableIsEmpty("HARPIA_TEST_SETTINGS_ROOT"))
		qFatal("Harpia test settings require the isolated test-settings bootstrap");
	return QSettings(QSettings::IniFormat, QSettings::UserScope,
		QStringLiteral("Harpia"), QStringLiteral("Recorder"));
#else
	return QSettings(format, QSettings::UserScope,
		QStringLiteral("Harpia"), QStringLiteral("Recorder"));
#endif
}

} // namespace harpia
