cmake_minimum_required(VERSION 3.28...3.30)

# Harpia Recorder — a minimal screen recorder built directly on the libobs
# backend. It reuses OBS's capture/encoder/output plugins but ships its own
# small Qt frontend instead of the full OBS Studio UI.

find_package(Qt6 REQUIRED Widgets Gui)

# FFmpeg (bundled with OBS deps) is used to extract clip thumbnails.
find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swscale)

if(NOT TARGET OBS::libobs)
  add_subdirectory("${CMAKE_SOURCE_DIR}/libobs" libobs)
endif()

add_executable(harpia-recorder)
add_executable(OBS::harpia ALIAS harpia-recorder)

target_link_libraries(
  harpia-recorder
  PRIVATE
    OBS::libobs
    Qt::Widgets
    Qt::Gui
    FFmpeg::avcodec
    FFmpeg::avformat
    FFmpeg::avutil
    FFmpeg::swscale
)

set_target_properties(
  harpia-recorder
  PROPERTIES AUTOMOC TRUE AUTOUIC TRUE AUTORCC TRUE AUTOGEN_PARALLEL AUTO
)

target_compile_definitions(harpia-recorder PRIVATE QT_ENABLE_STRICT_MODE_UP_TO=0xFF0000)

target_include_directories(harpia-recorder PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Provide the graphics-module library file names to the app, exactly as the OBS
# frontend does, so ObsContext can pass the right renderer to obs_reset_video.
foreach(graphics_library IN ITEMS opengl metal d3d11)
  string(TOUPPER ${graphics_library} graphics_library_U)
  if(TARGET OBS::libobs-${graphics_library})
    target_compile_definitions(
      harpia-recorder
      PRIVATE
        DL_${graphics_library_U}="$<$<IF:$<PLATFORM_ID:Windows>,TARGET_FILE_NAME,TARGET_SONAME_FILE_NAME>:OBS::libobs-${graphics_library}>"
    )
  else()
    target_compile_definitions(harpia-recorder PRIVATE DL_${graphics_library_U}="")
  endif()
endforeach()

target_sources(
  harpia-recorder
  PRIVATE
    main.cpp
    Version.hpp
    core/Logger.cpp
    core/Logger.hpp
    core/ObsContext.cpp
    core/ObsContext.hpp
    core/AudioManager.cpp
    core/AudioManager.hpp
    core/CaptureManager.cpp
    core/CaptureManager.hpp
    core/EncoderFactory.cpp
    core/EncoderFactory.hpp
    core/RecordingController.cpp
    core/RecordingController.hpp
    core/WebcamRecorder.cpp
    core/WebcamRecorder.hpp
    model/Preset.cpp
    model/Preset.hpp
    model/PresetStore.cpp
    model/PresetStore.hpp
    model/FileNameTemplate.cpp
    model/FileNameTemplate.hpp
    platform/IdleMonitor.hpp
    library/ClipLibrary.cpp
    library/ClipLibrary.hpp
    library/ThumbnailCache.cpp
    library/ThumbnailCache.hpp
    ui/MainWindow.cpp
    ui/MainWindow.hpp
    ui/AudioPanel.cpp
    ui/AudioPanel.hpp
    ui/RecentListWidget.hpp
    ui/PresetEditorDialog.cpp
    ui/PresetEditorDialog.hpp
    ui/MousePreview.cpp
    ui/MousePreview.hpp
    ui/MouseFxOverlay.cpp
    ui/MouseFxOverlay.hpp
    ui/ClipLibraryWindow.cpp
    ui/ClipLibraryWindow.hpp
    ui/ErrorLogsPanel.cpp
    ui/ErrorLogsPanel.hpp
    ui/RegionTool.cpp
    ui/RegionTool.hpp
)

# Platform-specific idle detection backend.
if(OS_WINDOWS)
  target_sources(harpia-recorder PRIVATE platform/win/WinIdleMonitor.cpp)
elseif(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
  target_sources(harpia-recorder PRIVATE platform/nix/NixIdleMonitor.cpp)
  # Wayland/GNOME idle uses the Mutter IdleMonitor over D-Bus (Qt::DBus); X11/Xss
  # are loaded at runtime via dlopen, so only libdl is needed at link time.
  find_package(Qt6 REQUIRED DBus)
  target_link_libraries(harpia-recorder PRIVATE Qt::DBus ${CMAKE_DL_LIBS})
elseif(OS_MACOS)
  target_sources(harpia-recorder PRIVATE platform/mac/MacIdleMonitor.cpp)
  target_link_libraries(harpia-recorder PRIVATE "-framework CoreGraphics")
endif()

if(OS_WINDOWS)
  include(cmake/os-windows.cmake)
endif()

set_target_properties_obs(
  harpia-recorder
  PROPERTIES FOLDER harpia OUTPUT_NAME "$<IF:$<PLATFORM_ID:Windows>,harpia,harpia-recorder>"
)
