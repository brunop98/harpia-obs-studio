find_package(
  FFmpeg
  6.1
  REQUIRED avcodec avfilter avdevice avutil swscale avformat swresample
)

if(NOT TARGET OBS::opts-parser)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/opts-parser" "${CMAKE_BINARY_DIR}/shared/opts-parser")
endif()

if(OS_WINDOWS AND CMAKE_VS_PLATFORM_NAME STREQUAL x64)
  find_package(AMF 1.4.29 REQUIRED)
  add_subdirectory(obs-amf-test)
elseif(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
  find_package(Libva REQUIRED)
  find_package(Libpci REQUIRED)
  find_package(Libdrm REQUIRED)
endif()
