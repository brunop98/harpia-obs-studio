cmake_minimum_required(VERSION 3.28...3.30)

# Harpia uses this plugin for monitor and window capture only (see
# harpia/core/CaptureManager.cpp). Upstream's game_capture source -- and with it
# graphics-hook, get-graphics-offsets and inject-helper, which have to be built
# for both bitnesses because they inject into the target process -- has been
# removed. That is what required the whole tree to be configured and built a
# second time for Win32.

# The capture-compatibility list (compat-helpers, its data files and the
# file-updater that refreshed it from obsproject.com on every start) is gone
# too: it only fed warnings to OBS's properties dialog, which Harpia never shows.

add_library(win-capture MODULE)
add_library(OBS::capture ALIAS win-capture)

target_sources(
  win-capture
  PRIVATE
    audio-helpers.c
    audio-helpers.h
    cursor-capture.c
    cursor-capture.h
    dc-capture.c
    dc-capture.h
    duplicator-monitor-capture.c
    monitor-capture.c
    plugin-main.c
    window-capture.c
)

# obfuscate, inject-library, ipc-util and hook-config went with game capture:
# nothing left in this plugin includes them.
target_link_libraries(
  win-capture
  PRIVATE OBS::libobs OBS::w32-pthreads OBS::winrt-headers
)

configure_file(cmake/windows/obs-module.rc.in win-capture.rc)
target_sources(win-capture PRIVATE win-capture.rc)

set_target_properties_obs(win-capture PROPERTIES FOLDER plugins/win-capture PREFIX "")
