# Windows-specific configuration for the Harpia recorder.
#
# GetLastInputInfo (idle detection) lives in user32; it is part of the default
# import set, but we link it explicitly for clarity.

target_link_libraries(harpia-recorder PRIVATE user32)

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
  COMMAND
    "${CMAKE_COMMAND}" -E copy_directory "$<TARGET_FILE_DIR:Qt6::Core>/../plugins/platforms"
    "${_harpia_runtime_dir}/platforms"
  VERBATIM
)
