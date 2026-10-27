#!/usr/bin/env python3
"""Build only a mock recorder against Harpia's actual runtime dependency rules.

No Qt, OBS SDK or GPU is required. Real shared modules use the renderer target
names and the existing per-module POST_BUILD staging shape. The Windows repair
copy must also restore a staged DLL without recompiling those modules.

Usage: python3 harpia/tests/graphicsruntime_test.py [path/to/cmake]
"""

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


CMAKE = shutil.which("cmake")
HELPER = Path(__file__).resolve().parents[1] / "cmake" / "HarpiaRuntimeTargets.cmake"


def digest(path):
    return hashlib.sha256(path.read_bytes()).digest()


class GraphicsRuntimeBuild(unittest.TestCase):
    def setUp(self):
        if not CMAKE:
            self.skipTest("CMake is required for the runtime build-graph fixture.")

    def fixture(self, renderers, windows):
        temporary = tempfile.TemporaryDirectory(prefix="harpia graphics runtime ")
        self.addCleanup(temporary.cleanup)
        source = Path(temporary.name)
        build = source / "build"
        runtime = build / "rundir" / "Release" / "bin" / "64bit"
        (source / "core.cpp").write_text("int obs_core() { return 0; }\n", encoding="utf-8")
        (source / "recorder.cpp").write_text("extern int obs_core(); int main() { return obs_core(); }\n", encoding="utf-8")
        (source / "mux.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        modules = []
        for renderer in renderers:
            (source / f"{renderer}.cpp").write_text(
                'extern int obs_core(); extern "C" int renderer_version() { return obs_core() + 1; }\n', encoding="utf-8")
            modules.append(f'''
add_library({renderer} MODULE EXCLUDE_FROM_ALL "{renderer}.cpp")
target_link_libraries({renderer} PRIVATE libobs)
set_target_properties({renderer} PROPERTIES PREFIX "" SUFFIX ".dll")
# The existing OBS Windows helper stages a renderer only after it is built.
add_custom_command(TARGET {renderer} POST_BUILD
  COMMAND "${{CMAKE_COMMAND}}" -E make_directory "${{runtime}}"
  COMMAND "${{CMAKE_COMMAND}}" -E copy_if_different "$<TARGET_FILE:{renderer}>" "${{runtime}}"
  VERBATIM)
''')
        checks = "\n".join(
            f'if(NOT EXISTS "{runtime.as_posix()}/{name}.dll")\n message(FATAL_ERROR "renderer missing before recorder link: {name}")\nendif()'
            for name in renderers
        )
        (source / "check-runtime.cmake.in").write_text(checks + "\n", encoding="utf-8")
        stage = 'harpia_stage_graphics_runtime(harpia-recorder "${runtime}")' if windows else ""
        (source / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.28)
project(HarpiaGraphicsRuntime LANGUAGES CXX)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/artifacts/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/artifacts/$<CONFIG>")
set(runtime "${{CMAKE_BINARY_DIR}}/rundir/$<CONFIG>/bin/64bit")
add_library(libobs STATIC core.cpp)
add_executable(harpia-recorder EXCLUDE_FROM_ALL recorder.cpp)
target_link_libraries(harpia-recorder PRIVATE libobs)
add_executable(obs-ffmpeg-mux EXCLUDE_FROM_ALL mux.cpp)
set_target_properties(obs-ffmpeg-mux PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${{runtime}}")
{''.join(modules)}
include("{HELPER.as_posix()}")
harpia_add_runtime_dependencies(harpia-recorder)
{stage}
file(GENERATE OUTPUT "${{CMAKE_BINARY_DIR}}/check-runtime.cmake" INPUT "${{CMAKE_CURRENT_SOURCE_DIR}}/check-runtime.cmake.in")
add_custom_command(TARGET harpia-recorder POST_BUILD
  COMMAND "${{CMAKE_COMMAND}}" -P "${{CMAKE_BINARY_DIR}}/check-runtime.cmake"
  VERBATIM)
''', encoding="utf-8")
        generator = ["-G", "Ninja"] if shutil.which("ninja") else []
        self.run_command([CMAKE, "-S", str(source), "-B", str(build), *generator, "-DCMAKE_BUILD_TYPE=Release"])
        return source, build, runtime

    def run_command(self, args):
        result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 0, result.stdout)

    def build_recorder(self, build):
        self.run_command([CMAKE, "--build", str(build), "--config", "Release", "--target", "harpia-recorder", "--parallel", "2"])

    def test_targeted_windows_build_repairs_and_refreshes_renderer(self):
        source, build, runtime = self.fixture(("libobs-d3d11", "libobs-opengl"), windows=True)
        self.build_recorder(build)
        d3d = runtime / "libobs-d3d11.dll"
        opengl = runtime / "libobs-opengl.dll"
        self.assertTrue(d3d.is_file() and opengl.is_file())
        self.assertTrue((runtime / ("obs-ffmpeg-mux.exe" if os.name == "nt" else "obs-ffmpeg-mux")).is_file())
        renderer_binary = build / "artifacts" / "Release" / "libobs-d3d11.dll"
        recorder_binary = build / "artifacts" / "Release" / ("harpia-recorder.exe" if os.name == "nt" else "harpia-recorder")
        renderer_stamp = renderer_binary.stat().st_mtime_ns
        recorder_stamp = recorder_binary.stat().st_mtime_ns
        original = digest(d3d)
        d3d.unlink()
        opengl.unlink()
        self.build_recorder(build)
        self.assertEqual(digest(d3d), original)
        self.assertTrue(opengl.is_file())
        self.assertEqual(renderer_binary.stat().st_mtime_ns, renderer_stamp)
        self.assertEqual(recorder_binary.stat().st_mtime_ns, recorder_stamp)
        # A renderer rebuild must replace the staged copy, not retain an old DLL.
        (source / "libobs-d3d11.cpp").write_text(
            'extern int obs_core(); extern "C" int renderer_version() { return obs_core() + 2; }\n', encoding="utf-8")
        self.build_recorder(build)
        self.assertNotEqual(digest(d3d), original)
        self.assertEqual(digest(d3d), digest(renderer_binary))

    def test_opengl_only_platform_keeps_existing_post_build_staging(self):
        _, build, runtime = self.fixture(("libobs-opengl",), windows=False)
        self.build_recorder(build)
        self.assertTrue((runtime / "libobs-opengl.dll").is_file())
        self.assertFalse((runtime / "libobs-d3d11.dll").exists())

    def test_unavailable_optional_targets_do_not_break_configuration(self):
        _, build, runtime = self.fixture((), windows=True)
        self.build_recorder(build)
        self.assertTrue((runtime / ("obs-ffmpeg-mux.exe" if os.name == "nt" else "obs-ffmpeg-mux")).is_file())


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        CMAKE = sys.argv.pop(1)
    if not CMAKE:
        raise SystemExit("Pass a CMake executable or put cmake on PATH.")
    unittest.main(verbosity=2)
