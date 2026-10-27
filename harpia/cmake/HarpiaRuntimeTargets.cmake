# Required modules and helper processes are loaded at runtime rather than
# linked into the recorder. Keep explicit build dependencies so a targeted
# harpia-recorder build also populates its runtime folder on a clean checkout.
include_guard(GLOBAL)

function(harpia_add_runtime_dependencies target)
  set(_harpia_runtime_targets
      # The graphics backend is itself a module, not a libobs link dependency.
      # Without it the video engine fails with OBS_VIDEO_MODULE_NOT_FOUND (-5).
      libobs-d3d11
      libobs-opengl
      # The ffmpeg muxer starts this helper process to write recordings.
      obs-ffmpeg
      obs-ffmpeg-mux
      obs-filters
      obs-x264
      obs-nvenc
      obs-qsv11
      # Hardware encoders probe their device using these helper executables.
      obs-nvenc-test
      obs-amf-test
      obs-qsv-test
      win-capture
      win-wasapi
      win-dshow
      mac-capture
      mac-avcapture
      linux-capture
      linux-pipewire
      linux-pulseaudio
      linux-v4l2
  )
  foreach(_dep IN LISTS _harpia_runtime_targets)
    if(TARGET ${_dep})
      add_dependencies(${target} ${_dep})
    endif()
  endforeach()
endfunction()

# A renderer's own POST_BUILD copy does not run again if only the staged DLL
# is deleted. On Windows run this inexpensive copy whenever the recorder is
# built, including when its executable and renderer are already up to date.
# Other platforms keep their existing bundle/install layout.
function(harpia_stage_graphics_runtime target directory)
  set(_graphics_targets "")
  set(_commands COMMAND "${CMAKE_COMMAND}" -E make_directory "${directory}")
  foreach(_renderer libobs-d3d11 libobs-opengl)
    if(TARGET ${_renderer})
      list(APPEND _graphics_targets ${_renderer})
      list(APPEND _commands COMMAND "${CMAKE_COMMAND}" -E copy_if_different
           "$<TARGET_FILE:${_renderer}>" "${directory}")
    endif()
  endforeach()
  if(_graphics_targets)
    add_custom_target(
      ${target}-graphics-runtime
      ${_commands}
      DEPENDS ${_graphics_targets}
      COMMENT "Staging Harpia graphics modules"
      VERBATIM
    )
    add_dependencies(${target} ${target}-graphics-runtime)
  endif()
endfunction()
