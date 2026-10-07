# OBS CMake Linux helper functions module

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
  set(OBS_SOVERSION 30)

  if(target_type STREQUAL EXECUTABLE)
    install(TARGETS ${target} RUNTIME DESTINATION "${OBS_EXECUTABLE_DESTINATION}" COMPONENT Runtime)

    add_custom_command(
      TARGET ${target}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${target}>"
        "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
      COMMENT "Copy ${target} to binary directory"
      VERBATIM
    )

    set_property(GLOBAL APPEND PROPERTY _OBS_EXECUTABLES ${target})

    set_target_properties(
      ${target}
      PROPERTIES
        BUILD_RPATH "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_LIBRARY_DESTINATION}"
        INSTALL_RPATH "${OBS_EXECUTABLE_RPATH}"
    )
  elseif(target_type STREQUAL SHARED_LIBRARY)
    set_target_properties(
      ${target}
      PROPERTIES
        VERSION ${OBS_SOVERSION}
        SOVERSION ${OBS_SOVERSION}
        BUILD_RPATH "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_LIBRARY_DESTINATION}"
        INSTALL_RPATH "${OBS_LIBRARY_RPATH}"
    )

    install(
      TARGETS ${target}
      LIBRARY DESTINATION "${OBS_LIBRARY_DESTINATION}" COMPONENT Runtime
      PUBLIC_HEADER DESTINATION "${OBS_INCLUDE_DESTINATION}" COMPONENT Development EXCLUDE_FROM_ALL
    )

    add_custom_command(
      TARGET ${target}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_LIBRARY_DESTINATION}"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${target}>"
        "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_LIBRARY_DESTINATION}/"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_SONAME_FILE:${target}>"
        "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_LIBRARY_DESTINATION}/"
      COMMENT "Copy ${target} to library directory (${OBS_LIBRARY_DESTINATION})"
      VERBATIM
    )

    if(target STREQUAL libobs)
      install(
        FILES "$<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_PREFIX:${target}>$<TARGET_FILE_BASE_NAME:${target}>.so.0"
        DESTINATION "${OBS_LIBRARY_DESTINATION}"
      )

      add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND
          "${CMAKE_COMMAND}" -E create_symlink
          "$<TARGET_FILE_PREFIX:${target}>$<TARGET_FILE_BASE_NAME:${target}>.so.${OBS_SOVERSION}"
          "$<TARGET_FILE_PREFIX:${target}>$<TARGET_FILE_BASE_NAME:${target}>.so.0"
        COMMAND
          "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_PREFIX:${target}>$<TARGET_FILE_BASE_NAME:${target}>.so.0"
          "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_LIBRARY_DESTINATION}"
        COMMENT "Create symlink for legacy ${target}"
      )
    endif()
  elseif(target_type STREQUAL MODULE_LIBRARY)
    set_target_properties(
      ${target}
      PROPERTIES
        VERSION 0
        SOVERSION ${OBS_SOVERSION}
        BUILD_RPATH "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_LIBRARY_DESTINATION}"
        INSTALL_RPATH "${OBS_MODULE_RPATH}"
    )

    set(plugin_destination "${OBS_PLUGIN_DESTINATION}")

    install(
      TARGETS ${target}
      LIBRARY DESTINATION "${plugin_destination}" COMPONENT Runtime NAMELINK_COMPONENT Development
    )

    add_custom_command(
      TARGET ${target}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${OBS_OUTPUT_DIR}/$<CONFIG>/${plugin_destination}"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${target}>"
        "${OBS_OUTPUT_DIR}/$<CONFIG>/${plugin_destination}"
      COMMENT "Copy ${target} to plugin directory (${plugin_destination})"
      VERBATIM
    )

    set_property(GLOBAL APPEND PROPERTY OBS_MODULES_ENABLED ${target})
  endif()

  target_install_resources(${target})
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
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${OBS_OUTPUT_DIR}/$<CONFIG>/${target_destination}"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data"
        "${OBS_OUTPUT_DIR}/$<CONFIG>/${target_destination}"
      COMMENT "Copy ${target} resources to data directory (${target_destination})"
      VERBATIM
    )
  endif()
endfunction()

# target_export: Helper function to export target as CMake package
function(target_export target)
  set(exclude_variant "")
  _target_export(${target})

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux/${target}.pc.in")
    message(DEBUG "Generating pkgconfig file ${target}.pc.in...")

    install(CODE "set(OBS_VERSION_CANONICAL ${OBS_VERSION_CANONICAL})" COMPONENT Development)
    install(CODE "set(CMAKE_C_STANDARD ${CMAKE_C_STANDARD})" COMPONENT Development)
    install(
      CODE
        "configure_file(\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux/${target}.pc.in\" \"${CMAKE_CURRENT_BINARY_DIR}/${target}.pc\" @ONLY)"
      COMPONENT Development
    )

    install(
      FILES "${CMAKE_CURRENT_BINARY_DIR}/${target}.pc"
      DESTINATION "${OBS_LIBRARY_DESTINATION}/pkgconfig"
      COMPONENT Development
    )
  endif()
endfunction()
