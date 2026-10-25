# Harpia's application bundle. Include after set_target_properties_obs() so the
# OBS helper cannot replace the application product name or signing settings.
if(NOT OS_MACOS)
  message(FATAL_ERROR "Harpia's macOS deployment rules require a macOS build")
endif()

if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum supported macOS version" FORCE)
elseif(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 13.0)
  message(FATAL_ERROR "Harpia's native screen/audio integration requires macOS 13 or newer")
endif()
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
set(HARPIA_MAC_BUNDLE_ID "com.harpia.recorder" CACHE STRING "Stable macOS application and permission identity")
set(HARPIA_MAC_CODESIGN_IDENTITY "${OBS_CODESIGN_IDENTITY}" CACHE STRING "Signing identity for the deployed app; - means ad-hoc")
if(NOT HARPIA_MAC_CODESIGN_IDENTITY)
  set(HARPIA_MAC_CODESIGN_IDENTITY "-")
endif()

# Keep the Finder version in step with the application version on rebuilds.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Version.hpp")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Version.hpp" _harpia_version_header)
foreach(_part MAJOR MINOR PATCH)
  string(REGEX MATCH "#define HARPIA_VERSION_${_part} ([0-9]+)" _match "${_harpia_version_header}")
  if(NOT _match)
    message(FATAL_ERROR "Could not read HARPIA_VERSION_${_part} from Version.hpp")
  endif()
  set(HARPIA_VERSION_${_part} "${CMAKE_MATCH_1}")
endforeach()
set(HARPIA_MAC_VERSION "${HARPIA_VERSION_MAJOR}.${HARPIA_VERSION_MINOR}.${HARPIA_VERSION_PATCH}")

set(_harpia_mac_resources "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos")
set(_harpia_mac_icon "${CMAKE_CURRENT_BINARY_DIR}/Harpia.icns")
add_custom_command(
  OUTPUT "${_harpia_mac_icon}"
  COMMAND /bin/bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/make-macos-icon.sh"
          "${CMAKE_CURRENT_SOURCE_DIR}/resources/harpia.png" "${_harpia_mac_icon}"
  DEPENDS resources/harpia.png scripts/make-macos-icon.sh
  VERBATIM
)
set_source_files_properties("${_harpia_mac_icon}" PROPERTIES GENERATED TRUE MACOSX_PACKAGE_LOCATION Resources)
target_sources(harpia-recorder PRIVATE "${_harpia_mac_icon}")
set_target_properties(harpia-recorder PROPERTIES
  MACOSX_BUNDLE TRUE
  OUTPUT_NAME Harpia
  MACOSX_BUNDLE_BUNDLE_NAME Harpia
  MACOSX_BUNDLE_GUI_IDENTIFIER "${HARPIA_MAC_BUNDLE_ID}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${HARPIA_MAC_VERSION}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${HARPIA_MAC_VERSION}"
  MACOSX_BUNDLE_ICON_FILE Harpia.icns
  MACOSX_BUNDLE_INFO_PLIST "${_harpia_mac_resources}/Info.plist.in"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${HARPIA_MAC_BUNDLE_ID}"
  XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE NO
  # Deployment modifies load commands and signs all nested code, then the app.
  # Disable Xcode's separate app signature to avoid signing an incomplete tree.
  XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
  XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED NO
  XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/../Frameworks"
)

set(_harpia_qt_hints "${Qt6_DIR}/../../../bin" "${Qt6CoreTools_DIR}/../../../bin")
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
  list(APPEND _harpia_qt_hints "${_prefix}/bin")
endforeach()
find_program(HARPIA_MACDEPLOYQT NAMES macdeployqt HINTS ${_harpia_qt_hints} REQUIRED)

set(_harpia_deploy_args
  --app "$<TARGET_BUNDLE_DIR:harpia-recorder>"
  --macdeployqt "${HARPIA_MACDEPLOYQT}"
  --identity "${HARPIA_MAC_CODESIGN_IDENTITY}"
  --entitlements "${_harpia_mac_resources}/entitlements.plist"
  --emoji "${CMAKE_CURRENT_SOURCE_DIR}/resources/emoji/noto-emoji.pack"
  --emoji-license "${CMAKE_CURRENT_SOURCE_DIR}/resources/emoji/LICENSE-Apache-2.0.txt"
)
if(CMAKE_OSX_ARCHITECTURES)
  foreach(_arch IN LISTS CMAKE_OSX_ARCHITECTURES)
    list(APPEND _harpia_deploy_args --architecture "${_arch}")
  endforeach()
else()
  list(APPEND _harpia_deploy_args --architecture "${CMAKE_SYSTEM_PROCESSOR}")
endif()
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
  list(APPEND _harpia_deploy_args --search-dir "${_prefix}/lib")
endforeach()

# Collect runtime modules/frameworks using the same registry as OBS. Building
# a dependency does not copy it into an application; every component is staged
# here before dependency discovery and signing.
get_property(_harpia_obs_dependencies GLOBAL PROPERTY _OBS_DEPENDENCIES)
list(REMOVE_DUPLICATES _harpia_obs_dependencies)
foreach(_target IN LISTS _harpia_obs_dependencies)
  if(_target STREQUAL harpia-recorder OR NOT TARGET ${_target})
    continue()
  endif()
  get_target_property(_type ${_target} TYPE)
  if(_type STREQUAL SHARED_LIBRARY)
    get_target_property(_framework ${_target} FRAMEWORK)
    if(_framework)
      list(APPEND _harpia_deploy_args --framework "$<TARGET_BUNDLE_DIR:${_target}>")
    else()
      list(APPEND _harpia_deploy_args --library "$<TARGET_FILE:${_target}>")
    endif()
  elseif(_type STREQUAL MODULE_LIBRARY)
    list(APPEND _harpia_deploy_args --plugin "$<TARGET_BUNDLE_DIR:${_target}>")
  elseif(_type STREQUAL EXECUTABLE)
    list(APPEND _harpia_deploy_args --helper "$<TARGET_FILE:${_target}>")
  else()
    continue()
  endif()
  add_dependencies(harpia-recorder ${_target})
  target_link_options(${_target} PRIVATE "LINKER:-headerpad_max_install_names")
endforeach()
target_link_options(harpia-recorder PRIVATE "LINKER:-headerpad_max_install_names")

add_custom_command(TARGET harpia-recorder POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/deploy-macos.py" ${_harpia_deploy_args}
  COMMENT "Deploying and signing the self-contained Harpia.app"
  COMMAND_EXPAND_LISTS VERBATIM
)

# Unlike POST_BUILD, this target also runs when only a runtime plugin changed.
# The build script uses it so an incremental package never retains stale code.
add_custom_target(harpia-deploy
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/deploy-macos.py" ${_harpia_deploy_args}
  DEPENDS harpia-recorder
  COMMENT "Refreshing the deployed Harpia application"
  COMMAND_EXPAND_LISTS VERBATIM
)
