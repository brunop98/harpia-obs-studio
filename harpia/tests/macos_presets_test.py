#!/usr/bin/env python3
"""Exercise production Mac preset inheritance with real, SDK-free CMake runs.

An SDK-free child preset inherits the production CI preset's cache variables
and uses a portable generator. No Apple compiler is needed.
"""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


PRESETS = Path(__file__).resolve().parents[2] / "CMakePresets.json"
CACHE_ENABLED = "CMAKE_XCODE_ATTRIBUTE_COMPILATION_CACHE_ENABLE_CACHING"
CACHE_PATH = "CMAKE_XCODE_ATTRIBUTE_COMPILATION_CACHE_CAS_PATH"


class MacPresetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = shutil.which(os.environ.get("HARPIA_TEST_CMAKE", "cmake"))
        if not cls.cmake:
            raise RuntimeError("CMake 3.28+ is required; install it or set HARPIA_TEST_CMAKE.")

    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="harpia Mac presets with spaces ")
        self.addCleanup(scratch.cleanup)
        self.source = Path(scratch.name).resolve()
        self.build = self.source / "build output"
        presets = json.loads(PRESETS.read_text())
        # Keep all production presets intact. Only this test child overrides
        # the host restriction and generator to configure an empty portable app.
        presets["configurePresets"].append({
            "name": "macos-ci-probe", "inherits": ["macos-ci"], "condition": True,
            "generator": "Ninja" if shutil.which("ninja") else "Unix Makefiles",
            "binaryDir": str(self.build)})
        (self.source / "CMakePresets.json").write_text(json.dumps(presets))
        (self.source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.28)\n"
            "project(mac_preset_probe LANGUAGES NONE)\n"
            "file(WRITE \"${CMAKE_BINARY_DIR}/preset-results.txt\"\n"
            f" \"enabled=${{{CACHE_ENABLED}}}\\n\"\n"
            f" \"path=${{{CACHE_PATH}}}\\n\"\n"
            " \"deployment=${CMAKE_OSX_DEPLOYMENT_TARGET}\\n\"\n"
            " \"warnings=${CMAKE_COMPILE_WARNING_AS_ERROR}\\n\")\n")
        self.environment = dict(os.environ)
        self.environment.pop("XCODE_CAS_PATH", None)

    def configure(self, *definitions):
        result = subprocess.run(
            [self.cmake, "--preset", "macos-ci-probe", *definitions],
            cwd=self.source, env=self.environment, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout)
        return dict(line.split("=", 1) for line in
                    (self.build / "preset-results.txt").read_text().splitlines())

    def assert_ci_cache_disabled(self, values):
        self.assertEqual(values["enabled"], "NO")
        self.assertEqual(values["path"], "")
        self.assertEqual(values["deployment"], "13.0")
        self.assertIn(values["warnings"], ("TRUE", "ON", "1"))

    def test_ci_does_not_require_a_shared_cas_store(self):
        for cas_path in (None, str(self.source / "missing shared CAS store")):
            with self.subTest(cas_path=cas_path):
                if cas_path is None:
                    self.environment.pop("XCODE_CAS_PATH", None)
                else:
                    self.environment["XCODE_CAS_PATH"] = cas_path
                self.assert_ci_cache_disabled(self.configure())

    def test_ci_reconfigure_resets_previously_enabled_cache_and_store(self):
        stale_path = str(self.source / "old missing CAS store")
        old = self.configure(f"-D{CACHE_ENABLED}=YES", f"-D{CACHE_PATH}={stale_path}")
        self.assertEqual(old["enabled"], "YES")
        self.assertEqual(old["path"], stale_path)
        self.assert_ci_cache_disabled(self.configure())


if __name__ == "__main__":
    unittest.main(verbosity=2)
