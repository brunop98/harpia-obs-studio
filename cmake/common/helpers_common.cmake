# OBS CMake common helper functions module

include_guard(GLOBAL)

# message_configuration: Function to print configuration outcome
function(message_configuration)
  include(FeatureSummary)
  feature_summary(WHAT ALL VAR _feature_summary)

  message(DEBUG "${_feature_summary}")

  message(
    NOTICE
    "                      _                   _             _ _       \n"
    "                 ___ | |__  ___       ___| |_ _   _  __| (_) ___  \n"
    "                / _ \\| '_ \\/ __|_____/ __| __| | | |/ _` | |/ _ \\ \n"
    "               | (_) | |_) \\__ \\_____\\__ \\ |_| |_| | (_| | | (_) |\n"
    "                \\___/|_.__/|___/     |___/\\__|\\__,_|\\__,_|_|\\___/ \n"
    "\nOBS:  Application Version: ${OBS_VERSION} - Build Number: ${OBS_BUILD_NUMBER}\n"
    "==================================================================================\n\n"
  )

  get_property(OBS_FEATURES_ENABLED GLOBAL PROPERTY OBS_FEATURES_ENABLED)
  list(SORT OBS_FEATURES_ENABLED COMPARE NATURAL CASE SENSITIVE ORDER ASCENDING)

  if(OBS_FEATURES_ENABLED)
    message(NOTICE "------------------------       Enabled Features           ------------------------")
    foreach(feature IN LISTS OBS_FEATURES_ENABLED)
      message(NOTICE " - ${feature}")
    endforeach()
  endif()

  get_property(OBS_FEATURES_DISABLED GLOBAL PROPERTY OBS_FEATURES_DISABLED)
  list(SORT OBS_FEATURES_DISABLED COMPARE NATURAL CASE SENSITIVE ORDER ASCENDING)

  if(OBS_FEATURES_DISABLED)
    message(NOTICE "------------------------       Disabled Features          ------------------------")
    foreach(feature IN LISTS OBS_FEATURES_DISABLED)
      message(NOTICE " - ${feature}")
    endforeach()
  endif()

  if(ENABLE_PLUGINS)
    get_property(OBS_MODULES_ENABLED GLOBAL PROPERTY OBS_MODULES_ENABLED)
    list(SORT OBS_MODULES_ENABLED COMPARE NATURAL CASE SENSITIVE ORDER ASCENDING)

    if(OBS_MODULES_ENABLED)
      message(NOTICE "------------------------        Enabled Modules           ------------------------")
      foreach(feature IN LISTS OBS_MODULES_ENABLED)
        message(NOTICE " - ${feature}")
      endforeach()
    endif()

    get_property(OBS_MODULES_DISABLED GLOBAL PROPERTY OBS_MODULES_DISABLED)
    list(SORT OBS_MODULES_DISABLED COMPARE NATURAL CASE SENSITIVE ORDER ASCENDING)

    if(OBS_MODULES_DISABLED)
      message(NOTICE "------------------------        Disabled Modules          ------------------------")
      foreach(feature IN LISTS OBS_MODULES_DISABLED)
        message(NOTICE " - ${feature}")
      endforeach()
    endif()
  endif()
  message(NOTICE "----------------------------------------------------------------------------------")
endfunction()

# target_enable_feature: Adds feature to list of enabled application features and sets optional compile definitions
function(target_enable_feature target feature_description)
  set_property(GLOBAL APPEND PROPERTY OBS_FEATURES_ENABLED "${feature_description}")

  if(ARGN)
    target_compile_definitions(${target} PRIVATE ${ARGN})
  endif()
endfunction()

# target_disable_feature: Adds feature to list of disabled application features and sets optional compile definitions
function(target_disable_feature target feature_description)
  set_property(GLOBAL APPEND PROPERTY OBS_FEATURES_DISABLED "${feature_description}")

  if(ARGN)
    target_compile_definitions(${target} PRIVATE ${ARGN})
  endif()
endfunction()

# target_disable: Adds target to list of disabled modules
function(target_disable target)
  set_property(GLOBAL APPEND PROPERTY OBS_MODULES_DISABLED ${target})
endfunction()

# target_export: Helper function to export target as CMake package
function(target_export target)
  if(NOT DEFINED exclude_variant)
    set(exclude_variant EXCLUDE_FROM_ALL)
  endif()

  get_target_property(is_framework ${target} FRAMEWORK)
  if(is_framework)
    set(package_destination "Frameworks/${target}.framework/Resources/cmake")
    set(include_destination "Frameworks/${target}.framework/Headers")
  else()
    if(OS_WINDOWS)
      set(package_destination "${OBS_CMAKE_DESTINATION}")
    else()
      set(package_destination "${OBS_CMAKE_DESTINATION}/${target}")
    endif()
    set(include_destination "${OBS_INCLUDE_DESTINATION}")
  endif()

  install(
    TARGETS ${target}
    EXPORT ${target}Targets
    RUNTIME DESTINATION "${OBS_EXECUTABLE_DESTINATION}" COMPONENT Development
    ${exclude_variant}
    LIBRARY DESTINATION "${OBS_LIBRARY_DESTINATION}" COMPONENT Development
    ${exclude_variant}
    ARCHIVE DESTINATION "${OBS_LIBRARY_DESTINATION}" COMPONENT Development
    ${exclude_variant}
    FRAMEWORK DESTINATION Frameworks COMPONENT Development
    ${exclude_variant}
    INCLUDES DESTINATION "${include_destination}"
    PUBLIC_HEADER DESTINATION "${include_destination}" COMPONENT Development
    ${exclude_variant}
  )

  get_target_property(obs_public_headers ${target} OBS_PUBLIC_HEADERS)

  if(obs_public_headers)
    foreach(header IN LISTS obs_public_headers)
      cmake_path(GET header PARENT_PATH header_dir)
      if(header_dir)
        if(NOT ${header_dir} IN_LIST header_dirs)
          list(APPEND header_dirs ${header_dir})
        endif()
        list(APPEND headers_${header_dir} ${header})
      else()
        list(APPEND headers ${header})
      endif()
    endforeach()

    foreach(header_dir IN LISTS header_dirs)
      install(
        FILES ${headers_${header_dir}}
        DESTINATION "${include_destination}/${header_dir}"
        COMPONENT Development
        ${exclude_variant}
      )
    endforeach()

    if(headers)
      install(FILES ${headers} DESTINATION "${include_destination}" COMPONENT Development ${exclude_variant})
    endif()
  endif()

  if(target STREQUAL libobs AND NOT EXISTS "${include_destination}/obsconfig.h")
    install(
      FILES "${CMAKE_BINARY_DIR}/config/obsconfig.h"
      DESTINATION "${include_destination}"
      COMPONENT Development
      ${exclude_variant}
    )
  endif()

  get_target_property(target_type ${target} TYPE)

  if(NOT target_type STREQUAL INTERFACE_LIBRARY)
    message(DEBUG "Generating export header for target ${target} as ${target}_EXPORT.h...")
    include(GenerateExportHeader)
    generate_export_header(${target} EXPORT_FILE_NAME "${target}_EXPORT.h")
    target_sources(${target} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/${target}_EXPORT.h>)

    set_property(TARGET ${target} APPEND PROPERTY PUBLIC_HEADER "${target}_EXPORT.h")
  endif()

  set(TARGETS_EXPORT_NAME ${target}Targets)
  message(
    DEBUG
    "Generating CMake package configuration file ${target}Config.cmake with targets file ${TARGETS_EXPORT_NAME}..."
  )
  include(CMakePackageConfigHelpers)
  configure_package_config_file(
    cmake/${target}Config.cmake.in
    ${target}Config.cmake
    INSTALL_DESTINATION "${package_destination}"
  )

  message(DEBUG "Generating CMake package version configuration file ${target}ConfigVersion.cmake...")
  write_basic_package_version_file(
    "${target}ConfigVersion.cmake"
    VERSION ${OBS_VERSION_CANONICAL}
    COMPATIBILITY SameMajorVersion
  )

  export(EXPORT ${target}Targets FILE "${TARGETS_EXPORT_NAME}.cmake" NAMESPACE OBS::)

  export(PACKAGE ${target})

  install(
    EXPORT ${TARGETS_EXPORT_NAME}
    FILE ${TARGETS_EXPORT_NAME}.cmake
    NAMESPACE OBS::
    DESTINATION "${package_destination}"
    COMPONENT Development
    ${exclude_variant}
  )

  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/${target}Config.cmake" "${CMAKE_CURRENT_BINARY_DIR}/${target}ConfigVersion.cmake"
    DESTINATION "${package_destination}"
    COMPONENT Development
    ${exclude_variant}
  )

  if(target STREQUAL libobs)
    install(
      FILES "${CMAKE_SOURCE_DIR}/cmake/finders/FindSIMDe.cmake"
      DESTINATION "${package_destination}/finders"
      COMPONENT Development
      ${exclude_variant}
    )
  endif()
endfunction()

# add_obs_plugin: Add plugin subdirectory if host platform is in specified list of supported platforms and architectures
function(add_obs_plugin target)
  set(options WITH_MESSAGE)
  set(oneValueArgs "")
  set(multiValueArgs PLATFORMS ARCHITECTURES)
  cmake_parse_arguments(PARSE_ARGV 0 _AOP "${options}" "${oneValueArgs}" "${multiValueArgs}")

  set(found_platform FALSE)
  list(LENGTH _AOP_PLATFORMS _AOP_NUM_PLATFORMS)

  set(found_architecture FALSE)
  list(LENGTH _AOP_ARCHITECTURES _AOP_NUM_ARCHITECTURES)

  if(_AOP_NUM_PLATFORMS EQUAL 0)
    set(found_platform TRUE)
  else()
    foreach(platform IN LISTS _AOP_PLATFORMS)
      set(check_var_name "OS_${platform}")
      if(${${check_var_name}})
        set(found_platform TRUE)
        break()
      endif()
    endforeach()
  endif()

  if(_AOP_NUM_ARCHITECTURES EQUAL 0)
    set(found_architecture TRUE)
  else()
    foreach(architecture IN LISTS _AOP_ARCHITECTURES)
      if(OS_WINDOWS)
        if("${architecture}" STREQUAL CMAKE_VS_PLATFORM_NAME)
          set(found_architecture TRUE)
        endif()
      elseif(OS_MACOS)
        if(
          "${architecture}" IN_LIST CMAKE_OSX_ARCHITECTURES
          OR "${architecture}" STREQUAL "${CMAKE_HOST_SYSTEM_PROCESSOR}"
        )
          set(found_architecture TRUE)
        endif()
      elseif("${architecture}" STREQUAL CMAKE_SYSTEM_PROCESSOR)
        set(found_architecture TRUE)
      endif()
    endforeach()
  endif()

  if(found_platform AND found_architecture)
    add_subdirectory(${target})
  elseif(_AOP_WITH_MESSAGE)
    add_custom_target(${target} COMMENT "Dummy target for unavailable module ${target}")
    target_disable(${target})
  endif()
endfunction()
