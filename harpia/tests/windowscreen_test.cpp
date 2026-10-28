// Monitor identity survives primary-screen changes; saved recorder and editor
// targets stay independent without touching the user's settings.
#include "ui/WindowScreen.hpp"

#include <QApplication>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QWidget>
#include <QWindow>

#include <cstdio>

using namespace harpia::window_screen;
namespace {
int failures = 0;
void check(bool condition, const char *message)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}

void identityChecks()
{
	const Screen saved{QStringLiteral("Display-2"), QStringLiteral("Acme"), QStringLiteral("Panel"),
			   QStringLiteral("unit-42"), QRect(-1920, 0, 1920, 1080)};
	Screen target = saved;
	Screen other = saved;
	other.name = QStringLiteral("Display-1");
	other.serial = QStringLiteral("unit-99");
	other.geometry.moveLeft(0);
	check(findScreen(saved, {other, target}) == 1 && findScreen(saved, {target, other}) == 0,
	      "screen-list reorder does not change the remembered hardware target");
	target.name = QStringLiteral("Renamed display");
	other.name = saved.name;
	check(findScreen(saved, {other, target}) == 1,
	      "hardware serial finds a renamed display before considering its old OS name");
	Screen placeholderSaved{QStringLiteral("DISPLAY2"), QStringLiteral("Acme"), QStringLiteral("Panel"),
				QStringLiteral("0"), QRect(1920, 0, 1920, 1080)};
	Screen duplicateSerial = placeholderSaved;
	duplicateSerial.name = QStringLiteral("DISPLAY1");
	Screen movedTarget = placeholderSaved;
	movedTarget.geometry.moveLeft(0);
	check(findScreen(placeholderSaved, {duplicateSerial, movedTarget}) == 1,
	      "an exact display name resolves duplicate placeholder serials before old monitor positions");

	Screen unnamedSerial = saved;
	unnamedSerial.serial.clear();
	other = unnamedSerial;
	other.geometry.moveLeft(0);
	check(findScreen(unnamedSerial, {other, unnamedSerial}) == 1,
	      "identical friendly names are resolved by their saved desktop location");
	target = saved;
	target.serial.clear();
	check(findScreen(saved, {target}) == 0,
	      "a driver that stops reporting a serial can still match a compatible display name");
	check(findScreen(unnamedSerial, {saved}) == 0,
	      "a saved display without a serial remains compatible when the driver gains one");
	other = saved;
	other.serial = QStringLiteral("different-unit");
	check(findScreen(saved, {other}) == -1,
	      "a known different serial cannot impersonate a display with the same name");
	other = saved;
	other.manufacturer = QStringLiteral("Different maker");
	check(findScreen(saved, {other}) == -1,
	      "conflicting hardware metadata rejects an otherwise matching serial and name");
	other = saved;
	other.name = QStringLiteral("Other display");
	other.serial = QStringLiteral("other-unit");
	check(findScreen(saved, {other}) == -1 && findScreen(saved, {}) == -1,
	      "disconnected displays and an empty monitor list return no match");
	check(centeredTopLeft(QSize(800, 600), QRect(-1920, 50, 1920, 1030)) == QPoint(-1360, 265),
	      "centering preserves negative desktop coordinates");
	check(centeredTopLeft(QSize(800, 600), QRect(10, 40, 1900, 1000)) == QPoint(560, 240),
	      "centering uses the available work area around taskbars and docks");
	check(centeredTopLeft(QSize(2500, 2000), QRect(-1920, 50, 1920, 1030)) == QPoint(-1920, 50),
	      "an oversized frame starts at the work area's visible top-left");
}

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
void settingsChecks()
{
	QScreen *primary = QGuiApplication::primaryScreen();
	check(primary != nullptr, "the QWidget fixture has a primary display");
	if (!primary)
		return;
	QWidget window;
	window.resize(320, 180);
	window.move(37, 53);
	window.show();
	QApplication::processEvents();
	const QPoint initialPosition = window.pos();
	const QSize initialSize = window.size();
	check(!restore(window, QStringLiteral("firstLaunch")) && window.pos() == initialPosition &&
	      window.size() == initialSize,
	      "a first launch leaves normal window placement and size unchanged");
	QSettings settings(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	check(!settings.contains(QStringLiteral("windowScreens/firstLaunch/name")),
	      "checking a first launch does not create a remembered target");
	save(window, QStringLiteral("recorder"));
	settings.sync();
	check(settings.contains(QStringLiteral("windowScreens/recorder/name")) &&
	      settings.value(QStringLiteral("windowScreens/recorder/name")).toString() == window.screen()->name() &&
	      settings.value(QStringLiteral("windowScreens/recorder/geometry")).toRect() == window.screen()->geometry(),
	      "saving a real window persists its display identity and logical desktop geometry");
	const QVariant recorderGeometry = settings.value(QStringLiteral("windowScreens/recorder/geometry"));
	settings.setValue(QStringLiteral("windowScreens/recorder/name"), QStringLiteral("disconnected-monitor"));
	settings.setValue(QStringLiteral("windowScreens/recorder/serial"), QStringLiteral("disconnected-unit"));
	settings.sync();
	QScreen *editorTarget = window.screen();
	save(window, QStringLiteral("editor"));
	settings.sync();
	check(settings.value(QStringLiteral("windowScreens/recorder/name")).toString() == QStringLiteral("disconnected-monitor") &&
	      settings.value(QStringLiteral("windowScreens/recorder/geometry")) == recorderGeometry &&
	      settings.value(QStringLiteral("windowScreens/editor/name")).toString() == window.screen()->name(),
	      "recorder and editor saves keep separate remembered targets");
	window.move(7, 11);
	const bool editorRestored = restore(window, QStringLiteral("editor"));
	check(editorRestored && window.windowHandle()->screen() == editorTarget && window.frameGeometry().topLeft() ==
	      centeredTopLeft(window.frameGeometry().size(), window.screen()->availableGeometry()),
	      "restoring a saved QWidget centers its frame on the display's work area");
	window.move(13, 17);
	const bool fallbackRestored = restore(window, QStringLiteral("recorder"));
	check(fallbackRestored && window.windowHandle()->screen() == primary &&
	      window.frameGeometry().topLeft() == centeredTopLeft(window.frameGeometry().size(), primary->availableGeometry()),
	      "a disconnected remembered target falls back to the current primary display");
	check(settings.value(QStringLiteral("windowScreens/recorder/name")).toString() == QStringLiteral("disconnected-monitor"),
	      "restoring a different window does not overwrite its saved monitor identity");
	window.resize(primary->availableGeometry().size() + QSize(1000, 1000));
	check(restore(window, QStringLiteral("editor")) &&
	      window.windowHandle()->screen() == editorTarget &&
	      editorTarget->availableGeometry().contains(window.frameGeometry()),
	      "the editor returns to its own monitor and fits a large frame inside its work area");
	window.hide();
	save(window, QStringLiteral("hiddenDialog"));
	settings.sync();
	check(settings.contains(QStringLiteral("windowScreens/hiddenDialog/name")) &&
	      settings.value(QStringLiteral("windowScreens/hiddenDialog/geometry")).toRect() == window.screen()->geometry(),
	      "a hidden finished dialog can still save its last display");
}
#endif
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir temporary;
	if (!temporary.isValid())
		return 2;
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
	// Native settings are file-based on Linux; registry and CFPreferences
	// cannot be safely redirected by setPath, so those platforms run pure checks.
	qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
	QSettings::setDefaultFormat(QSettings::IniFormat);
	for (QSettings::Format format : {QSettings::IniFormat, QSettings::NativeFormat}) {
		QSettings::setPath(format, QSettings::UserScope, temporary.path());
		QSettings::setPath(format, QSettings::SystemScope, temporary.path() + QStringLiteral("/system"));
	}
#endif
	QApplication app(argc, argv);
	identityChecks();
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
	settingsChecks();
#else
	std::printf("SKIP runtime settings checks: native settings are not file-isolated on this platform.\n");
#endif
	std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
