#!/usr/bin/env python3
"""Packaging regression tests which run without a Mac SDK or signing identity."""
import importlib.util
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("harpia_deploy", SOURCE / "scripts/deploy-macos.py")
deploy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(deploy)


def binary(path, content=b"body"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\xcf\xfa\xed\xfe" + content)
    path.chmod(0o755)
    return path


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="harpia package ")
        self.addCleanup(self.scratch.cleanup)
        # macOS exposes /var through /private/var. The deployer canonicalizes
        # paths; fixture metadata and expected paths must use the same identity.
        self.root = Path(self.scratch.name).resolve()
        self.app = self.root / "Harpia.app"
        self.executable = binary(self.app / "Contents/MacOS/Harpia")
        self.deployer = deploy.Deployer(self.app, [], ["arm64"])

    def test_otool_parser_preserves_spaces_weak_dependencies_and_fat_repeats(self):
        commands = """Load command 0
          cmd LC_LOAD_DYLIB
          name /a path/QtCore.framework/Versions/A/QtCore (offset 24)
          cmd LC_RPATH
          path @loader_path/../Frameworks (offset 12)
          cmd LC_ID_DYLIB
          name @rpath/example.dylib (offset 24)
          cmd LC_LOAD_WEAK_DYLIB
          name /System/Library/Frameworks/ScreenCaptureKit.framework/Versions/A/ScreenCaptureKit (offset 24)
          cmd LC_REEXPORT_DYLIB
          name @rpath/QtGui.framework/Versions/A/QtGui (offset 24)
        """
        dependencies, rpaths, identities = deploy.parse_load_commands(commands + commands)
        self.assertEqual(len(dependencies), 3)
        self.assertEqual(dependencies[0], "/a path/QtCore.framework/Versions/A/QtCore")
        self.assertEqual(rpaths, ["@loader_path/../Frameworks"])
        self.assertEqual(identities, {"@rpath/example.dylib"})

    def test_minimum_os_parser_covers_modern_legacy_and_universal_load_commands(self):
        output = "cmd LC_BUILD_VERSION\n minos 13.0\n sdk 26.5\ncmd LC_VERSION_MIN_MACOSX\n version 12.0\n sdk 26.5\n"
        self.assertEqual(deploy.parse_minimum_versions(output + output), ["13.0", "12.0"])
        self.assertEqual(deploy.os_version("13.0"), deploy.os_version("13.0.0"))

    def test_dependencies_cannot_require_a_newer_os_than_the_app_advertises(self):
        self.deployer.minimum_os = "13.0"
        self.deployer.minimum_versions[self.executable] = ["14.0"]
        with patch.object(self.deployer, "metadata", return_value=([], [], set())), \
                patch.object(deploy, "run", return_value="arm64"):
            with self.assertRaisesRegex(RuntimeError, "requires macOS 14.0"):
                self.deployer.verify_dependencies()

    def test_only_native_binaries_are_discovered_and_framework_aliases_are_unique(self):
        framework = self.app / "Contents/Frameworks/F.framework"
        real = binary(framework / "Versions/A/F")
        (framework / "Versions/Current").symlink_to("A")
        (framework / "F").symlink_to("Versions/Current/F")
        (self.app / "Contents/readme").write_text("not an executable")
        self.assertEqual(deploy.macho_files(self.app), sorted([self.executable, real]))

    def test_copied_dylib_resolves_original_loader_relative_dependencies(self):
        original = binary(self.root / "SDK with spaces/lib/first.dylib")
        neighbor = binary(original.parent / "second.dylib")
        copied = self.deployer.copy_dependency(original)
        self.assertEqual(self.deployer.resolve_dependency("@loader_path/second.dylib", copied, []), neighbor)
        self.assertTrue(deploy.inside(copied, self.app))

    def test_rpath_resolution_uses_original_framework_location(self):
        original = binary(self.root / "prefix/lib/F.framework/Versions/A/F")
        neighbor = binary(self.root / "prefix/lib/other.dylib")
        copied = self.deployer.copy_dependency(original)
        self.assertEqual(self.deployer.resolve_dependency("@rpath/other.dylib", copied,
                                                         ["@loader_path/../../.."]), neighbor)

    def test_copying_framework_preserves_resources_and_version_symlinks(self):
        original = binary(self.root / "SDK/F.framework/Versions/A/F")
        framework = deploy.framework_root(original)
        (framework / "Versions/Current").symlink_to("A")
        (framework / "F").symlink_to("Versions/Current/F")
        resources = framework / "Versions/A/Resources"
        resources.mkdir()
        (resources / "default.effect").write_text("shader")
        (framework / "Resources").symlink_to("Versions/Current/Resources")
        copied = self.deployer.copy_dependency(original)
        destination = deploy.framework_root(copied)
        self.assertEqual((destination / "Resources/default.effect").read_text(), "shader")
        self.assertTrue((destination / "Versions/Current").is_symlink())
        self.assertEqual(self.deployer.copy_dependency(original), copied)

    def test_different_libraries_with_same_name_cannot_silently_overwrite_each_other(self):
        first = binary(self.root / "prefix one/lib/same.dylib", b"one")
        second = binary(self.root / "prefix two/lib/same.dylib", b"two")
        self.deployer.copy_dependency(first)
        with self.assertRaisesRegex(RuntimeError, "Conflicting dylibs"):
            self.deployer.copy_dependency(second)

    def test_identical_libraries_from_different_prefixes_share_one_copy(self):
        first = binary(self.root / "prefix one/lib/same.dylib", b"same")
        second = binary(self.root / "prefix two/lib/same.dylib", b"same")
        self.assertEqual(self.deployer.copy_dependency(first), self.deployer.copy_dependency(second))

    def test_redeployment_refreshes_a_library_left_by_an_older_build(self):
        old = binary(self.app / "Contents/Frameworks/same.dylib", b"old")
        source = binary(self.root / "prefix/lib/same.dylib", b"new")
        copied = self.deployer.copy_dependency(source)
        self.assertEqual(copied, old)
        self.assertEqual(copied.read_bytes(), source.read_bytes())

    def test_missing_runtime_dependency_fails_with_the_binary_and_install_name(self):
        with self.assertRaisesRegex(RuntimeError, "missing.dylib.*Harpia"):
            self.deployer.resolve_dependency("@rpath/missing.dylib", self.executable, [])

    def test_external_bundle_symlinks_are_rejected_before_signing(self):
        outside = binary(self.root / "outside.dylib")
        (self.app / "Contents/external").symlink_to(outside)
        with self.assertRaisesRegex(RuntimeError, "symlink escapes"):
            self.deployer.verify_dependencies()

    def test_complete_dependency_chain_is_copied_and_rewritten_not_just_direct_dependencies(self):
        first = binary(self.root / "prefix/lib/first.dylib")
        second = binary(first.parent / "second.dylib")
        metadata = {
            self.executable: ([str(first)], [str(first.parent)], set()),
            first: (["@loader_path/second.dylib"], [], {str(first)}),
            second: (["/usr/lib/libSystem.B.dylib"], [], {str(second)}),
        }
        edits = []
        def read_metadata(path):
            path = Path(path).resolve()
            original = self.deployer.origins.get(path, path)
            deps, rpaths, ids = metadata.get(path, metadata[original])
            return list(deps), list(rpaths), set(ids)
        def command(arguments, **kwargs):
            if arguments[0] == "/usr/bin/lipo":
                return "arm64 x86_64"
            self.assertEqual(arguments[0], "/usr/bin/install_name_tool")
            path = Path(arguments[-1]).resolve()
            deps, rpaths, ids = read_metadata(path)
            if arguments[1] == "-change":
                deps = [arguments[3] if x == arguments[2] else x for x in deps]
            elif arguments[1] == "-id":
                ids = {arguments[2]}
            elif arguments[1] == "-delete_rpath":
                rpaths.remove(arguments[2])
            metadata[path] = (deps, rpaths, ids)
            edits.append(arguments)
            return ""
        with patch.object(self.deployer, "metadata", side_effect=read_metadata), patch.object(deploy, "run", side_effect=command):
            self.deployer.deploy_dependencies()
            self.deployer.verify_dependencies()
        self.assertTrue((self.app / "Contents/Frameworks/first.dylib").is_file())
        self.assertTrue((self.app / "Contents/Frameworks/second.dylib").is_file())
        self.assertTrue(any(x[1] == "-delete_rpath" for x in edits))
        self.assertTrue(all(dep.startswith("@loader_path/") or dep.startswith("/usr/lib/")
                            for path in deploy.macho_files(self.app) for dep in read_metadata(path)[0]))

    def test_signing_finishes_nested_code_before_signing_app_with_entitlements(self):
        binary(self.app / "Contents/PlugIns/P.plugin/Contents/MacOS/P")
        entitlements = SOURCE / "resources/macos/entitlements.plist"
        commands = []
        with patch.object(deploy, "run", side_effect=lambda args, **kwargs: commands.append(args) or ""):
            deploy.sign(self.app, "-", entitlements)
        sign_commands = [x for x in commands if "--sign" in x]
        self.assertEqual(sign_commands[-1][-1], str(self.app))
        self.assertIn("--entitlements", sign_commands[-1])
        self.assertTrue(any(x[-1].endswith("P.plugin") for x in sign_commands[:-1]))
        self.assertFalse(any("--timestamp" in x for x in sign_commands))
        self.assertIn("--verify", commands[-1])
        self.assertIn("--strict", commands[-1])

    def test_hardened_runtime_profile_does_not_disable_library_validation_or_enable_jit(self):
        with (SOURCE / "resources/macos/entitlements.plist").open("rb") as file:
            entitlements = plistlib.load(file)
        self.assertTrue(entitlements["com.apple.security.device.audio-input"])
        self.assertTrue(entitlements["com.apple.security.device.camera"])
        self.assertNotIn("com.apple.security.cs.disable-library-validation", entitlements)
        self.assertNotIn("com.apple.security.cs.allow-jit", entitlements)
        self.assertNotIn("com.apple.security.app-sandbox", entitlements)

    def test_local_signatures_clear_runtime_flags_on_every_nested_binary_and_bundle(self):
        # Real dependency shapes, including a previously vendor-signed dylib,
        # must all use the local policy when re-signing an incremental build.
        binary(self.app / "Contents/Frameworks/libavcodec.dylib")
        binary(self.app / "Contents/Frameworks/F.framework/Versions/A/F")
        binary(self.app / "Contents/PlugIns/P.plugin/Contents/MacOS/P")
        commands = []
        with patch.object(deploy, "run", side_effect=lambda args, **kwargs: commands.append(args) or ""):
            deploy.sign(self.app, "-", SOURCE / "resources/macos/entitlements.plist")
        signed = [command for command in commands if "--sign" in command]
        self.assertGreaterEqual(len(signed), 7)
        for command in signed:
            self.assertEqual(command[command.index("--options") + 1], "0")
            self.assertEqual(command[command.index("--sign") + 1], "-")
            self.assertNotIn("--timestamp", command)

    def test_developer_id_keeps_runtime_timestamp_and_same_identity_for_dependencies(self):
        binary(self.app / "Contents/Frameworks/libavcodec.dylib")
        identity = "Developer ID Application: Harpia Test (TESTTEAMID)"
        commands = []
        with patch.object(deploy, "run", side_effect=lambda args, **kwargs: commands.append(args) or ""):
            deploy.sign(self.app, identity, SOURCE / "resources/macos/entitlements.plist")
        signed = [command for command in commands if "--sign" in command]
        self.assertEqual(len(signed), 3)
        for command in signed:
            self.assertEqual(command[command.index("--options") + 1], "runtime")
            self.assertEqual(command[command.index("--sign") + 1], identity)
            self.assertIn("--timestamp", command)

    def test_runtime_entitlements_are_scoped_to_helpers_and_survive_redeployment(self):
        deno = binary(self.app / "Contents/Helpers/deno")
        ytdlp = binary(self.app / "Contents/Helpers/yt-dlp")
        args = SimpleNamespace(framework=[], plugin=[], library=[], helper=[], emoji=None,
                               emoji_license=None, yt_dlp=None, ffmpeg=None, deno=None)
        with patch.dict("os.environ", {}, clear=True), patch.object(deploy, "run", return_value="not signed"):
            deploy.stage(args, self.deployer)
        self.assertEqual(self.deployer.helper_entitlements[deno], {"com.apple.security.cs.allow-jit": True})
        self.assertEqual(self.deployer.helper_entitlements[ytdlp], {"com.apple.security.cs.disable-library-validation": True})
        self.assertNotIn(self.executable, self.deployer.helper_entitlements)

    def test_script_download_helper_cannot_be_mistaken_for_standalone_runtime(self):
        script = self.root / "yt-dlp"
        script.write_text("#!/usr/bin/python3\nprint('needs Python')\n")
        args = SimpleNamespace(framework=[], plugin=[], library=[], helper=[], emoji=None,
                               emoji_license=None, yt_dlp=script, ffmpeg=None, deno=None)
        with patch.dict("os.environ", {}, clear=True):
            with self.assertRaisesRegex(RuntimeError, "standalone Mac executable"):
                deploy.stage(args, self.deployer)


class TemporaryDirectoryTests(unittest.TestCase):
    def test_packaging_checks_accept_a_symlinked_system_temp_directory(self):
        # Reproduce macOS's /var -> /private/var on any host. Run only the
        # packaging cases in the child, so this regression cannot recurse.
        with tempfile.TemporaryDirectory(prefix="harpia temp alias ") as scratch:
            root = Path(scratch).resolve()
            physical = root / "physical"
            physical.mkdir()
            alias = root / "temporary directory link"
            alias.symlink_to(physical, target_is_directory=True)
            result = subprocess.run(
                [sys.executable, str(Path(__file__).resolve()), "PackagingTests"],
                env=dict(os.environ, TMPDIR=str(alias)), text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
