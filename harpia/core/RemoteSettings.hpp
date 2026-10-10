#pragma once

// Whether Harpia listens for the local remote control (core/RemoteControl.hpp)
// and on which port. QSettings "remote/...". On by default, so the Unity Play
// Mode Recorder works without any setup in Harpia; the server only ever
// listens on 127.0.0.1 and refuses requests from web pages.

#include <QtGlobal>

namespace harpia {

inline constexpr quint16 kRemoteDefaultPort = 47811;

struct RemoteSettings {
	bool enabled = true;
	int port = kRemoteDefaultPort;

	static RemoteSettings load();
	void save() const;
};

} // namespace harpia
