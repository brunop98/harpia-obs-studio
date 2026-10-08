#pragma once

// The name people see: window titles, the splash, About, the startup and crash
// messages, and -- through resources/harpia.rc -- what Windows shows for
// harpia.exe in Task Manager, on the taskbar and in Properties > Details.
//
// NOT the QApplication name in main.cpp ("Harpia Recorder"): Qt derives the
// settings and data folders from that one (QSettings, AppConfigLocation,
// AppDataLocation), so changing it would leave every saved preset, sound,
// template and setting behind in the old folder.
//
// Only a #define: the resource compiler includes this file too.
#define HARPIA_APP_DISPLAY_NAME "Harpia Recorder and Editor"
