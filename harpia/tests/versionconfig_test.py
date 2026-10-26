#!/usr/bin/env python3
"""Configure real CMake projects from isolated Git checkouts and source archives.

No Apple SDK, compiler, downloads or upstream release tags are required. Set
HARPIA_TEST_CMAKE to select CMake explicitly instead of finding it on PATH.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


VERSION_MODULE = Path(__file__).resolve().parents[2] / "cmake/common/versionconfig.cmake"


class VersionConfigurationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = shutil.which(os.environ.get("HARPIA_TEST_CMAKE", "cmake"))
        cls.git = shutil.which("git")
        if not cls.cmake:
            raise RuntimeError("CMake 3.28+ is required; install it or set HARPIA_TEST_CMAKE.")
        if not cls.git:
            raise RuntimeError("Git is required for the isolated version-configuration checks.")

    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="harpia version with spaces ")
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name).resolve()
        self.source = self.root / "source checkout"
        self.source.mkdir()
        self.build = self.root / "build output"
        self.environment = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                                GIT_TERMINAL_PROMPT="0")
        for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY", "GIT_COMMON_DIR"):
            self.environment.pop(name, None)
        self.environment.update(GIT_AUTHOR_NAME="Harpia version test", GIT_AUTHOR_EMAIL="test@example.invalid",
                                GIT_COMMITTER_NAME="Harpia version test", GIT_COMMITTER_EMAIL="test@example.invalid")
        # Include the production module directly; a numeric project() version
        # must be accepted before the probe writes any result file.
        (self.source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.28)\n"
            "set(_obs_default_version 0 0 1)\n"
            "set(_obs_release_candidate 0)\n"
            "set(_obs_beta 0)\n"
            f"include([===[{VERSION_MODULE.as_posix()}]===])\n"
            "project(version_probe VERSION ${OBS_VERSION_CANONICAL} LANGUAGES NONE)\n"
            "file(WRITE \"${CMAKE_BINARY_DIR}/version-results.txt\"\n"
            "  \"canonical=${OBS_VERSION_CANONICAL}\\n\"\n"
            "  \"display=${OBS_VERSION}\\n\"\n"
            "  \"major=${OBS_VERSION_MAJOR}\\n\"\n"
            "  \"minor=${OBS_VERSION_MINOR}\\n\"\n"
            "  \"patch=${OBS_VERSION_PATCH}\\n\"\n"
            "  \"rc=${OBS_RELEASE_CANDIDATE}\\n\"\n"
            "  \"beta=${OBS_BETA}\\n\"\n"
            "  \"project=${PROJECT_VERSION}\\n\")\n"
        )
        (self.source / "payload.txt").write_text("initial revision\n")

    def git_command(self, *arguments, cwd=None):
        result = subprocess.run([self.git, *arguments], cwd=cwd or self.source, env=self.environment,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.strip()

    def initialize_git(self):
        self.git_command("init", "--quiet")
        self.git_command("add", "CMakeLists.txt", "payload.txt")
        self.git_command("commit", "--quiet", "-m", "initial fixture")

    def commit_revision(self):
        with (self.source / "payload.txt").open("a") as payload:
            payload.write("another revision\n")
        self.git_command("add", "payload.txt")
        self.git_command("commit", "--quiet", "-m", "next fixture revision")

    def describe(self):
        return self.git_command("describe", "--always", "--tags", "--dirty=-modified")

    def configure(self, *definitions, succeeds=True):
        result = subprocess.run([self.cmake, "-S", str(self.source), "-B", str(self.build), *definitions],
                                env=self.environment, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=30)
        if not succeeds:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            return result.stdout
        self.assertEqual(result.returncode, 0, result.stdout)
        values = dict(line.split("=", 1) for line in
                      (self.build / "version-results.txt").read_text().splitlines())
        self.assertEqual(values["project"], values["canonical"], result.stdout)
        self.assertEqual(".".join(values[key] for key in ("major", "minor", "patch")), values["canonical"])
        return values

    def fake_git(self, stdout="", stderr="", code=0):
        # The selected tool's distinct version/error proves it was invoked;
        # actual repositories and tags are used in all discovery cases.
        if os.name == "nt":
            path = self.root / "selected git.cmd"
            lines = ["@echo off"]
            if stdout:
                lines.append("echo " + stdout)
            if stderr:
                lines.append("echo " + stderr + " 1>&2")
            lines.append("exit /b " + str(code))
        else:
            path = self.root / "selected git"
            lines = ["#!/bin/sh"]
            if stdout:
                lines.append("printf '%s\\n' '" + stdout + "'")
            if stderr:
                lines.append("printf '%s\\n' '" + stderr + "' >&2")
            lines.append("exit " + str(code))
        path.write_text("\n".join(lines) + "\n")
        path.chmod(0o755)
        return path

    def test_source_archive_uses_numeric_backend_default(self):
        values = self.configure()
        self.assertEqual(values["canonical"], "0.0.1")
        self.assertEqual(values["display"], "0.0.1")
        self.assertEqual((values["rc"], values["beta"]), ("0", "0"))

    def test_untagged_checkout_keeps_hash_out_of_project_version(self):
        self.initialize_git()
        for dirty in (False, True):
            with self.subTest(dirty=dirty):
                if dirty:
                    with (self.source / "payload.txt").open("a") as payload:
                        payload.write("uncommitted edit\n")
                revision = self.describe()
                values = self.configure()
                self.assertEqual(values["canonical"], "0.0.1")
                self.assertEqual(values["display"], "0.0.1-g" + revision)

    def test_shallow_checkout_without_reachable_tag_uses_default(self):
        self.initialize_git()
        self.git_command("tag", "32.2.1")
        self.commit_revision()
        shallow = self.root / "shallow source checkout"
        self.git_command("clone", "--quiet", "--depth", "1", self.source.as_uri(), str(shallow), cwd=self.root)
        self.source = shallow
        self.assertEqual(self.git_command("rev-parse", "--is-shallow-repository"), "true")
        self.assertEqual(self.git_command("tag", "--list"), "")
        revision = self.describe()
        values = self.configure()
        self.assertEqual(values["canonical"], "0.0.1")
        self.assertEqual(values["display"], "0.0.1-g" + revision)

    def test_git_worktree_file_is_supported(self):
        self.initialize_git()
        worktree = self.root / "linked worktree"
        self.git_command("worktree", "add", "--quiet", "--detach", str(worktree), "HEAD")
        self.source = worktree
        self.assertTrue((worktree / ".git").is_file())
        revision = self.describe()
        values = self.configure()
        self.assertEqual(values["canonical"], "0.0.1")
        self.assertEqual(values["display"], "0.0.1-g" + revision)

    def test_release_tags_with_optional_v_have_numeric_metadata(self):
        self.initialize_git()
        for tag in ("32.2.1", "v32.2.1"):
            with self.subTest(tag=tag):
                self.git_command("tag", tag)
                values = self.configure()
                self.assertEqual(values["canonical"], "32.2.1")
                self.assertEqual(values["display"], "32.2.1")
                self.assertEqual((values["rc"], values["beta"]), ("0", "0"))
                self.git_command("tag", "--delete", tag)

    def test_prerelease_metadata_survives_distance_and_dirty_suffix(self):
        self.initialize_git()
        for tag, rc, beta in (("31.0.0-rc2", "2", "0"), ("31.0.0-beta3", "0", "3")):
            with self.subTest(tag=tag):
                self.git_command("tag", tag)
                self.commit_revision()
                with (self.source / "payload.txt").open("a") as payload:
                    payload.write("uncommitted edit\n")
                description = self.describe()
                self.assertTrue(description.startswith(tag + "-1-g"))
                self.assertTrue(description.endswith("-modified"))
                values = self.configure()
                self.assertEqual(values["canonical"], "31.0.0")
                self.assertEqual(values["display"], description)
                self.assertEqual((values["rc"], values["beta"]), (rc, beta))
                self.git_command("restore", "payload.txt")
                self.git_command("tag", "--delete", tag)

    def test_nonsemantic_tag_uses_backend_default(self):
        self.initialize_git()
        self.git_command("tag", "harpia-preview")
        self.assertEqual(self.describe(), "harpia-preview")
        values = self.configure()
        self.assertEqual(values["canonical"], "0.0.1")
        self.assertEqual(values["display"], "0.0.1")

    def test_valid_explicit_override_bypasses_git(self):
        self.initialize_git()
        broken_git = self.fake_git(stderr="Git must not be called for an override", code=7)
        for override, canonical, rc in (("0.0.1-cloud", "0.0.1", "0"), ("31.0.0-rc2", "31.0.0", "2")):
            with self.subTest(override=override):
                values = self.configure("-DOBS_VERSION_OVERRIDE=" + override, "-DGIT_EXECUTABLE=" + str(broken_git))
                self.assertEqual(values["canonical"], canonical)
                self.assertEqual(values["display"], override)
                self.assertEqual(values["rc"], rc)

    def test_invalid_explicit_override_fails_before_project(self):
        for override in ("2fe535b", "1.2", "v31.0.0"):
            with self.subTest(override=override):
                output = self.configure("-DOBS_VERSION_OVERRIDE=" + override, succeeds=False)
                self.assertIn("Invalid version supplied", output)
                self.assertNotIn("list index", output)
                self.assertFalse((self.build / "version-results.txt").exists())

    def test_selected_git_accepts_success_with_stderr_warning(self):
        self.initialize_git()
        selected = self.fake_git(stdout="33.4.5", stderr="fixture warning on successful discovery")
        values = self.configure("-DGIT_EXECUTABLE=" + str(selected))
        self.assertEqual(values["canonical"], "33.4.5")
        self.assertEqual(values["display"], "33.4.5")

    def test_selected_git_failure_preserves_diagnostic(self):
        self.initialize_git()
        selected = self.fake_git(stderr="intentional Git discovery failure", code=7)
        output = self.configure("-DGIT_EXECUTABLE=" + str(selected), succeeds=False)
        self.assertIn("Could not fetch OBS version tag from git (7)", output)
        self.assertIn("intentional Git discovery failure", output)
        self.assertNotIn("list index", output)

    def test_real_git_failure_is_not_reported_as_a_version_hash(self):
        self.git_command("init", "--quiet")
        output = self.configure(succeeds=False)
        self.assertIn("Could not fetch OBS version tag from git", output)
        self.assertNotIn("list index", output)
        self.assertFalse((self.build / "version-results.txt").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
