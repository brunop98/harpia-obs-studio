# Windows-specific configuration for the Harpia recorder.
#
# GetLastInputInfo (idle detection) lives in user32; it is part of the default
# import set, but we link it explicitly for clarity.

target_link_libraries(harpia-recorder PRIVATE user32)

# The editor window's translation unit has grown past the 65k COFF section
# limit (C1128); /bigobj lifts it. Harmless for the smaller files.
target_compile_options(harpia-recorder PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)

# Build a GUI app (no console window) on Windows.
set_target_properties(
  harpia-recorder
  PROPERTIES
    WIN32_EXECUTABLE TRUE
    VS_DEBUGGER_COMMAND "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>/bin/64bit/$<TARGET_FILE_NAME:harpia-recorder>"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>/bin/64bit"
)

set(_harpia_runtime_dir "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>/bin/64bit")

add_custom_command(
  TARGET harpia-recorder
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "$<TARGET_FILE_DIR:Qt6::Core>" "${_harpia_runtime_dir}"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "$<TARGET_FILE_DIR:FFmpeg::avcodec>" "${_harpia_runtime_dir}"
  # Every Qt plugin folder, not just platforms/: tls/ is what makes HTTPS work
  # (subtitles, yt-dlp thumbnails -- without it every request fails at once),
  # multimedia/ is the audio backend for playback and the microphone,
  # imageformats/ reads JPEG/WebP stills, styles/ and iconengines/ the look.
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "$<TARGET_FILE_DIR:Qt6::Core>/../plugins" "${_harpia_runtime_dir}"
  VERBATIM
)

# The Microsoft Visual C++ runtime the build was compiled against, next to the
# exe (app-local), so the rundir and the package run on a PC that has never had
# Visual Studio or the VC++ Redistributable. Taken from Visual Studio's own
# redist folder (VC\Redist\MSVC\<version>\x64\Microsoft.VC143.CRT): the files
# Microsoft licenses for redistribution, and -- CMake picks the newest one
# installed -- no older than the compiler, which the runtime requires (an older
# msvcp140.dll than the compiler can stop the app with "procedure entry point
# not found"). The package used to copy whatever System32 happened to hold.
# The obs-ffmpeg-mux and encoder test helpers beside the exe use the same copy.
# (A Debug build runs on the debug runtime, which is not redistributable; the
# release one copied next to it does no harm.)
unset(MSVC_REDIST_DIR CACHE) # found again on every configure: a Visual Studio update replaces the folder
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE) # the Universal CRT is part of Windows 10 and 11
include(InstallRequiredSystemLibraries)
set(_harpia_crt "")
foreach(_dll IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
  if(EXISTS "${_dll}")
    list(APPEND _harpia_crt "${_dll}")
  endif()
endforeach()
if(_harpia_crt)
  message(STATUS "Harpia: Visual C++ runtime from ${MSVC_REDIST_DIR}")
  add_custom_command(
    TARGET harpia-recorder
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_harpia_crt} "${_harpia_runtime_dir}"
    COMMENT "Copying the Visual C++ runtime next to harpia.exe"
    VERBATIM
  )
else()
  message(WARNING "Harpia: Visual Studio's Visual C++ runtime (VC\\Redist\\MSVC) was not found, so it is not "
                  "copied next to harpia.exe: the build will only run where the Visual C++ Redistributable "
                  "is installed, and Build-Harpia.ps1 -Package will refuse to package it.")
endif()

# The ffmpeg_muxer recording output spawns the obs-ffmpeg-mux helper EXECUTABLE
# as a child process, resolved as "<dir of harpia.exe>/obs-ffmpeg-mux.exe". Copy
# the freshly built helper right next to harpia.exe so recording can start it —
# independent of the OBS install/bundle machinery (which only runs for the
# obs-studio target). harpia-recorder depends on this target (see CMakeLists.txt),
# so it is always built before this copy runs.
if(TARGET obs-ffmpeg-mux)
  add_custom_command(
    TARGET harpia-recorder
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:obs-ffmpeg-mux>" "${_harpia_runtime_dir}"
    COMMENT "Copying obs-ffmpeg-mux helper next to harpia.exe"
    VERBATIM
  )
endif()
