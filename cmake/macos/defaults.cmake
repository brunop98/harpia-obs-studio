# OBS CMake macOS defaults module

include_guard(GLOBAL)

# Set empty codesigning team if not specified as cache variable
if(NOT OBS_CODESIGN_TEAM)
  set(OBS_CODESIGN_TEAM "" CACHE STRING "OBS code signing team for macOS" FORCE)

  # Set ad-hoc codesigning identity if not specified as cache variable
  if(NOT OBS_CODESIGN_IDENTITY)
    set(OBS_CODESIGN_IDENTITY "-" CACHE STRING "OBS code signing identity for macOS" FORCE)
  endif()
endif()

include(xcode)

include(buildspec)

# Enable find_package targets to become globally available targets
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
# Enable RPATH support for generated binaries
set(CMAKE_MACOSX_RPATH TRUE)
# Use RPATHs from build tree _in_ the build tree
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
# Do not add default linker search paths to RPATH
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
# Use common bundle-relative RPATH for installed targets
set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks")
# Ignore any dependent packages installed via Homebrew
list(APPEND CMAKE_IGNORE_PREFIX_PATH "/opt/homebrew" "/usr/local")

# Used for library exports only (obs-frontend-api)
set(OBS_LIBRARY_DESTINATION "lib")
set(OBS_INCLUDE_DESTINATION "include/obs")
set(OBS_CMAKE_DESTINATION "lib/cmake")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/resources/package.applescript"
  "${CMAKE_CURRENT_BINARY_DIR}/package.applescript"
  @ONLY
)
