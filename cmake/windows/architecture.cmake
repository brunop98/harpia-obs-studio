# OBS CMake Windows Architecture Helper

include_guard(GLOBAL)

include(compilerconfig)

if(NOT DEFINED OBS_PARENT_ARCHITECTURE)
  if(CMAKE_VS_PLATFORM_NAME MATCHES "(Win32|x64|ARM64)")
    set(OBS_PARENT_ARCHITECTURE ${CMAKE_VS_PLATFORM_NAME})
  else()
    message(FATAL_ERROR "Unsupported generator platform for Windows builds: ${CMAKE_VS_PLATFORM_NAME}!")
  endif()
endif()

# Upstream configured and built this whole tree a second time here, for the
# other bitness, so the game-capture helpers (injected into the captured
# process) existed in both. Game capture and the virtual camera are gone, so
# Harpia builds one architecture and there is no child build to stub for.
