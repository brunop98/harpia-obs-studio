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
# of libobs and a minimal plugin set. The stock OBS Studio frontend, the
# Lua/Python scripting support (SWIG bindings, obs-scripting) and the streaming
# stack have been removed.
# HEVC stays ON: the recorder offers H.265 as a selectable per-preset codec.
option(ENABLE_HEVC "Enable HEVC encoders" ON)
option(ENABLE_HARPIA "Enable building the Harpia recorder (minimal screen recorder on libobs)" ON)

add_subdirectory(libobs)
if(OS_WINDOWS)
  add_subdirectory(libobs-d3d11)
  add_subdirectory(libobs-winrt)
endif()
# macOS renders through libobs-opengl too (harpia/core/ObsContext.cpp); the
# Swift Metal renderer upstream ships for macOS is gone.
add_subdirectory(libobs-opengl)
add_subdirectory(plugins)

if(ENABLE_HARPIA)
  add_subdirectory(harpia)
endif()

message_configuration()
