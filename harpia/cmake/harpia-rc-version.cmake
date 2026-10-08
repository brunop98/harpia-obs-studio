# Writes the version numbers from Version.hpp as a header the Windows resource
# script (resources/harpia.rc) can include. Run at BUILD time (cmake -P) by the
# custom command in CMakeLists.txt, so bumping the patch number never needs a
# re-configure: Version.hpp itself cannot be included by rc.exe, which does not
# understand its C++ half.
#
#   cmake -DVERSION_HPP=<path>/Version.hpp -DOUT=<path>/harpia_version_rc.h -P harpia-rc-version.cmake

file(STRINGS "${VERSION_HPP}" _lines REGEX "^#define HARPIA_VERSION_(MAJOR|MINOR|PATCH) [0-9]+")
foreach(_part MAJOR MINOR PATCH)
  set(_v_${_part} "")
  foreach(_line IN LISTS _lines)
    if(_line MATCHES "^#define HARPIA_VERSION_${_part} ([0-9]+)")
      set(_v_${_part} "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  if(_v_${_part} STREQUAL "")
    message(FATAL_ERROR "harpia-rc-version: no HARPIA_VERSION_${_part} in ${VERSION_HPP}")
  endif()
endforeach()

set(_text "// Generated from Version.hpp by cmake/harpia-rc-version.cmake. Do not edit.
#define HARPIA_RC_VERSION ${_v_MAJOR},${_v_MINOR},${_v_PATCH},0
#define HARPIA_RC_VERSION_STR \"${_v_MAJOR}.${_v_MINOR}.${_v_PATCH}\"
")

# Only rewritten when it changes, so an unchanged version does not recompile the
# resources.
set(_old "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" _old)
endif()
if(NOT _old STREQUAL _text)
  file(WRITE "${OUT}" "${_text}")
endif()
