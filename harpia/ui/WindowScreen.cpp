#include "WindowScreen.hpp"
#include "core/RecorderSettings.hpp"

#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace harpia::window_screen {
namespace {
Screen describe(const QScreen &screen)
{
	return {screen.name(), screen.manufacturer(), screen.model(), screen.serialNumber(), screen.geometry()};
}

bool compatible(const QString &saved, const QString &current)
{
	return saved.isEmpty() || current.isEmpty() || saved == current;
}
} // namespace

int findScreen(const Screen &saved, const QVector<Screen> &screens)
{
	// Prefer hardware identity, then the OS's display name. Some platforms
	// report the same friendly name for identical displays; position resolves
	// those ties without confusing the current primary screen with the last one.
	for (int pass = 0; pass < 2; ++pass) {
		int best = -1;
		bool bestNameMatch = false;
		qint64 bestDistance = std::numeric_limits<qint64>::max();
		for (int i = 0; i < screens.size(); ++i) {
			const Screen &screen = screens[i];
			const bool match = pass == 0
				? !saved.serial.isEmpty() && saved.serial == screen.serial &&
					compatible(saved.manufacturer, screen.manufacturer) && compatible(saved.model, screen.model)
				: !saved.name.isEmpty() && saved.name == screen.name &&
					compatible(saved.serial, screen.serial) &&
					compatible(saved.manufacturer, screen.manufacturer) && compatible(saved.model, screen.model);
			if (!match)
				continue;
			const qint64 dx = qint64(screen.geometry.x()) - saved.geometry.x();
			const qint64 dy = qint64(screen.geometry.y()) - saved.geometry.y();
			const qint64 distance = std::abs(dx) + std::abs(dy);
			const bool nameMatch = !saved.name.isEmpty() && saved.name == screen.name;
			if (best < 0 || (nameMatch && !bestNameMatch) || (nameMatch == bestNameMatch && distance < bestDistance)) {
				best = i;
				bestNameMatch = nameMatch;
				bestDistance = distance;
			}
		}
		if (best >= 0)
			return best;
	}
	return -1;
}

QPoint centeredTopLeft(const QSize &frameSize, const QRect &available)
{
	return available.topLeft() + QPoint(std::max(0, (available.width() - frameSize.width()) / 2),
					  std::max(0, (available.height() - frameSize.height()) / 2));
}

bool restore(QWidget &window, const QString &key)
{
	auto settings = recorderSettings();
	settings.beginGroup(QStringLiteral("windowScreens/") + key);
	if (!settings.contains(QStringLiteral("name")))
		return false; // first launch keeps the platform's normal placement
	const Screen saved{settings.value(QStringLiteral("name")).toString(),
			   settings.value(QStringLiteral("manufacturer")).toString(),
			   settings.value(QStringLiteral("model")).toString(),
			   settings.value(QStringLiteral("serial")).toString(),
			   settings.value(QStringLiteral("geometry")).toRect()};
	const auto screens = QGuiApplication::screens();
	QVector<Screen> descriptions;
	for (const QScreen *screen : screens)
		descriptions.append(describe(*screen));
	const int index = findScreen(saved, descriptions);
	QScreen *target = index >= 0 ? screens[index] : QGuiApplication::primaryScreen();
	if (!target || !target->availableGeometry().isValid())
		return false;
	// QWindow::setScreen alone does not relocate a window in a shared virtual
	// desktop. Move it as well, using Qt's logical pixels for mixed-DPI setups.
	if (QWindow *handle = window.windowHandle())
		handle->setScreen(target);
	const QRect available = target->availableGeometry();
	const QSize decoration = window.frameGeometry().size() - window.size();
	const QSize maximum = (available.size() - decoration).expandedTo(QSize(1, 1));
	window.resize(window.size().boundedTo(maximum).expandedTo(window.minimumSize()));
	window.move(centeredTopLeft(window.frameGeometry().size(), available));
	return true;
}

void save(QWidget &window, const QString &key)
{
	QScreen *screen = window.screen();
	if (!screen)
		return;
	const Screen current = describe(*screen);
	auto settings = recorderSettings();
	settings.beginGroup(QStringLiteral("windowScreens/") + key);
	settings.setValue(QStringLiteral("name"), current.name);
	settings.setValue(QStringLiteral("manufacturer"), current.manufacturer);
	settings.setValue(QStringLiteral("model"), current.model);
	settings.setValue(QStringLiteral("serial"), current.serial);
	settings.setValue(QStringLiteral("geometry"), current.geometry);
}

} // namespace harpia::window_screen
