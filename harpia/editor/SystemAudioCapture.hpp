#pragma once

// Recording the computer's own sound -- whatever Windows is playing -- for the
// voiceover panel's "Windows audio" sources: play a video in the browser and
// grab its sound straight onto the timeline.
//
// Two ways to listen, both WASAPI:
//   - Process loopback (Windows 10 version 2004 and newer; see
//     platform/ProcessLoopback.hpp):
//     either every app EXCEPT Harpia -- the timeline playing along is never
//     recorded back into itself -- or ONE app and its child processes (only
//     the browser: no Discord pings, no music player in the background).
//   - Endpoint loopback, everywhere else: the whole mix of the default output.
//     Harpia's own sound is in it, so the editor mutes its preview for the take
//     (excludesSelf() says which one is running).
//
// Elsewhere than Windows there is nothing to listen with: available() is false
// and the panel offers microphones only.
//
// The capture runs on its own thread and hands over 16-bit stereo PCM through
// take(). Gaps where nothing is playing (endpoint loopback sends no packets
// then) are filled with silence by the clock, so a take is as long as it took.

#include <QByteArray>
#include <QString>
#include <QVector>

#include <memory>

namespace harpia {

// An app with sound open on the default output, for "Only <app>".
struct SystemAudioApp {
	QString exe;  // file name, "chrome.exe": what is remembered between launches
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

	int sampleRate() const; // what Windows plays at (48000 for process loopback)
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
