# OBS CMake macOS helper functions module

include_guard(GLOBAL)

include(helpers_common)

# set_target_xcode_properties: Sets Xcode-specific target attributes
function(set_target_xcode_properties target)
  set(options "")
  set(oneValueArgs "")
  set(multiValueArgs PROPERTIES)
  cmake_parse_arguments(PARSE_ARGV 0 _STXP "${options}" "${oneValueArgs}" "${multiValueArgs}")

  message(DEBUG "Setting Xcode properties for target ${target}...")

  while(_STXP_PROPERTIES)
    list(POP_FRONT _STXP_PROPERTIES key value)
    set_property(TARGET ${target} PROPERTY XCODE_ATTRIBUTE_${key} "${value}")
  endwhile()
endfunction()

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

  string(TIMESTAMP CURRENT_YEAR "%Y")

  # Target is a GUI or CLI application
  if(target_type STREQUAL EXECUTABLE)
    # The OBS Studio app bundle (Sparkle, Syphon, the DAL plugin, the camera
    # extension and virtual-camera entitlements) was set up here; none of it
    # exists in this tree.
    if(${target} STREQUAL obs-ffmpeg-mux)
      if(OBS_CODESIGN_IDENTITY STREQUAL "-")
        set_target_xcode_properties(${target} PROPERTIES ENABLE_HARDENED_RUNTIME NO)
      endif()

      set_target_xcode_properties(${target} PROPERTIES SKIP_INSTALL NO)
      set_property(GLOBAL APPEND PROPERTY _OBS_EXECUTABLES ${target})
      set_property(GLOBAL APPEND PROPERTY _OBS_DEPENDENCIES ${target})
    else()
      set_property(TARGET ${target} PROPERTY XCODE_ATTRIBUTE_SKIP_INSTALL NO)
      set_property(GLOBAL APPEND PROPERTY _OBS_EXECUTABLES ${target})
      set_property(GLOBAL APPEND PROPERTY _OBS_DEPENDENCIES ${target})
      _add_entitlements()
    endif()
  elseif(target_type STREQUAL SHARED_LIBRARY)
    set_target_properties(
      ${target}
      PROPERTIES
        NO_SONAME TRUE
        MACHO_COMPATIBILITY_VERSION 1.0
        MACHO_CURRENT_VERSION ${OBS_VERSION_MAJOR}
        SOVERSION 0
        VERSION 0
    )

    set_target_xcode_properties(
      ${target}
      PROPERTIES DYLIB_COMPATIBILITY_VERSION 1.0
                 DYLIB_CURRENT_VERSION ${OBS_VERSION_MAJOR}
                 PRODUCT_NAME ${target}
                 PRODUCT_BUNDLE_IDENTIFIER com.obsproject.${target}
                 SKIP_INSTALL YES
    )

    get_target_property(is_framework ${target} FRAMEWORK)
    if(is_framework)
      set_target_properties(
        ${target}
        PROPERTIES FRAMEWORK_VERSION A MACOSX_FRAMEWORK_IDENTIFIER com.obsproject.${target}
      )

      set_target_xcode_properties(
        ${target}
        PROPERTIES CODE_SIGN_IDENTITY ""
                   DEVELOPMENT_TEAM ""
                   SKIP_INSTALL YES
                   PRODUCT_NAME ${target}
                   PRODUCT_BUNDLE_IDENTIFIER com.obsproject.${target}
                   CURRENT_PROJECT_VERSION ${OBS_BUILD_NUMBER}
                   MARKETING_VERSION ${OBS_VERSION_CANONICAL}
                   GENERATE_INFOPLIST_FILE YES
                   INFOPLIST_FILE ""
                   INFOPLIST_KEY_CFBundleDisplayName ${target}
                   INFOPLIST_KEY_NSHumanReadableCopyright "(c) 2012-${CURRENT_YEAR} Lain Bailey"
      )
    endif()

    set_property(GLOBAL APPEND PROPERTY _OBS_FRAMEWORKS ${target})
    set_property(GLOBAL APPEND PROPERTY _OBS_DEPENDENCIES ${target})
  elseif(target_type STREQUAL MODULE_LIBRARY)
    # Scripting (obspython/obslua) and the DAL plugin had branches here.
    set_target_properties(${target} PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION plugin)

    set_target_xcode_properties(
      ${target}
      PROPERTIES PRODUCT_NAME ${target}
                 PRODUCT_BUNDLE_IDENTIFIER com.obsproject.${target}
                 CURRENT_PROJECT_VERSION ${OBS_BUILD_NUMBER}
                 MARKETING_VERSION ${OBS_VERSION_CANONICAL}
                 GENERATE_INFOPLIST_FILE YES
                 INFOPLIST_KEY_CFBundleDisplayName ${target}
                 INFOPLIST_KEY_NSHumanReadableCopyright "(c) 2012-${CURRENT_YEAR} Lain Bailey"
    )

    set_property(GLOBAL APPEND PROPERTY OBS_MODULES_ENABLED ${target})
    set_property(GLOBAL APPEND PROPERTY _OBS_DEPENDENCIES ${target})
  endif()

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

# _add_entitlements: Macro to add entitlements shipped with project
macro(_add_entitlements)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/entitlements.plist")
    set_target_xcode_properties(
      ${target}
      PROPERTIES CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/entitlements.plist"
    )
  endif()
endmacro()

# target_export: Helper function to export target as CMake package
function(target_export target)
  # Exclude CMake package from 'ALL' target
  set(exclude_variant EXCLUDE_FROM_ALL)
  _target_export(${target})
endfunction()

# target_install_resources: Helper function to add resources into bundle
function(target_install_resources target)
  message(DEBUG "Installing resources for target ${target}...")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    file(GLOB_RECURSE data_files "${CMAKE_CURRENT_SOURCE_DIR}/data/*")
    list(FILTER data_files EXCLUDE REGEX "\\.DS_Store$")
    foreach(data_file IN LISTS data_files)
      cmake_path(
        RELATIVE_PATH data_file
        BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
        OUTPUT_VARIABLE relative_path
      )
      cmake_path(GET relative_path PARENT_PATH relative_path)
      target_sources(${target} PRIVATE "${data_file}")
      set_property(SOURCE "${data_file}" PROPERTY MACOSX_PACKAGE_LOCATION "Resources/${relative_path}")
      source_group("Resources/${relative_path}" FILES "${data_file}")
    endforeach()
  endif()
endfunction()

