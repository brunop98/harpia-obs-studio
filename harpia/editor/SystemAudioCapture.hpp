#pragma once

// Records computer audio for editor narration. Windows uses WASAPI process
// loopback (or endpoint loopback as fallback). macOS 13+ uses ScreenCaptureKit
// with native system/application filters and excludes Harpia's own playback.
// Unsupported platforms still offer microphone narration through Qt.
//
// take() returns signed 16-bit stereo PCM. Silent gaps are kept on the capture
// clock so a quiet application does not shorten or accelerate the take.

#include <QByteArray>
#include <QString>
#include <QVector>

#include <memory>

namespace harpia {

// An app with sound open on the default output, for "Only <app>".
struct SystemAudioApp {
	QString exe;  // Windows executable or macOS bundle ID; stable between launches
	QString name; // what people call it, "Google Chrome"
	bool playing = false;
	bool hasSound = true; // sound open on the output now (not only a window)
};

namespace system_audio {

// Whether the computer's sound can be recorded at all on this system.
bool available();
// Whether one app (or everything but Harpia) can be singled out.
bool perAppAvailable();
// Apps with sound open now, playing ones first; never Harpia itself.
QVector<SystemAudioApp> apps();
// Running apps a person would pick from: those with sound open (playing ones
// first), then those with a window on screen and no sound yet -- a game
// before it makes a noise. Never Harpia itself, nor the Windows shell (whose
// process tree is every app started from the taskbar).
QVector<SystemAudioApp> runningApps();

// The voiceover source ids for these, beside the microphones' own ids:
// "harpia:system" for everything, "harpia:system:app:<exe>" for one app.
QString sourceId(const QString &appExe = QString());
// Whether `id` is one of those; `appExe` gets the app (empty for everything).
bool isSourceId(const QString &id, QString *appExe = nullptr);

} // namespace system_audio

class SystemAudioCapture {
public:
	SystemAudioCapture();
	~SystemAudioCapture();
	SystemAudioCapture(const SystemAudioCapture &) = delete;
	SystemAudioCapture &operator=(const SystemAudioCapture &) = delete;

	// Start listening: to `appExe` alone, or to everything when it is empty.
	// Returns once the stream is running, or false with the reason.
	bool start(const QString &appExe, QString *error);
	// Stop listening. What was captured can still be take()n afterwards.
	void stop();
	bool running() const;

	int sampleRate() const; // native capture rate (48000 for SCK/process loopback)
	static constexpr int kChannels = 2;
	// Everything-but-Harpia, or one app: Harpia's own sound is not in it.
	bool excludesSelf() const;

	// The 16-bit stereo samples captured since the last call.
	QByteArray take();
	// Set when the stream died mid-take (the output device was unplugged, ...).
	QString failure() const;

	struct Impl;

private:
	std::unique_ptr<Impl> d_;
};

} // namespace harpia
