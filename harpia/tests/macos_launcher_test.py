#!/usr/bin/env python3
"""Exercise launcher discovery/forwarding with fake Mac tools, without an SDK.

These are shell integration tests, not a native Mac build or Bash 3.2 run.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1]
SCRIPTS = SOURCE / "scripts"
COMPILER_REQUIREMENTS = (SOURCE.parent / "cmake/macos/compilerconfig.cmake").read_text()
MIN_XCODE = re.search(r"set\(obs_macos_minimum_xcode ([\d.]+)\)", COMPILER_REQUIREMENTS).group(1)
MIN_SDK = re.search(r"set\(obs_macos_minimum_sdk ([\d.]+)\)", COMPILER_REQUIREMENTS).group(1)
MIN_CMAKE = re.search(r"cmake_minimum_required\(VERSION ([0-9]+\.[0-9]+(?:\.[0-9]+)?)",
                      (SOURCE.parent / "CMakeLists.txt").read_text()).group(1)


def previous_version(version):
    parts = [int(part) for part in version.split(".")]
    if len(parts) > 1 and parts[1]:
        return f"{parts[0]}.{parts[1] - 1}"
    return f"{parts[0] - 1}.99"

MOCK_TOOL = r'''#!PYTHON
import json, os, pathlib, sys
root = pathlib.Path(os.environ["HARPIA_LAUNCHER_MOCK_ROOT"])
configuration = json.loads((root / "mock.json").read_text())
name = pathlib.Path(sys.argv[0]).name
arguments = sys.argv[1:]
with (root / "trace.jsonl").open("a") as output:
    output.write(json.dumps({"tool": name, "arguments": arguments,
                             "developer": os.environ.get("DEVELOPER_DIR", ""),
                             "sdkroot": os.environ.get("SDKROOT", "")}) + "\n")
if name == "uname":
    print(configuration.get("system", "Darwin") if arguments == ["-s"] else "arm64")
elif name == "xcode-select":
    if arguments != ["-p"]:
        raise SystemExit("The launcher must not change global xcode-select settings")
    print(configuration["developer"])
elif name == "xcodebuild":
    if arguments == ["-version"]:
        print("Xcode " + configuration["xcode"] + "\nBuild version TEST")
    elif arguments == ["-checkFirstLaunchStatus"]:
        raise SystemExit(configuration.get("first_launch_exit", 0))
    else:
        raise SystemExit("Unexpected xcodebuild command: " + repr(arguments))
elif name == "xcrun":
    if "--show-sdk-platform-version" in arguments or "--show-sdk-version" in arguments:
        print(configuration["sdk"])
    elif "--show-sdk-path" in arguments:
        print(configuration["sdk_path"])
    elif "--find" in arguments or "-f" in arguments:
        tool = arguments[-1]
        if tool == "xcodebuild":
            directory = pathlib.Path(configuration["developer"]) / "usr/bin"
        elif tool == "clang":
            directory = pathlib.Path(configuration["developer"]) / "Toolchains/XcodeDefault.xctoolchain/usr/bin"
        else:
            directory = root / "tools"
        print(directory / tool)
    else:
        raise SystemExit("Unexpected xcrun command: " + repr(arguments))
elif name == "cmake":
    if arguments == ["--version"]:
        print("cmake version " + configuration.get("cmake", "3.31.6"))
    elif "--build" in arguments:
        if configuration.get("build_exit", 0):
            raise SystemExit(configuration["build_exit"])
        if "harpia-deploy" in arguments:
            build = pathlib.Path(arguments[arguments.index("--build") + 1])
            config = arguments[arguments.index("--config") + 1]
            (build / "harpia" / config / "Harpia.app/Contents/MacOS").mkdir(parents=True, exist_ok=True)
    elif "--preset" in arguments:
        raise SystemExit(configuration.get("configure_exit", 0))
    else:
        raise SystemExit("Unexpected cmake command: " + repr(arguments))
elif name == "ctest":
    if arguments == ["--version"]:
        print("ctest version " + configuration.get("ctest", configuration.get("cmake", "3.31.6")))
    else:
        raise SystemExit(configuration.get("test_exit", 0))
elif name == "python3":
    if arguments in (["--version"], ["-V"]):
        print("Python " + configuration.get("python", "3.12.2"))
    elif len(arguments) >= 2 and arguments[0] == "-c" and "sys.version_info" in arguments[1]:
        print(configuration.get("python", "3.12.2"))
    else:
        os.execv("PYTHON", ["PYTHON"] + arguments)
elif name == "git":
    print("git version 2.49.0")
elif name == "clang":
    print("Apple clang version 17.0.0")
elif name == "ffmpeg":
    print("ffmpeg version 7.1.3")
elif name == "open":
    raise SystemExit(configuration.get("open_exit", 0))
elif name in ("package-mock", "package-macos.sh"):
    raise SystemExit(configuration.get("package_exit", 0))
elif name in ("brew", "sudo"):
    raise SystemExit("Tests forbid automatic installation or privilege changes")
'''.replace("PYTHON", sys.executable)


class LauncherTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="harpia launcher with spaces ")
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.repo = self.root / "source checkout"
        self.scripts = self.repo / "harpia/scripts"
        self.scripts.mkdir(parents=True)
        for name in ("macos-tools.sh", "build-macos.sh", "build-and-run-harpia.sh", "build-and-run-harpia.command"):
            shutil.copy2(SCRIPTS / name, self.scripts / name)
        for name in ("CMakeLists.txt", "CMakePresets.json", "cmake/macos/compilerconfig.cmake"):
            source = SOURCE.parent / name
            destination = self.repo / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
        self.tools = self.root / "tools"
        self.tools.mkdir()
        self.mock = self.root / "mock-tool"
        self.mock.write_text(MOCK_TOOL)
        self.mock.chmod(0o755)
        for name in ("uname", "xcode-select", "xcodebuild", "xcrun", "cmake", "ctest", "python3", "git", "clang",
                     "codesign", "sips", "iconutil", "otool", "install_name_tool", "lipo", "ditto", "open", "ffmpeg",
                     "brew", "sudo"):
            (self.tools / name).symlink_to(self.mock)
        # Keep PATH deliberately small: a missing mocked tool must not be
        # accidentally supplied by this Linux host's ordinary development PATH.
        for name in ("dirname", "basename", "sed", "awk", "grep", "sort", "head", "tail", "cut", "tr", "find",
                     "mkdir", "pwd", "readlink", "ls", "cat", "date"):
            executable = shutil.which(name)
            if executable:
                (self.tools / name).symlink_to(executable)
        developer = self.root / "Xcode Test.app/Contents/Developer"
        sdk = developer / "Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
        sdk.mkdir(parents=True)
        (developer / "usr/bin").mkdir(parents=True)
        (developer / "usr/bin/xcodebuild").symlink_to(self.mock)
        toolchain = developer / "Toolchains/XcodeDefault.xctoolchain/usr/bin"
        toolchain.mkdir(parents=True)
        (toolchain / "clang").symlink_to(self.mock)
        self.configuration = {"developer": str(developer), "sdk_path": str(sdk), "xcode": MIN_XCODE, "sdk": MIN_SDK}
        self.environment = dict(os.environ, PATH=str(self.tools), HARPIA_LAUNCHER_MOCK_ROOT=str(self.root))
        self.environment.pop("DEVELOPER_DIR", None)
        self.environment.pop("SDKROOT", None)
        self.environment.pop("CODESIGN_IDENT", None)
        self.environment.pop("HARPIA_MAC_CMAKE", None)
        self.environment.pop("HARPIA_MAC_CTEST", None)
        self.environment.pop("HARPIA_MAC_PYTHON", None)
        self.save_configuration()

    def save_configuration(self, **changes):
        self.configuration.update(changes)
        (self.root / "mock.json").write_text(json.dumps(self.configuration))

    def run_script(self, name="build-and-run-harpia.sh", *arguments):
        return subprocess.run(["/bin/bash", str(self.scripts / name), *arguments], cwd=self.root,
                              env=self.environment, text=True, capture_output=True, timeout=20)

    def run_helper(self, tests=False):
        # Override only candidate-directory enumeration in this unit fixture.
        # Discovery/version checking itself remains the production shell code.
        command = r'''source "$1"
harpia_macos_search_dirs() { printf '%s\n' "$HARPIA_LAUNCHER_MOCK_ROOT/tools"; }
harpia_macos_check_tools "$2" "$3"
'''
        return subprocess.run(["/bin/bash", "-c", command, "launcher-test", str(self.scripts / "macos-tools.sh"),
                               str(self.repo), "true" if tests else "false"], cwd=self.root,
                              env=dict(self.environment, DEVELOPER_DIR=self.configuration["developer"]),
                              text=True, capture_output=True, timeout=20)

    def trace(self, tool=None):
        path = self.root / "trace.jsonl"
        calls = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
        return [call for call in calls if tool is None or call["tool"] == tool]

    def assert_no_build_or_launch(self):
        self.assertFalse(any("--preset" in call["arguments"] or "--build" in call["arguments"]
                             for call in self.trace("cmake")))
        self.assertFalse(self.trace("open"))
        self.assertFalse(self.trace("brew"))
        self.assertFalse(self.trace("sudo"))

    def test_check_tools_selects_full_xcode_without_building_or_launching(self):
        result = self.run_script("build-and-run-harpia.sh", "--check-tools")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_no_build_or_launch()
        self.assertTrue(self.trace("xcodebuild"))
        self.assertTrue(any("--show-sdk-path" in call["arguments"] for call in self.trace("xcrun")))
        self.assertTrue(all(call["arguments"] == ["-p"] for call in self.trace("xcode-select")))

    def test_debug_wrapper_builds_and_launches_from_a_checkout_with_spaces(self):
        build = self.root / "custom build output"
        result = self.run_script("build-and-run-harpia.sh", "Debug", "--arch", "x86_64", "--build-dir", str(build),
                                 "--preset", "macos-ci", "--identity", "A signing identity with spaces")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        configure = next(call for call in self.trace("cmake") if "--preset" in call["arguments"])
        self.assertIn("macos-ci", configure["arguments"])
        self.assertIn("-DCMAKE_OSX_ARCHITECTURES=x86_64", configure["arguments"])
        self.assertIn("-DHARPIA_MAC_CODESIGN_IDENTITY=A signing identity with spaces", configure["arguments"])
        self.assertTrue(any(argument.startswith("-DPython3_EXECUTABLE=") for argument in configure["arguments"]))
        self.assertTrue(any(argument.startswith("-DGIT_EXECUTABLE=") for argument in configure["arguments"]))
        builds = [call for call in self.trace("cmake") if "--build" in call["arguments"]]
        self.assertEqual(len(builds), 1)
        self.assertIn("Debug", builds[0]["arguments"])
        self.assertIn("harpia-deploy", builds[0]["arguments"])
        self.assertEqual(self.trace("open")[-1]["arguments"], [str(build / "harpia/Debug/Harpia.app")])

    def test_explicit_xcode_app_path_is_used_without_changing_global_selection(self):
        developer = Path(self.configuration["developer"])
        self.environment["DEVELOPER_DIR"] = str(developer.parent.parent)
        result = self.run_script("build-and-run-harpia.sh", "--check-tools")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.trace("xcode-select"))
        self.assertTrue(all(call["developer"] == str(developer) for call in self.trace("xcodebuild")))

    def test_selected_sdk_overrides_a_stale_sdkroot_during_configuration(self):
        self.environment["SDKROOT"] = str(self.root / "removed old SDK")
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        configure = next(call for call in self.trace("cmake") if "--preset" in call["arguments"])
        self.assertIn("-DCMAKE_OSX_SYSROOT=" + self.configuration["sdk_path"], configure["arguments"])

    def test_test_and_packaging_options_are_forwarded_before_launch(self):
        (self.scripts / "package-macos.sh").symlink_to(self.mock)
        result = self.run_script("build-and-run-harpia.sh", "--arch", "universal", "--test", "--check-runtime")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        configure = next(call for call in self.trace("cmake") if "--preset" in call["arguments"])
        self.assertIn("-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64", configure["arguments"])
        self.assertIn("-DHARPIA_BUILD_TESTS=ON", configure["arguments"])
        self.assertTrue(any("harpia-tests" in call["arguments"] for call in self.trace("cmake")))
        self.assertEqual(len([call for call in self.trace("ctest") if call["arguments"] != ["--version"]]), 1)
        packages = self.trace("package-macos.sh")
        self.assertEqual(len(packages), 1)
        self.assertIn("--check-runtime", packages[0]["arguments"])
        calls = [call["tool"] for call in self.trace()]
        self.assertLess(calls.index("ctest"), calls.index("package-macos.sh"))
        self.assertLess(calls.index("package-macos.sh"), calls.index("open"))

    def test_failed_configure_keeps_error_status_and_does_not_launch(self):
        self.save_configuration(configure_exit=37)
        result = self.run_script()
        self.assertEqual(result.returncode, 37, result.stdout + result.stderr)
        self.assertFalse(self.trace("open"))
        self.assertFalse(any("--build" in call["arguments"] for call in self.trace("cmake")))

    def test_failed_build_keeps_error_status_and_does_not_launch(self):
        self.save_configuration(build_exit=41)
        result = self.run_script()
        self.assertEqual(result.returncode, 41, result.stdout + result.stderr)
        self.assertFalse(self.trace("open"))

    def test_finder_command_delegates_arguments_and_preserves_noninteractive_failure(self):
        self.save_configuration(configure_exit=43)
        result = self.run_script("build-and-run-harpia.command", "Debug")
        self.assertEqual(result.returncode, 43, result.stdout + result.stderr)
        self.assertFalse(self.trace("open"))

    def test_non_mac_host_exits_before_build(self):
        self.save_configuration(system="Linux")
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("macOS", result.stdout + result.stderr)
        self.assert_no_build_or_launch()

    def test_old_xcode_reports_actual_requirement_and_apple_download(self):
        self.save_configuration(xcode=previous_version(MIN_XCODE))
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(MIN_XCODE, result.stdout + result.stderr)
        self.assertIn("developer.apple.com", result.stdout + result.stderr)
        self.assert_no_build_or_launch()

    def test_old_sdk_is_rejected_even_with_new_xcode(self):
        self.save_configuration(sdk=previous_version(MIN_SDK))
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(MIN_SDK, result.stdout + result.stderr)
        self.assertIn("developer.apple.com", result.stdout + result.stderr)

    def test_unfinished_xcode_setup_has_actionable_guidance(self):
        self.save_configuration(first_launch_exit=69)
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stdout + result.stderr, r"(?i)(first.launch|license|open.Xcode)")
        self.assert_no_build_or_launch()

    def test_missing_cmake_reports_download_before_build(self):
        (self.tools / "cmake").unlink()
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cmake.org", result.stdout + result.stderr)
        self.assert_no_build_or_launch()

    def test_old_cmake_reports_the_minimum_supported_version(self):
        self.save_configuration(cmake=previous_version(MIN_CMAKE))
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(MIN_CMAKE, result.stdout + result.stderr)
        self.assertIn("cmake.org", result.stdout + result.stderr)

    def test_old_python_reports_python310_and_its_download(self):
        self.save_configuration(python="3.9.22")
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("3.10", result.stdout + result.stderr)
        self.assertIn("python.org", result.stdout + result.stderr)

    def test_a_newer_installation_is_found_after_an_outdated_path_entry(self):
        alternate = self.root / "alternate tools with spaces"
        alternate.mkdir()
        for name, version in (("cmake", "3.31.6"), ("python3", "3.12.2")):
            executable = alternate / name
            prefix = "cmake version " if name == "cmake" else ""
            executable.write_text(f"#!{sys.executable}\nprint({prefix + version!r})\n")
            executable.chmod(0o755)
        self.save_configuration(cmake=previous_version(MIN_CMAKE), python="3.9.22")
        command = r'''source "$1"
harpia_macos_search_dirs() {
  printf '%s\n' "$HARPIA_LAUNCHER_MOCK_ROOT/tools" "$HARPIA_LAUNCHER_MOCK_ROOT/alternate tools with spaces"
}
harpia_macos_find_tool "$2" "$3"
'''
        for name, minimum in (("cmake", MIN_CMAKE), ("python3", "3.10")):
            with self.subTest(tool=name):
                result = subprocess.run(["/bin/bash", "-c", command, "launcher-test", str(self.scripts / "macos-tools.sh"),
                                         name, minimum], env=self.environment, text=True, capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout.strip(), str(alternate / name))

    def test_test_mode_requires_ffmpeg_before_configure(self):
        (self.tools / "ffmpeg").unlink()
        result = self.run_helper(tests=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ffmpeg", (result.stdout + result.stderr).lower())
        self.assertIn("https://", result.stdout + result.stderr)
        self.assert_no_build_or_launch()

    def test_matching_ctest_is_required_only_for_test_mode(self):
        (self.tools / "ctest").unlink()
        ordinary = self.run_helper()
        self.assertEqual(ordinary.returncode, 0, ordinary.stdout + ordinary.stderr)
        tested = self.run_helper(tests=True)
        self.assertNotEqual(tested.returncode, 0)
        self.assertIn("ctest", (tested.stdout + tested.stderr).lower())

    def test_outdated_ctest_is_rejected_before_media_tests_start(self):
        self.save_configuration(ctest=previous_version(MIN_CMAKE))
        result = self.run_helper(tests=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ctest", (result.stdout + result.stderr).lower())
        self.assertIn("cmake.org", result.stdout + result.stderr)

    def test_sdk_directory_must_exist(self):
        self.save_configuration(sdk_path=str(self.root / "missing Mac SDK"))
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SDK", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
