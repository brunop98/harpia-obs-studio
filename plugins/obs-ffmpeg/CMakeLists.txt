cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_FFMPEG_LOGGING "Enables obs-ffmpeg logging" OFF)
# Not here: the native SRT/RIST streaming output (and the librist / libsrt it
# needed) and the media source (with shared/media-playback). Harpia records
# files; it neither streams nor plays media through libobs. Nor its HLS,
# MPEG-TS and replay-buffer outputs, or the OpenH264 encoder: Harpia records
# with ffmpeg_muxer / ffmpeg_output and the hardware encoders or x264.

if(OS_LINUX OR OS_WINDOWS)
  option(ENABLE_FFMPEG_NVENC "Enable legacy FFmpeg NVENC encoder" OFF)
endif()

include(cmake/dependencies.cmake)

add_library(obs-ffmpeg MODULE)
add_library(OBS::ffmpeg ALIAS obs-ffmpeg)

add_subdirectory(ffmpeg-mux)

target_sources(
  obs-ffmpeg
  PRIVATE
    $<$<BOOL:${ENABLE_FFMPEG_LOGGING}>:obs-ffmpeg-logging.c>
    $<$<BOOL:${ENABLE_FFMPEG_NVENC}>:obs-ffmpeg-nvenc.c>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:obs-ffmpeg-vaapi.c>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:vaapi-utils.c>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:vaapi-utils.h>
    obs-ffmpeg-audio-encoders.c
    obs-ffmpeg-av1.c
    obs-ffmpeg-compat.h
    obs-ffmpeg-formats.h
    obs-ffmpeg-mux.c
    obs-ffmpeg-mux.h
    obs-ffmpeg-output.c
    obs-ffmpeg-output.h
    obs-ffmpeg-video-encoders.c
    obs-ffmpeg.c
)

target_compile_options(obs-ffmpeg PRIVATE $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>:-Wno-shorten-64-to-32>)
target_compile_definitions(
  obs-ffmpeg
  PRIVATE
    $<$<BOOL:${ENABLE_FFMPEG_LOGGING}>:ENABLE_FFMPEG_LOGGING>
    $<$<BOOL:${ENABLE_FFMPEG_NVENC}>:ENABLE_FFMPEG_NVENC>
)

target_link_libraries(
  obs-ffmpeg
  PRIVATE
    OBS::libobs
    OBS::opts-parser
    FFmpeg::avcodec
    FFmpeg::avfilter
    FFmpeg::avformat
    FFmpeg::avdevice
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
    $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>
    $<$<PLATFORM_ID:Windows>:ws2_32>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:Libva::va>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:Libva::drm>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:Libpci::pci>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:Libdrm::Libdrm>
)

if(OS_WINDOWS)
  if(CMAKE_VS_PLATFORM_NAME STREQUAL x64)
    target_sources(obs-ffmpeg PRIVATE texture-amf-opts.hpp texture-amf.cpp)
    target_link_libraries(obs-ffmpeg PRIVATE AMF::AMF)
  endif()

  configure_file(cmake/windows/obs-module.rc.in obs-ffmpeg.rc)
  target_sources(obs-ffmpeg PRIVATE obs-ffmpeg.rc)
endif()

set_target_properties_obs(obs-ffmpeg PROPERTIES FOLDER plugins/obs-ffmpeg PREFIX "")
