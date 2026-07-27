# QuickJS-ng, vendored — see README.harpia.md for the version and why it is here.
#
# Deliberately minimal: only the four core engine sources, no quickjs-libc (the
# standard library would give scripts file and process access, which transform
# scripts must not have), no CLI, no install rules, no tests.

add_library(harpia-quickjs STATIC
  quickjs.c
  libregexp.c
  libunicode.c
  dtoa.c
)

set_target_properties(harpia-quickjs PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS ON
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON
  FOLDER "third_party"
)

target_include_directories(harpia-quickjs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(harpia-quickjs PRIVATE _GNU_SOURCE)

if(WIN32)
  target_compile_definitions(harpia-quickjs PRIVATE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
endif()

include(CheckCCompilerFlag)

# Third-party code: never fail the build on its warnings, and turn off the
# specific ones it trips so the log stays readable. Upstream's own build does
# the same; the flag list is taken from its CMakeLists.
function(harpia_qjs_flag FLAG)
  string(MAKE_C_IDENTIFIER "QJS_HAS${FLAG}" VAR)
  check_c_compiler_flag(${FLAG} ${VAR})
  if(${VAR})
    target_compile_options(harpia-quickjs PRIVATE ${FLAG})
  endif()
endfunction()

if(MSVC)
  # C11 atomics are opt-in on MSVC and quickjs.c requires them.
  harpia_qjs_flag(/experimental:c11atomics)
  target_compile_options(harpia-quickjs PRIVATE /W0 /wd4018 /wd4061 /wd4100 /wd4200
                         /wd4242 /wd4244 /wd4245 /wd4267 /wd4388 /wd4389 /wd4456
                         /wd4457 /wd4710 /wd4711 /wd4820 /wd4996 /wd5045)
  # OBS builds its targets with /WX; this one must not inherit it.
  string(REGEX REPLACE "/WX" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
else()
  target_compile_options(harpia-quickjs PRIVATE -w -Wno-error)
  foreach(f -funsigned-char -Wno-implicit-fallthrough -Wno-sign-compare
            -Wno-array-bounds -Wno-maybe-uninitialized)
    harpia_qjs_flag(${f})
  endforeach()
endif()

find_package(Threads)
target_link_libraries(harpia-quickjs PUBLIC ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})

find_library(HARPIA_QJS_LIBM m)
if(HARPIA_QJS_LIBM)
  target_link_libraries(harpia-quickjs PUBLIC m)
endif()
