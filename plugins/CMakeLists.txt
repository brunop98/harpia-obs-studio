cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_PLUGINS "Enable building OBS plugins" ON)

if(NOT ENABLE_PLUGINS)
  set_property(GLOBAL APPEND PROPERTY OBS_FEATURES_DISABLED "Plugin Support")
  return()
endif()

set_property(GLOBAL APPEND PROPERTY OBS_FEATURES_ENABLED "Plugin Support")

# Harpia recorder needs only a minimal set of runtime plugins: screen capture,
# audio capture, H.264 encoders (software + GPU), the ffmpeg muxer/output, and
# the crop filter. The upstream streaming, UI, capture-card, camera, browser,
# and websocket plugins have been removed to keep the build light. (Stock
# obs-studio no longer builds from this tree as a result.)

# win-dshow (webcam capture) and obs-qsv11 (Intel QSV encoder) REQUIRE ATL on
# Windows. If the "C++ ATL for v143 build tools" VS component isn't installed
# those plugins can't compile — and since harpia-recorder build-depends on the
# plugins that exist, that would block the whole recorder. Detect ATL and simply
# skip those two when it's missing, so the recorder always builds (just without
# webcam / QSV until ATL is installed). ATL is a Windows/MSVC concern only.
if(OS_WINDOWS)
  include(CheckIncludeFileCXX)
  check_include_file_cxx("atlbase.h" HARPIA_HAVE_ATL)
  if(NOT HARPIA_HAVE_ATL)
    message(
      WARNING
      "ATL (atlbase.h) not found — skipping win-dshow (webcam) and obs-qsv11 (Intel QSV). "
      "Install the Visual Studio component 'C++ ATL for latest v143 build tools (x86 & x64)' "
      "and reconfigure to enable them."
    )
  endif()
else()
  set(HARPIA_HAVE_ATL TRUE)
endif()

# Add plugins in alphabetical order to retain order in IDE projects.
#
# Linux audio + capture (+ webcam via v4l2)
add_obs_plugin(linux-alsa PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-capture PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-pipewire PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-pulseaudio PLATFORMS LINUX FREEBSD OPENBSD)
add_obs_plugin(linux-v4l2 PLATFORMS LINUX FREEBSD OPENBSD)
# macOS capture (screen + audio), webcam, and hardware encoder
add_obs_plugin(mac-avcapture PLATFORMS MACOS)
add_obs_plugin(mac-capture PLATFORMS MACOS)
add_obs_plugin(mac-videotoolbox PLATFORMS MACOS)
# Cross-platform encode / mux / filter
add_obs_plugin(obs-ffmpeg)
add_obs_plugin(obs-filters)
add_obs_plugin(obs-nvenc PLATFORMS WINDOWS LINUX ARCHITECTURES x64 x86_64)
# obs-qsv11 needs ATL on Windows (uses VPL on Linux, no ATL there).
if(NOT OS_WINDOWS OR HARPIA_HAVE_ATL)
  add_obs_plugin(obs-qsv11 PLATFORMS WINDOWS LINUX ARCHITECTURES x64 x86_64)
endif()
add_obs_plugin(obs-x264)
# Windows capture + audio + webcam (dshow). win-dshow (webcam) needs ATL.
add_obs_plugin(win-capture PLATFORMS WINDOWS)
if(HARPIA_HAVE_ATL)
  add_obs_plugin(win-dshow PLATFORMS WINDOWS)
endif()
add_obs_plugin(win-wasapi PLATFORMS WINDOWS)
