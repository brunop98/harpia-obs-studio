# OBS CMake Windows helper functions module

include_guard(GLOBAL)

include(helpers_common)

# set_target_properties_obs: Set target properties for libobs and its plugins
function(set_target_properties_obs target)
  set(options "")
  set(oneValueArgs "")
  set(multiValueArgs PROPERTIES)
  cmake_parse_arguments(PARSE_ARGV 0 _STPO "${options}" "${oneValueArgs}" "${multiValueArgs}")

  message(DEBUG "Setting additional properties for target ${target}...")

  while(_STPO_PROPERTIES)
    list(POP_FRONT _STPO_PROPERTIES key value)
    set_property(TARGET ${target} PROPERTY ${key} "${value}")
  endwhile()

  get_target_property(target_type ${target} TYPE)

  if(target_type STREQUAL EXECUTABLE)
    # The OBS Studio app and the browser helper were special-cased here; Harpia
    # deploys its own runtime (harpia/cmake/os-windows.cmake).
    _target_install_obs(${target} DESTINATION ${OBS_EXECUTABLE_DESTINATION})
    set_property(GLOBAL APPEND PROPERTY _OBS_EXECUTABLES ${target})
  elseif(target_type STREQUAL SHARED_LIBRARY)
    set_target_properties(${target} PROPERTIES VERSION ${OBS_VERSION_MAJOR} SOVERSION ${OBS_VERSION_CANONICAL})

    _target_install_obs(
      ${target}
        DESTINATION "${OBS_EXECUTABLE_DESTINATION}"
        LIBRARY_DESTINATION "${OBS_LIBRARY_DESTINATION}"
        HEADER_DESTINATION "${OBS_INCLUDE_DESTINATION}"
    )
  elseif(target_type STREQUAL MODULE_LIBRARY)
    set_target_properties(${target} PROPERTIES VERSION 0 SOVERSION ${OBS_VERSION_CANONICAL})

    if(target STREQUAL libobs-d3d11 OR target STREQUAL libobs-opengl OR target STREQUAL libobs-winrt)
      set(target_destination "${OBS_EXECUTABLE_DESTINATION}")
    # Scripting (obspython/obslua), graphics-hook and the virtual camera had
    # their own destinations here; none of them exists in this tree.
    else()
      set(target_destination "${OBS_PLUGIN_DESTINATION}")
    endif()

    _target_install_obs(${target} DESTINATION ${target_destination})

    set_property(GLOBAL APPEND PROPERTY OBS_MODULES_ENABLED ${target})
  endif()

  target_link_options(${target} PRIVATE "/PDBALTPATH:$<TARGET_PDB_FILE_NAME:${target}>")
  target_install_resources(${target})

  get_target_property(target_sources ${target} SOURCES)
  set(target_ui_files ${target_sources})
  list(FILTER target_ui_files INCLUDE REGEX ".+\\.(ui|qrc)")
  source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "UI Files" FILES ${target_ui_files})

  if(${target} STREQUAL libobs)
    set(target_source_files ${target_sources})
    set(target_header_files ${target_sources})
    list(FILTER target_source_files INCLUDE REGEX ".+\\.(m|c[cp]?p?|swift)")
    list(FILTER target_header_files INCLUDE REGEX ".+\\.h(pp)?")

    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source Files" FILES ${target_source_files})
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Header Files" FILES ${target_header_files})
  endif()
endfunction()

# _target_install_obs: Helper function to install build artifacts to rundir and install location
function(_target_install_obs target)
  set(options "")
  set(oneValueArgs "DESTINATION" "LIBRARY_DESTINATION" "HEADER_DESTINATION")
  set(multiValueArgs "")
  # (The x86/x64 options installed 32-bit game-capture helpers; gone.)
  cmake_parse_arguments(PARSE_ARGV 0 _TIO "${options}" "${oneValueArgs}" "${multiValueArgs}")

  set(target_file "$<TARGET_FILE:${target}>")
  set(target_pdb_file "$<TARGET_PDB_FILE:${target}>")
  set(comment "Copy ${target} to destination")

  get_target_property(target_type ${target} TYPE)
  if(target_type STREQUAL EXECUTABLE)
    install(TARGETS ${target} RUNTIME DESTINATION "${_TIO_DESTINATION}" COMPONENT Runtime)
  elseif(target_type STREQUAL SHARED_LIBRARY)
    if(NOT _TIO_LIBRARY_DESTINATION)
      set(_TIO_LIBRARY_DESTINATION ${_TIO_DESTINATION})
    endif()
    if(NOT _TIO_HEADER_DESTINATION)
      set(_TIO_HEADER_DESTINATION include)
    endif()
    install(
      TARGETS ${target}
      RUNTIME DESTINATION "${_TIO_DESTINATION}"
      LIBRARY DESTINATION "${_TIO_LIBRARY_DESTINATION}" COMPONENT Runtime EXCLUDE_FROM_ALL
      PUBLIC_HEADER DESTINATION "${_TIO_HEADER_DESTINATION}" COMPONENT Development EXCLUDE_FROM_ALL
    )
  elseif(target_type STREQUAL MODULE_LIBRARY)
    install(
      TARGETS ${target}
      LIBRARY DESTINATION "${_TIO_DESTINATION}" COMPONENT Runtime NAMELINK_COMPONENT Development
    )
  endif()

  add_custom_command(
    TARGET ${target}
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E echo "${comment}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OBS_OUTPUT_DIR}/$<CONFIG>/${_TIO_DESTINATION}"
    COMMAND "${CMAKE_COMMAND}" -E copy ${target_file} "${OBS_OUTPUT_DIR}/$<CONFIG>/${_TIO_DESTINATION}"
    COMMAND
      "${CMAKE_COMMAND}" -E $<IF:$<CONFIG:Debug,RelWithDebInfo,Release>,copy,true> ${target_pdb_file}
      "${OBS_OUTPUT_DIR}/$<CONFIG>/${_TIO_DESTINATION}"
    COMMENT ""
    VERBATIM
  )

  install(
    FILES ${target_pdb_file}
    CONFIGURATIONS RelWithDebInfo Debug Release
    DESTINATION "${_TIO_DESTINATION}"
    COMPONENT Runtime
    OPTIONAL
  )
endfunction()

# target_export: Helper function to export target as CMake package
function(target_export target)
  # Exclude CMake package from 'ALL' target
  set(exclude_variant EXCLUDE_FROM_ALL)
  _target_export(${target})

  get_target_property(target_type ${target} TYPE)
  if(NOT target_type STREQUAL INTERFACE_LIBRARY)
    install(
      FILES "$<TARGET_PDB_FILE:${target}>"
      CONFIGURATIONS RelWithDebInfo Debug Release
      DESTINATION "${OBS_EXECUTABLE_DESTINATION}"
      COMPONENT Development
      OPTIONAL
    )
  endif()
endfunction()

# Helper function to add resources into bundle
function(target_install_resources target)
  message(DEBUG "Installing resources for target ${target}...")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    file(GLOB_RECURSE data_files "${CMAKE_CURRENT_SOURCE_DIR}/data/*")
    foreach(data_file IN LISTS data_files)
      cmake_path(
        RELATIVE_PATH data_file
        BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
        OUTPUT_VARIABLE relative_path
      )
      cmake_path(GET relative_path PARENT_PATH relative_path)
      target_sources(${target} PRIVATE "${data_file}")
      source_group("Resources/${relative_path}" FILES "${data_file}")
    endforeach()

    get_property(obs_module_list GLOBAL PROPERTY OBS_MODULES_ENABLED)
    if(target IN_LIST obs_module_list)
      set(target_destination "${OBS_DATA_DESTINATION}/obs-plugins/${target}")
    else()
      set(target_destination "${OBS_DATA_DESTINATION}/${target}")
    endif()

    install(
      DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
      DESTINATION "${target_destination}"
      USE_SOURCE_PERMISSIONS
      COMPONENT Runtime
    )

    add_custom_command(
      TARGET ${target}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E echo "Copy ${target} resources to data directory"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${OBS_OUTPUT_DIR}/$<CONFIG>/${target_destination}"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data"
        "${OBS_OUTPUT_DIR}/$<CONFIG>/${target_destination}"
      COMMENT ""
      VERBATIM
    )
  endif()
endfunction()

