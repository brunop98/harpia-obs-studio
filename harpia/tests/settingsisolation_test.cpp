#include "core/RecorderSettings.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <cstdio>

static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}
int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	const QString root = qEnvironmentVariable("HARPIA_TEST_SETTINGS_ROOT");
	check(!root.isEmpty() && QFileInfo(root).isDir(), "settings bootstrap creates a process-private directory");
	QSettings native(QStringLiteral("HarpiaSettingsConstructorProbe"), QStringLiteral("Test"));
	check(native.format() == QSettings::NativeFormat,
		"Qt's explicit domain constructor remains native despite the default format");
	QSettings settings = harpia::recorderSettings();
	check(settings.format() == QSettings::IniFormat, "the recorder factory explicitly chooses INI in test binaries");
	check(settings.fileName().startsWith(root + QDir::separator()),
		"the explicit recorder domain is inside the private directory");
	settings.setValue(QStringLiteral("isolation/probe"), 42);
	settings.sync();
	check(settings.status() == QSettings::NoError && QFileInfo::exists(settings.fileName()),
		"the test writes a real private INI file");
	check(harpia::recorderSettings().value(QStringLiteral("isolation/probe")).toInt() == 42,
		"the production call sites share the same isolated test store");
	check(harpia::recorderSettings(QSettings::NativeFormat).format() == QSettings::IniFormat,
		"a caller's explicit native preference cannot escape isolation in tests");
	QSettings defaults;
	check(defaults.format() == QSettings::IniFormat && defaults.fileName().startsWith(root + QDir::separator()),
		"default settings constructors are isolated too");
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
