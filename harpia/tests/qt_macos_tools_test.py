#!/usr/bin/env python3
"""Exercise matching Qt add-on preparation without an Apple SDK or downloads.

The outer CMake project is real. Only the nested configure/build/install tool
is replaced with a recorder, so argument boundaries, reuse and failure handling
are checked independently of Qt's own build implementation.
"""

import configparser
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


HARPIA = Path(__file__).resolve().parents[1]
HELPER = HARPIA / "cmake/qt-macos-tools.cmake"
WRAPPER = HARPIA / "cmake/qt-macos-tools"

MOCK_CMAKE = r'''#!PYTHON
import json, os, pathlib, re, sys
arguments = sys.argv[1:]
trace = pathlib.Path(os.environ["HARPIA_QT_TOOLS_TRACE"])
configuration = json.loads(pathlib.Path(os.environ["HARPIA_QT_TOOLS_MOCK"]).read_text())
if "--build" in arguments:
    stage, marker = "build", "--build"
elif "--install" in arguments:
    stage, marker = "install", "--install"
else:
    stage, marker = "configure", "-B"
with trace.open("a") as output:
    output.write(json.dumps({"stage": stage, "arguments": arguments}) + "\n")
failure = configuration.get(stage + "_exit", 0)
if failure:
    print("intentional " + stage + " failure", file=sys.stderr)
    raise SystemExit(failure)
build = pathlib.Path(arguments[arguments.index(marker) + 1])
build.mkdir(parents=True, exist_ok=True)
saved = build / "fake-configure.json"
if stage == "configure":
    values = {}
    for argument in arguments:
        match = re.match(r"-D([^:=]+)(?::[^=]+)?=(.*)", argument)
        if match:
            values[match[1]] = match[2]
    saved.write_text(json.dumps(values))
elif stage == "install":
    values = json.loads(saved.read_text())
    prefix = pathlib.Path(values["CMAKE_INSTALL_PREFIX"])
    if values.get("HARPIA_QT_BUILD_MACDEPLOYQT", "").upper() in ("ON", "TRUE", "1"):
        program = prefix / "bin/macdeployqt"
        program.parent.mkdir(parents=True, exist_ok=True)
        program.write_text("#!/bin/sh\nexit 0\n")
        program.chmod(0o755)
    if values.get("HARPIA_QT_BUILD_TESTLIB", "").upper() in ("ON", "TRUE", "1"):
        package = prefix / "lib/cmake/Qt6Test"
        package.mkdir(parents=True, exist_ok=True)
        (package / "Qt6TestConfig.cmake").write_text(
            'set(Qt6Test_FOUND TRUE)\nset(Qt6Test_VERSION "6.11.1")\n'
            'if(NOT TARGET Qt6::Test)\n add_library(Qt6::Test INTERFACE IMPORTED GLOBAL)\nendif()\n')
        (package / "Qt6TestConfigVersion.cmake").write_text(
            'set(PACKAGE_VERSION "6.11.1")\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\n'
            'if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)\n'
            ' set(PACKAGE_VERSION_EXACT TRUE)\nendif()\n')
'''.replace("PYTHON", sys.executable)


def quote(value):
    return "[===[" + str(value).replace("\\", "/") + "]===]"


class QtMacToolsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = shutil.which(os.environ.get("HARPIA_TEST_CMAKE", "cmake"))
        if not cls.cmake:
            raise RuntimeError("CMake 3.28+ is required; install it or set HARPIA_TEST_CMAKE.")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="harpia qt tools with spaces ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / "application source"
        self.build = self.root / "application build"
        self.source.mkdir()
        self.sdk = self.root / "selected Qt SDK"
        self.core = self.sdk / "lib/QtCore.framework/Versions/A/QtCore"
        self.core.parent.mkdir(parents=True)
        self.core.write_text("selected Qt runtime")
        for name in ("Qt6", "Qt6Core", "Qt6BuildInternals"):
            directory = self.sdk / "lib/cmake" / name
            directory.mkdir(parents=True)
            (directory / (name + "Config.cmake")).write_text("# selected SDK fixture\n")
        self.qtbase = self.root / "matching qtbase source"
        self.qtbase.mkdir()
        (self.qtbase / ".cmake.conf").write_text('set(QT_REPO_MODULE_VERSION "6.11.1")\n')
        for relative in ("src/testlib/CMakeLists.txt", "src/tools/macdeployqt/CMakeLists.txt",
                         "src/corelib/kernel/qcore_unix_p.h"):
            path = self.qtbase / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("# complete-source sentinel\n")
        self.mock = self.root / "record nested cmake"
        self.mock.write_text(MOCK_CMAKE)
        self.mock.chmod(0o755)
        self.trace_path = self.root / "trace.jsonl"
        self.configuration_path = self.root / "mock.json"
        self.configuration_path.write_text("{}")
        self.environment = dict(os.environ, HARPIA_QT_TOOLS_TRACE=str(self.trace_path),
                                HARPIA_QT_TOOLS_MOCK=str(self.configuration_path))
        self.with_tests = True
        self.imported_tool = None
        self.imported_test = False
        self.prefixes = [self.sdk]
        self.sdk_version = "6.11.1"

    def write_probe(self):
        imported = ""
        if self.imported_tool:
            imported += ("add_executable(Qt6::macdeployqt IMPORTED GLOBAL)\n"
                         "set_target_properties(Qt6::macdeployqt PROPERTIES IMPORTED_LOCATION "
                         + quote(self.imported_tool) + ")\n")
        if self.imported_test:
            imported += "add_library(Qt6::Test INTERFACE IMPORTED GLOBAL)\nset(Qt6Test_FOUND TRUE)\n"
        (self.source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.28)\n"
            "project(qt_tools_probe LANGUAGES NONE)\n"
            "set(APPLE TRUE)\n"
            "set(OS_MACOS TRUE)\n"
            f"set(Qt6_VERSION {quote(self.sdk_version)})\nset(Qt6Core_VERSION {quote(self.sdk_version)})\n"
            f"set(Qt6_DIR {quote(self.sdk / 'lib/cmake/Qt6')})\n"
            f"set(Qt6Core_DIR {quote(self.sdk / 'lib/cmake/Qt6Core')})\n"
            f"set(QT6_INSTALL_PREFIX {quote(self.sdk)})\n"
            "set(QT6_INSTALL_BINS bin)\nset(QT6_INSTALL_LIBEXECS libexec)\n"
            "set(QT6_INSTALL_LIBS lib)\nset(QT6_INSTALL_PLUGINS plugins)\n"
            "set(CMAKE_PREFIX_PATH " + " ".join(quote(prefix) for prefix in self.prefixes) + ")\n"
            "add_library(Qt6::Core SHARED IMPORTED GLOBAL)\n"
            "set_target_properties(Qt6::Core PROPERTIES FRAMEWORK TRUE IMPORTED_LOCATION "
            + quote(self.core) + " INTERFACE_QT_MAJOR_VERSION 6)\n" + imported +
            f"set(HARPIA_BUILD_TESTS {'ON' if self.with_tests else 'OFF'})\n"
            f"set(HARPIA_QTBASE_SOURCE_DIR {quote(self.qtbase)} CACHE PATH \"\")\n"
            f"include({quote(HELPER)})\n"
            "set(_original_cmake_command \"${CMAKE_COMMAND}\")\n"
            f"set(CMAKE_COMMAND {quote(self.mock)})\n"
            "harpia_prepare_qt_tools()\n"
            "set(CMAKE_COMMAND \"${_original_cmake_command}\")\n"
            "get_target_property(_core_location Qt6::Core IMPORTED_LOCATION)\n"
            "file(WRITE \"${CMAKE_BINARY_DIR}/results.txt\"\n"
            " \"sdk=${QT6_INSTALL_PREFIX}\\n\"\n"
            " \"qt6=${Qt6_DIR}\\n\"\n"
            " \"core=${_core_location}\\n\"\n"
            " \"test=${Qt6Test_DIR}\\n\")\n"
            "file(GENERATE OUTPUT \"${CMAKE_BINARY_DIR}/deploy-tool.txt\" CONTENT \"${HARPIA_MACDEPLOYQT}\\n\")\n"
        )

    def configure(self, *arguments, succeeds=True):
        self.write_probe()
        result = subprocess.run([self.cmake, "-S", str(self.source), "-B", str(self.build), *arguments],
                                env=self.environment, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=30)
        if not succeeds:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            return result.stdout
        self.assertEqual(result.returncode, 0, result.stdout)
        values = dict(line.split("=", 1) for line in (self.build / "results.txt").read_text().splitlines())
        self.assertEqual(values["sdk"], str(self.sdk))
        self.assertEqual(values["qt6"], str(self.sdk / "lib/cmake/Qt6"))
        self.assertEqual(values["core"], str(self.core))
        return values

    def calls(self):
        if not self.trace_path.exists():
            return []
        return [json.loads(line) for line in self.trace_path.read_text().splitlines()]

    def definitions(self):
        arguments = [call["arguments"] for call in self.calls() if call["stage"] == "configure"][-1]
        return {argument[2:].split(":", 1)[0].split("=", 1)[0]: argument.split("=", 1)[1]
                for argument in arguments if argument.startswith("-D") and "=" in argument}

    def make_tool(self, relative):
        tool = self.sdk / relative
        tool.parent.mkdir(parents=True, exist_ok=True)
        tool.write_text("#!/bin/sh\nexit 0\n")
        tool.chmod(0o755)
        return tool

    def test_imported_sdk_tool_is_used_without_building_anything(self):
        self.with_tests = False
        self.imported_tool = self.make_tool("tools in SDK/macdeployqt")
        self.configure()
        self.assertEqual((self.build / "deploy-tool.txt").read_text().strip(), str(self.imported_tool))
        self.assertEqual(self.calls(), [])

    def test_installed_sdk_tool_and_existing_test_target_need_no_addon(self):
        tool = self.make_tool("bin/macdeployqt")
        self.imported_test = True
        self.configure()
        self.assertEqual((self.build / "deploy-tool.txt").read_text().strip(), str(tool))
        self.assertEqual(self.calls(), [])

    def test_missing_components_build_and_install_in_order_using_matching_sdk(self):
        values = self.configure()
        self.assertEqual([call["stage"] for call in self.calls()], ["configure", "build", "install"])
        definitions = self.definitions()
        self.assertEqual(definitions["HARPIA_QT_VERSION"], "6.11.1")
        self.assertEqual(definitions["Qt6_DIR"], str(self.sdk / "lib/cmake/Qt6"))
        self.assertEqual(definitions["HARPIA_QTBASE_SOURCE_DIR"], str(self.qtbase))
        self.assertIn(definitions["HARPIA_QT_BUILD_MACDEPLOYQT"].upper(), ("ON", "TRUE", "1"))
        self.assertIn(definitions["HARPIA_QT_BUILD_TESTLIB"].upper(), ("ON", "TRUE", "1"))
        self.assertNotEqual(Path(definitions["CMAKE_INSTALL_PREFIX"]), self.sdk)
        self.assertTrue(Path((self.build / "deploy-tool.txt").read_text().strip()).is_file())
        self.assertTrue((Path(values["test"]) / "Qt6TestConfig.cmake").is_file())
        qt_configuration = configparser.ConfigParser(interpolation=None)
        qt_configuration.read(Path(definitions["CMAKE_INSTALL_PREFIX"]) / "bin/qt.conf")
        self.assertEqual(qt_configuration["Paths"]["Prefix"], str(self.sdk))
        self.assertEqual(qt_configuration["Paths"]["Plugins"], "plugins")
        self.assertEqual(qt_configuration["Paths"]["Libraries"], "lib")

    def test_an_existing_component_is_not_rebuilt(self):
        tool = self.make_tool("bin/macdeployqt")
        self.configure()
        definitions = self.definitions()
        self.assertIn(definitions["HARPIA_QT_BUILD_MACDEPLOYQT"].upper(), ("OFF", "FALSE", "0"))
        self.assertIn(definitions["HARPIA_QT_BUILD_TESTLIB"].upper(), ("ON", "TRUE", "1"))
        self.assertEqual((self.build / "deploy-tool.txt").read_text().strip(), str(tool))

    def test_universal_architectures_and_sysroot_remain_single_arguments(self):
        sysroot = self.root / "MacOS SDK with spaces"
        sysroot.mkdir()
        self.configure("-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64", "-DCMAKE_OSX_SYSROOT=" + str(sysroot),
                       "-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0")
        definitions = self.definitions()
        self.assertEqual(definitions["CMAKE_OSX_ARCHITECTURES"], "arm64;x86_64")
        self.assertEqual(definitions["CMAKE_OSX_SYSROOT"], str(sysroot))
        self.assertEqual(definitions["CMAKE_OSX_DEPLOYMENT_TARGET"], "13.0")

    def test_ready_components_are_reused_on_next_configure(self):
        self.configure()
        calls = self.calls()
        self.configure()
        self.assertEqual(self.calls(), calls)

    def test_changing_architecture_does_not_reuse_an_incompatible_test_overlay(self):
        first = self.configure("-DCMAKE_OSX_ARCHITECTURES=arm64")
        second = self.configure("-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64")
        self.assertNotEqual(second["test"], first["test"])
        self.assertEqual([call["stage"] for call in self.calls()],
                         ["configure", "build", "install", "configure", "build", "install"])
        self.assertIn(self.definitions()["HARPIA_QT_BUILD_TESTLIB"].upper(), ("ON", "TRUE", "1"))

    def test_a_foreign_test_package_cannot_mix_qt_installations(self):
        foreign = self.root / "unrelated Qt SDK"
        package = foreign / "lib/cmake/Qt6Test"
        package.mkdir(parents=True)
        (package / "Qt6TestConfig.cmake").write_text(
            "set(Qt6Test_FOUND TRUE)\nset(Qt6Test_VERSION 6.11.1)\n"
            "add_library(Qt6::Test INTERFACE IMPORTED GLOBAL)\n")
        (package / "Qt6TestConfigVersion.cmake").write_text(
            "set(PACKAGE_VERSION 6.11.1)\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
            "set(PACKAGE_VERSION_EXACT TRUE)\n")
        self.prefixes.insert(0, foreign)
        values = self.configure()
        self.assertNotEqual(values["test"], str(package))
        self.assertIn(self.definitions()["HARPIA_QT_BUILD_TESTLIB"].upper(), ("ON", "TRUE", "1"))

    def test_failed_build_keeps_failure_and_does_not_install(self):
        self.configuration_path.write_text(json.dumps({"build_exit": 17}))
        output = self.configure(succeeds=False)
        self.assertIn("intentional build failure", output)
        self.assertEqual([call["stage"] for call in self.calls()], ["configure", "build"])

    def test_failed_configure_does_not_build_or_install(self):
        self.configuration_path.write_text(json.dumps({"configure_exit": 19}))
        output = self.configure(succeeds=False)
        self.assertIn("intentional configure failure", output)
        self.assertEqual([call["stage"] for call in self.calls()], ["configure"])

    def test_a_tool_elsewhere_on_path_cannot_replace_the_matching_qt_tool(self):
        other = self.root / "unrelated Qt/bin"
        other.mkdir(parents=True)
        program = other / "macdeployqt"
        program.write_text("#!/bin/sh\nexit 93\n")
        program.chmod(0o755)
        self.environment["PATH"] = str(other) + os.pathsep + self.environment.get("PATH", "")
        self.with_tests = False
        self.configure()
        selected = (self.build / "deploy-tool.txt").read_text().strip()
        self.assertNotEqual(selected, str(program))
        self.assertEqual([call["stage"] for call in self.calls()], ["configure", "build", "install"])

    def test_missing_sdk_version_fails_before_starting_a_component_build(self):
        self.with_tests = False
        self.sdk_version = ""
        output = self.configure(succeeds=False)
        self.assertIn("selected Qt SDK version", output)
        self.assertEqual(self.calls(), [])

    def test_wrong_source_version_is_rejected_before_compiler_or_sdk_checks(self):
        (self.qtbase / ".cmake.conf").write_text('set(QT_REPO_MODULE_VERSION "6.10.0")\n')
        result = subprocess.run([self.cmake, "-S", str(WRAPPER), "-B", str(self.root / "wrong source build"),
                                 "-DHARPIA_QT_VERSION=6.11.1", "-DHARPIA_QTBASE_SOURCE_DIR=" + str(self.qtbase),
                                 "-DCMAKE_INSTALL_PREFIX=" + str(self.root / "overlay")],
                                env=self.environment, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=30)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("does not match SDK version", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
