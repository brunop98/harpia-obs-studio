cmake_minimum_required(VERSION 3.28...3.30)

find_package(FFmpeg REQUIRED avcodec avutil)

add_library(win-dshow MODULE)
add_library(OBS::dshow ALIAS win-dshow)

if(NOT TARGET OBS::tiny-nv12-scale)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/obs-tiny-nv12-scale" obs-tiny-nv12-scale)
endif()

if(NOT TARGET OBS::shared-memory-queue)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/obs-shared-memory-queue" obs-shared-memory-queue)
endif()

if(NOT TARGET OBS::libdshowcapture)
  add_subdirectory("${CMAKE_SOURCE_DIR}/deps/libdshowcapture" libdshowcapture)
endif()

if(NOT TARGET OBS::winhandle)
  add_subdirectory("${CMAKE_SOURCE_DIR}/libobs" "${CMAKE_BINARY_DIR}/libobs")
endif()

target_sources(
  win-dshow
  PRIVATE dshow-plugin.cpp encode-dstr.hpp ffmpeg-decode.c ffmpeg-decode.h win-dshow-encoder.cpp win-dshow.cpp
)

configure_file(cmake/windows/obs-module.rc.in win-dshow.rc)
target_sources(win-dshow PRIVATE win-dshow.rc)

target_link_libraries(
  win-dshow
  PRIVATE
    OBS::libobs
    OBS::w32-pthreads
    OBS::libdshowcapture
    OBS::tiny-nv12-scale
    OBS::shared-memory-queue
    OBS::winhandle
    FFmpeg::avcodec
    FFmpeg::avutil
    strmiids
    winmm
)

# Harpia uses win-dshow for webcam CAPTURE only. The virtual-camera output and
# its module are gone: nothing added the virtualcam-module directory, so the
# targets this used to test for could never exist and the whole block was
# unreachable -- a guard around code that had already been removed from the
# build in every way except the file still being there.

set_target_properties_obs(win-dshow PROPERTIES FOLDER plugins/win-dshow PREFIX "")
