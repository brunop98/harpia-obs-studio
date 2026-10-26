cmake_minimum_required(VERSION 3.28)

# Build only the pieces deliberately omitted from OBS's Qt package. This is a
# separate CMake project: none of Qt's internal build settings enter Harpia,
# and QtCore/Gui/Widgets continue to come from the selected, matching SDK.
if(NOT HARPIA_QT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "HARPIA_QT_VERSION must name the selected Qt SDK's full version")
endif()
if(NOT EXISTS "${HARPIA_QTBASE_SOURCE_DIR}/.cmake.conf")
  message(FATAL_ERROR "HARPIA_QTBASE_SOURCE_DIR must contain the matching complete qtbase source archive")
endif()
file(REAL_PATH "${HARPIA_QTBASE_SOURCE_DIR}" HARPIA_QTBASE_SOURCE_DIR)
include("${HARPIA_QTBASE_SOURCE_DIR}/.cmake.conf")
if(NOT QT_REPO_MODULE_VERSION VERSION_EQUAL HARPIA_QT_VERSION)
  message(FATAL_ERROR
    "Qt source version ${QT_REPO_MODULE_VERSION} does not match SDK version ${HARPIA_QT_VERSION}")
endif()

project(HarpiaQtTools VERSION "${HARPIA_QT_VERSION}"
  DESCRIPTION "Missing build and test components for Harpia's matching Qt SDK" LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(HARPIA_QT_BUILD_MACDEPLOYQT "Build the matching macOS deployment tool" OFF)
option(HARPIA_QT_BUILD_TESTLIB "Build the matching QtTest library" OFF)
if(NOT HARPIA_QT_BUILD_MACDEPLOYQT AND NOT HARPIA_QT_BUILD_TESTLIB)
  message(FATAL_ERROR "Choose at least one missing Qt component to build")
endif()
if(HARPIA_QT_BUILD_MACDEPLOYQT AND NOT APPLE)
  message(FATAL_ERROR "macdeployqt must be built on macOS")
endif()
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  message(FATAL_ERROR "Set a separate CMAKE_INSTALL_PREFIX for the missing Qt components")
endif()

# Never let QtBuildInternals reset the install prefix to the original SDK.
set(QT_BUILD_INTERNALS_NO_FORCE_SET_INSTALL_PREFIX ON)
set(QT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(QT_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(QT_BUILD_MANUAL_TESTS OFF CACHE BOOL "" FORCE)
set(QT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QT_BUILD_DOC_SNIPPETS OFF CACHE BOOL "" FORCE)
set(QT_BUILD_TOOLS_BY_DEFAULT "${HARPIA_QT_BUILD_MACDEPLOYQT}" CACHE BOOL "" FORCE)
if(APPLE)
  enable_language(OBJCXX)
endif()
find_package(Qt6 "${HARPIA_QT_VERSION}" EXACT REQUIRED COMPONENTS BuildInternals Core Gui Widgets Network)
if(QT6_INSTALL_PREFIX)
  file(REAL_PATH "${QT6_INSTALL_PREFIX}" _sdk_prefix)
  file(MAKE_DIRECTORY "${CMAKE_INSTALL_PREFIX}")
  file(REAL_PATH "${CMAKE_INSTALL_PREFIX}" _addon_prefix)
  if(_sdk_prefix STREQUAL _addon_prefix)
    message(FATAL_ERROR "The Qt component install prefix must not replace the original Qt SDK")
  endif()
endif()

# QtTest has a few relative includes into corelib; retain the complete source
# tree while compiling only its testlib subdirectory. Qt's own module helpers
# generate its public/private headers and the Qt6Test package configuration.
set(QtBase_SOURCE_DIR "${HARPIA_QTBASE_SOURCE_DIR}")
set(QtBase_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(QT_SOURCE_TREE "${HARPIA_QTBASE_SOURCE_DIR}")
qt_build_repo_begin()
if(HARPIA_QT_BUILD_TESTLIB)
  add_subdirectory("${HARPIA_QTBASE_SOURCE_DIR}/src/testlib" testlib)
endif()
if(HARPIA_QT_BUILD_MACDEPLOYQT)
  # The published package disables this feature, so its installed feature
  # value must not suppress the tool's otherwise independent source target.
  set(QT_FEATURE_macdeployqt ON)
  add_subdirectory("${HARPIA_QTBASE_SOURCE_DIR}/src/tools/macdeployqt" macdeployqt)
endif()
qt_build_repo_end()

# These components live in a separate install prefix and intentionally do not
# carry another QtCore copy. Their installed binaries must still find the
# original SDK before Qt can read any qt.conf used for deployment queries.
# Set the target properties explicitly even when the SDK disabled Qt's own
# rpath feature, which otherwise makes qt_apply_rpaths return without changes.
foreach(_target IN ITEMS Test macdeployqt macdeployqt_native)
  if(TARGET ${_target})
    if(APPLE AND NOT _target STREQUAL Test)
      # macdeployqt runs on the build machine, even when the app and QtTest
      # are built for the other Mac architecture. The OBS SDK is universal.
      set_property(TARGET ${_target} PROPERTY OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()
    set_property(TARGET ${_target} PROPERTY INSTALL_RPATH_USE_LINK_PATH TRUE)
    if(QT6_INSTALL_PREFIX AND QT6_INSTALL_LIBS)
      set_property(TARGET ${_target} APPEND PROPERTY INSTALL_RPATH "${_sdk_prefix}/${QT6_INSTALL_LIBS}")
    endif()
  endif()
endforeach()
