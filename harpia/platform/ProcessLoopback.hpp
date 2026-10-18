#pragma once

// Windows' process loopback: the sound of one program (and the programs it
// started), or of every program but one. Two features stand on it:
//
//   - the recorder's "PC audio from" choice (core/AppAudioSource): only these
//     apps, or every app but the music player;
//   - the voiceover panel's Windows audio (editor/SystemAudioCapture).
//
// Windows only: everything here is declared, and defined, only there.

#if defined(_WIN32)

#include <string>

struct IAudioClient;

namespace harpia {

namespace process_loopback {

// Whether this Windows has it. Microsoft documents build 20348, but it works
// from Windows 10 version 2004 (build 19041), where OBS Studio's application
// audio capture has relied on it since 2022.
bool available();

enum class Mode {
	IncludeTree = 0, // the program and the programs it started
	ExcludeTree = 1, // everything but those
};

// An audio client for the stream, not yet initialised: the caller picks the
// format, calls Initialize with AUDCLNT_STREAMFLAGS_LOOPBACK, and releases it.
// Null on failure, with the reason in `hr` (an HRESULT).
IAudioClient *activate(unsigned long pid, Mode mode, long *hr);

// The top of a running program's own process tree, by its file name
// ("spotify.exe", any case): a browser's main process, whose tree holds the
// audio service that actually plays. 0 when it is not running.
unsigned long rootProcessOf(const std::wstring &exe);

} // namespace process_loopback

} // namespace harpia

#endif
