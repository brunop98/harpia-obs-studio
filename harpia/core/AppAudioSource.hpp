#pragma once

// The libobs source behind "PC audio from" (core/PcAudioApps.hpp): one app's
// sound, or everything but one app's, through Windows' process loopback.
//
// Settings: "exe", the program's file name ("spotify.exe"), and "exclude",
// true for everything BUT that program. The program need not be running: the
// source looks for it every second and follows it as it starts, quits and
// starts again. An app to record that is not running records nothing; an app
// to leave out that is not running leaves out nothing.
//
// Unlike OBS Studio's Application Audio Capture, it finds the program by its
// processes rather than by a window, so a music player minimised to the
// notification area is found too -- which is the one people leave out.

namespace harpia {

namespace app_audio {

// Register the source with libobs where Windows can do it (Windows 10 version
// 2004 and newer). Call once the modules are loaded; later calls do nothing.
// Returns whether the source is registered.
bool registerSource();

} // namespace app_audio

} // namespace harpia
