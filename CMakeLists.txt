cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(obs-studio VERSION ${OBS_VERSION_CANONICAL})

if(CMAKE_HOST_SYSTEM_NAME MATCHES "Windows")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/architecture.cmake")
  if(NOT OBS_PARENT_ARCHITECTURE STREQUAL CMAKE_VS_PLATFORM_NAME)
    return()
  endif()
endif()

include(compilerconfig)
include(defaults)
include(helpers)

# This tree is slimmed down to build only the Harpia recorder (harpia/) on top
# of libobs and a minimal plugin set. The stock OBS Studio frontend, scripting,
# and streaming stack have been removed; the options below default off.
option(ENABLE_SCRIPTING "Enable scripting support" OFF)
# HEVC stays ON: the recorder offers H.265 as a selectable per-preset codec.
option(ENABLE_HEVC "Enable HEVC encoders" ON)
option(ENABLE_HARPIA "Enable building the Harpia recorder (minimal screen recorder on libobs)" ON)

add_subdirectory(libobs)
if(OS_WINDOWS)
  add_subdirectory(libobs-d3d11)
  add_subdirectory(libobs-winrt)
endif()
add_subdirectory(libobs-opengl)
if(OS_MACOS)
  add_subdirectory(libobs-metal)
endif()
add_subdirectory(plugins)

if(ENABLE_HARPIA)
  add_subdirectory(harpia)
endif()

message_configuration()
