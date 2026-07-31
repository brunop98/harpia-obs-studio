cmake_minimum_required(VERSION 3.28...3.30)

# Harpia uses this plugin for monitor and window capture only (see
# harpia/core/CaptureManager.cpp). Upstream's game_capture source -- and with it
# graphics-hook, get-graphics-offsets and inject-helper, which have to be built
# for both bitnesses because they inject into the target process -- has been
# removed. That is what required the whole tree to be configured and built a
# second time for Win32.

option(ENABLE_COMPAT_UPDATES "Checks for capture compatibility data updates" ON)

set(COMPAT_URL "https://obsproject.com/obs2_update/win-capture" CACHE STRING "Default services package URL")
mark_as_advanced(COMPAT_URL)

if(NOT TARGET OBS::file-updater)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/file-updater" "${CMAKE_BINARY_DIR}/shared/file-updater")
endif()

find_package(jansson REQUIRED)

add_library(win-capture MODULE)
add_library(OBS::capture ALIAS win-capture)

target_sources(
  win-capture
  PRIVATE
    audio-helpers.c
    audio-helpers.h
    compat-format-ver.h
    compat-helpers.c
    compat-helpers.h
    cursor-capture.c
    cursor-capture.h
    dc-capture.c
    dc-capture.h
    duplicator-monitor-capture.c
    monitor-capture.c
    plugin-main.c
    window-capture.c
)

target_compile_definitions(
  win-capture
  PRIVATE
    COMPAT_URL="${COMPAT_URL}"
    "$<$<BOOL:${ENABLE_COMPAT_UPDATES}>:ENABLE_COMPAT_UPDATES>"
    OBS_VERSION="${OBS_VERSION_CANONICAL}"
)

# obfuscate, inject-library, ipc-util and hook-config went with game capture:
# nothing left in this plugin includes them.
target_link_libraries(
  win-capture
  PRIVATE OBS::file-updater OBS::libobs OBS::w32-pthreads OBS::winrt-headers jansson::jansson
)

# Remove once jansson has been fixed on obs-deps
target_link_options(win-capture PRIVATE /IGNORE:4098)

configure_file(cmake/windows/obs-module.rc.in win-capture.rc)
target_sources(win-capture PRIVATE win-capture.rc)

set_target_properties_obs(win-capture PROPERTIES FOLDER plugins/win-capture PREFIX "")
