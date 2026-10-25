#pragma once

#include "editor/SystemAudioCapture.hpp"

#include <memory>

namespace harpia::mac_audio {

// Native system audio is available through ScreenCaptureKit from macOS 13.
bool available();
// Stable bundle IDs, excluding Harpia and background-only agents. macOS does
// not expose a public per-app audible/playing query here, so these are running
// applications rather than a claim that each is currently producing sound.
QVector<SystemAudioApp> runningApps();

class Capture {
public:
	Capture();
	~Capture();
	Capture(const Capture &) = delete;
	Capture &operator=(const Capture &) = delete;
	bool start(const QString &bundleId, QString *error);
	void stop();
	bool running() const;
	QByteArray take(); // signed 16-bit interleaved stereo, 48000 Hz
	QString failure() const;
private:
	struct Impl;
	std::unique_ptr<Impl> d_;
};

} // namespace harpia::mac_audio
