#include "core/RemoteSettings.hpp"

#include <QSettings>

namespace harpia {

RemoteSettings RemoteSettings::load()
{
	QSettings s(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	RemoteSettings r;
	r.enabled = s.value(QStringLiteral("remote/enabled"), true).toBool();
	const int port = s.value(QStringLiteral("remote/port"), int(kRemoteDefaultPort)).toInt();
	r.port = (port >= 1024 && port <= 65535) ? port : int(kRemoteDefaultPort);
	return r;
}

void RemoteSettings::save() const
{
	QSettings s(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	s.setValue(QStringLiteral("remote/enabled"), enabled);
	s.setValue(QStringLiteral("remote/port"), port);
}

} // namespace harpia
