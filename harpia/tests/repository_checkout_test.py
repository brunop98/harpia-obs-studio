#!/usr/bin/env python3
"""Check the submodule metadata needed by a fresh GitHub Actions checkout.

No submodule is initialized or downloaded. The temporary repositories reproduce
checkout's recursive Git cleanup with an uninitialized, synthetic gitlink.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]
SYNTHETIC_COMMIT = "1" * 40
MODULE_PATH = "deps/capture source/src"


def git(repository, *arguments):
    environment = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                       GIT_TERMINAL_PROMPT="0")
    return subprocess.run(["git", *arguments], cwd=repository, env=environment,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)


def require_success(result):
    if result.returncode:
        raise AssertionError(result.stderr.decode(errors="replace"))
    return result.stdout


def committed_gitlinks(repository):
    records = require_success(git(repository, "ls-tree", "-r", "-z", "HEAD")).split(b"\0")
    return {os.fsdecode(record.partition(b"\t")[2]) for record in records
            if record.startswith(b"160000 ")}


def mappings(repository):
    configuration = Path(repository) / ".gitmodules"
    if not configuration.exists():
        return []
    result = git(repository, "config", "--null", "--file", str(configuration),
                 "--get-regexp", r"^submodule\..*\.path$")
    if result.returncode == 1:
        return []
    records = require_success(result).split(b"\0")
    entries = []
    for record in records:
        if not record:
            continue
        key, _, path = record.partition(b"\n")
        prefix = key.decode().removesuffix(".path")
        url = git(repository, "config", "--file", str(configuration), "--get", prefix + ".url")
        if url.returncode not in (0, 1):
            require_success(url)
        entries.append((os.fsdecode(path), url.stdout.decode().strip()))
    return entries


def mapping_errors(links, entries):
    paths = [path for path, _ in entries]
    errors = []
    for path in sorted(links):
        if paths.count(path) != 1:
            errors.append(f"Expected one .gitmodules mapping for tracked gitlink {path!r}")
    for path, url in entries:
        if path not in links:
            errors.append(f"Stale .gitmodules mapping has no tracked gitlink: {path!r}")
        if not url:
            errors.append(f"Submodule {path!r} has no URL")
    return errors


class RepositoryCheckoutTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="harpia checkout with spaces ")
        self.addCleanup(scratch.cleanup)
        self.fixture = Path(scratch.name)
        require_success(git(self.fixture, "init", "--quiet"))
        require_success(git(self.fixture, "update-index", "--add", "--cacheinfo",
                            "160000," + SYNTHETIC_COMMIT + "," + MODULE_PATH))
        (self.fixture / MODULE_PATH).mkdir(parents=True)

    def write_mapping(self, path=MODULE_PATH, url="https://example.invalid/capture.git"):
        configuration = str(self.fixture / ".gitmodules")
        require_success(git(self.fixture, "config", "--file", configuration,
                            "submodule.capture.path", path))
        if url is not None:
            require_success(git(self.fixture, "config", "--file", configuration,
                                "submodule.capture.url", url))

    def test_committed_gitlinks_have_exact_working_tree_mappings(self):
        self.assertEqual(mapping_errors(committed_gitlinks(REPOSITORY), mappings(REPOSITORY)), [])

    def test_checkout_cleanup_can_walk_the_repository_without_initializing_submodules(self):
        for command in (("submodule", "status"),
                        ("submodule", "foreach", "--recursive", "git", "rev-parse", "--is-inside-work-tree")):
            with self.subTest(command=command):
                result = git(REPOSITORY, *command)
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))

    def test_missing_mapping_reproduces_git_exit_128(self):
        for command in (("submodule", "status"), ("submodule", "foreach", "--recursive", "true")):
            with self.subTest(command=command):
                result = git(self.fixture, *command)
                self.assertEqual(result.returncode, 128)
                self.assertIn(b".gitmodules", result.stderr)

    def test_restored_mapping_fixes_status_and_recursive_cleanup_without_a_download(self):
        self.write_mapping()
        self.assertEqual(mapping_errors({MODULE_PATH}, mappings(self.fixture)), [])
        status = git(self.fixture, "submodule", "status")
        self.assertEqual(status.returncode, 0, status.stderr.decode(errors="replace"))
        self.assertTrue(status.stdout.startswith(("-" + SYNTHETIC_COMMIT + " ").encode()))
        cleanup = git(self.fixture, "submodule", "foreach", "--recursive", "git", "rev-parse",
                      "--is-inside-work-tree")
        self.assertEqual(cleanup.returncode, 0, cleanup.stderr.decode(errors="replace"))
        self.assertFalse((self.fixture / MODULE_PATH / ".git").exists())
        self.assertFalse((self.fixture / ".git/modules").exists())

    def test_a_mapping_for_another_path_cannot_hide_an_unmapped_gitlink(self):
        self.write_mapping(path="deps/a different source")
        errors = mapping_errors({MODULE_PATH}, mappings(self.fixture))
        self.assertEqual(len(errors), 2)
        self.assertIn(MODULE_PATH, "\n".join(errors))

    def test_a_mapping_without_a_url_is_incomplete(self):
        self.write_mapping(url=None)
        self.assertEqual(mapping_errors({MODULE_PATH}, mappings(self.fixture)),
                         [f"Submodule {MODULE_PATH!r} has no URL"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
