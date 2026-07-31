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

if(OBS_PARENT_ARCHITECTURE STREQUAL CMAKE_VS_PLATFORM_NAME)
  # Upstream configures and builds this entire tree a SECOND time here, for the
  # other bitness, purely so the game-capture helpers (graphics-hook,
  # get-graphics-offsets, inject-helper) exist in both: they are injected into
  # the captured process, so they have to match its architecture.
  #
  # Harpia does not do game capture -- CaptureManager asks for monitor_capture
  # and window_capture -- and those three are gone, so nothing is left that
  # needs a second architecture. The only other user of the mechanism,
  # win-dshow's obs-virtualcam-module, is not part of this build at all (nothing
  # adds its directory). Spawning the child configure would build libobs again
  # into a directory no target then reads.
  #
  # The branch below still exists for a child build, and the guards in the root
  # CMakeLists and libobs still honour OBS_PARENT_ARCHITECTURE, so reinstating
  # one is a matter of putting the execute_process calls back.
else()
  # target_disable_feature: Stub macro for child architecture builds
  macro(target_disable_feature)
  endmacro()

  # target_disable: Stub macro for child architecture builds
  macro(target_disable)
  endmacro()

  # target_add_resource: Stub macro for child architecture builds
  macro(target_add_resource)
  endmacro()

  # target_export: Stub macro for child architecture builds
  macro(target_export)
  endmacro()

  # set_target_properties_obs: Stub macro for child architecture builds
  macro(set_target_properties_obs)
    set_target_properties(${ARGV})
  endmacro()

  # check_uuid: Helper function to check for valid UUID
  function(check_uuid uuid_string return_value)
    set(valid_uuid TRUE)
    # gersemi: off
    set(uuid_token_lengths 8 4 4 4 12)
    # gersemi: on
    set(token_num 0)

    string(REPLACE "-" ";" uuid_tokens ${uuid_string})
    list(LENGTH uuid_tokens uuid_num_tokens)

    if(uuid_num_tokens EQUAL 5)
      message(DEBUG "UUID ${uuid_string} is valid with 5 tokens.")
      foreach(uuid_token IN LISTS uuid_tokens)
        list(GET uuid_token_lengths ${token_num} uuid_target_length)
        string(LENGTH "${uuid_token}" uuid_actual_length)
        if(uuid_actual_length EQUAL uuid_target_length)
          string(REGEX MATCH "[0-9a-fA-F]+" uuid_hex_match ${uuid_token})
          if(NOT uuid_hex_match STREQUAL uuid_token)
            set(valid_uuid FALSE)
            break()
          endif()
        else()
          set(valid_uuid FALSE)
          break()
        endif()
        math(EXPR token_num "${token_num}+1")
      endforeach()
    else()
      set(valid_uuid FALSE)
    endif()
    message(DEBUG "UUID ${uuid_string} valid: ${valid_uuid}")
    set(${return_value} ${valid_uuid} PARENT_SCOPE)
  endfunction()

  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/buildspec.cmake")

  # Only what a second-architecture build has to produce. The three
  # game-capture helpers that used to be listed here no longer exist.
  add_subdirectory(libobs)

  return()
endif()
