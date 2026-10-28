#!/usr/bin/env python3
"""Deploy the actual OBS/Qt runtime, repair Mach-O dependencies and sign Harpia.

Called by the macOS CMake target. No development path is allowed to remain in a
load command. --verify-only also validates a relocated copy of the final app.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
from collections import deque

MACHO_MAGICS = {b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
                b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
                b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"}
SYSTEM_PREFIXES = ("/System/Library/", "/usr/lib/", "/Library/Apple/System/Library/")


def run(command: list[str], *, check: bool = True) -> str:
    completed = subprocess.run([str(p) for p in command], text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if check and completed.returncode:
        raise RuntimeError(f"Command failed ({completed.returncode}): {' '.join(map(str, command))}\n"
                           f"{completed.stdout}")
    return completed.stdout


def is_macho(path: Path) -> bool:
    try:
        with path.open("rb") as file:
            return file.read(4) in MACHO_MAGICS
    except (OSError, IsADirectoryError):
        return False


def macho_files(directory: Path) -> list[Path]:
    return sorted({file.resolve() for file in directory.rglob("*")
                   if file.is_file() and not file.is_symlink() and is_macho(file)})


def framework_root(path: Path) -> Path | None:
    return next((parent for parent in (path, *path.parents) if parent.suffix == ".framework"), None)


def parse_load_commands(output: str) -> tuple[list[str], list[str], set[str]]:
    """Read otool -l, including paths containing spaces and fat-binary repeats."""
    dependencies, rpaths, identities = [], [], set()
    command = ""
    for line in output.splitlines():
        stripped = line.strip()
        if stripped.startswith("cmd "):
            command = stripped[4:]
        elif stripped.startswith("path ") and command == "LC_RPATH":
            value = re.sub(r" \(offset \d+\)$", "", stripped[5:])
            if value not in rpaths:
                rpaths.append(value)
        elif stripped.startswith("name "):
            value = re.sub(r" \(offset \d+\)$", "", stripped[5:])
            if command == "LC_ID_DYLIB":
                identities.add(value)
            elif command in {"LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB",
                             "LC_LOAD_UPWARD_DYLIB", "LC_LAZY_LOAD_DYLIB"} and value not in dependencies:
                dependencies.append(value)
    return dependencies, rpaths, identities


def parse_minimum_versions(output: str) -> list[str]:
    versions = []
    command = ""
    for line in output.splitlines():
        stripped = line.strip()
        if stripped.startswith("cmd "):
            command = stripped[4:]
        elif (command == "LC_BUILD_VERSION" and stripped.startswith("minos ")) or (
                command == "LC_VERSION_MIN_MACOSX" and stripped.startswith("version ")):
            value = stripped.split()[1]
            if value not in versions:
                versions.append(value)
    return versions


def os_version(value: str) -> tuple[int, int, int]:
    parts = [int(part) for part in value.split(".")]
    if not parts or len(parts) > 3:
        raise ValueError(f"Invalid macOS version: {value}")
    return tuple((parts + [0, 0])[:3])


def inside(path: Path, directory: Path) -> bool:
    try:
        path.resolve().relative_to(directory.resolve())
        return True
    except ValueError:
        return False


def same_file_contents(first: Path, second: Path) -> bool:
    if first.stat().st_size != second.stat().st_size:
        return False
    def digest(path: Path) -> bytes:
        answer = hashlib.sha256()
        with path.open("rb") as file:
            for block in iter(lambda: file.read(1024 * 1024), b""):
                answer.update(block)
        return answer.digest()
    return digest(first) == digest(second)


class Deployer:
    def __init__(self, app: Path, search_dirs: list[Path], architectures: list[str]):
        self.app = app.resolve()
        self.macos = self.app / "Contents/MacOS"
        self.frameworks = self.app / "Contents/Frameworks"
        self.search_dirs = [path.resolve() for path in search_dirs if path.is_dir()]
        self.architectures = architectures
        self.copied: dict[Path, Path] = {}
        self.origins: dict[Path, Path] = {}
        self.destination_sources: dict[Path, Path] = {}
        self.newly_copied: set[Path] = set()
        self.helper_entitlements: dict[Path, dict[str, bool]] = {}
        self.minimum_versions: dict[Path, list[str]] = {}
        self.minimum_os: str | None = None

    def record_copy(self, source: Path, destination: Path) -> None:
        source, destination = source.resolve(), destination.resolve()
        self.copied[source] = destination
        self.origins[destination] = source
        self.destination_sources[destination] = source
        self.newly_copied.add(destination)

    def record_framework(self, source: Path, destination: Path) -> None:
        for binary in macho_files(destination):
            original = source / binary.relative_to(destination.resolve())
            self.record_copy(original, binary)

    def metadata(self, binary: Path) -> tuple[list[str], list[str], set[str]]:
        output = run(["/usr/bin/otool", "-l", str(binary)])
        self.minimum_versions[binary.resolve()] = parse_minimum_versions(output)
        return parse_load_commands(output)

    def expand_path(self, value: str, binary: Path) -> Path | None:
        if value.startswith("@loader_path/"):
            return binary.parent / value[len("@loader_path/"):]
        if value.startswith("@executable_path/"):
            # All OBS/Qt libraries are loaded by the main application. Helper
            # paths are rewritten to @loader_path during deployment as well.
            return self.macos / value[len("@executable_path/"):]
        if value.startswith("/"):
            return Path(value)
        return None

    def resolve_dependency(self, name: str, binary: Path, rpaths: list[str]) -> Path:
        candidates = []
        loaders = [binary]
        if binary.resolve() in self.origins:
            loaders.append(self.origins[binary.resolve()])
        for loader in loaders:
            expanded = self.expand_path(name, loader)
            if expanded:
                candidates.append(expanded)
        if name.startswith("@executable_path/") and len(loaders) > 1:
            candidates.append(loaders[-1].parent / name[len("@executable_path/"):])
        if name.startswith("@rpath/"):
            suffix = name[len("@rpath/"):]
            for entry in rpaths:
                for loader in loaders:
                    base = self.expand_path(entry, loader)
                    if base is not None:
                        candidates.append(base / suffix)
            candidates.append(self.frameworks / suffix)
            candidates.extend(directory / suffix for directory in self.search_dirs)
        elif not name.startswith("/"):
            candidates.append(binary.parent / name)
            candidates.extend(directory / name for directory in self.search_dirs)
        for candidate in candidates:
            if candidate.is_file():
                return candidate.resolve()
        raise RuntimeError(f"Cannot resolve {name!r}, needed by {binary}. "
                           "Supply the matching dependency prefix with --search-dir; "
                           "do not ship a bundle with missing dylibs.")

    def copy_dependency(self, source: Path) -> Path:
        source = source.resolve()
        if inside(source, self.app):
            return source
        if source in self.copied:
            return self.copied[source]
        framework = framework_root(source)
        if framework:
            destination_root = self.frameworks / framework.name
            destination = destination_root / source.relative_to(framework)
            previous = self.destination_sources.get(destination.resolve())
            if previous:
                if previous != source and not same_file_contents(source, previous):
                    raise RuntimeError(f"Conflicting frameworks named {framework.name}: {source} and {previous}")
                self.copied[source] = destination.resolve()
                return destination.resolve()
            replace_directory(framework, destination_root)
            self.record_framework(framework, destination_root)
        else:
            destination = self.frameworks / source.name
            previous = self.destination_sources.get(destination.resolve())
            if previous:
                if previous != source and not same_file_contents(source, previous):
                    raise RuntimeError(f"Conflicting dylibs named {source.name}: {source} and {previous}")
                self.copied[source] = destination.resolve()
                return destination.resolve()
            self.frameworks.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
            self.record_copy(source, destination)
        return destination.resolve()

    def deploy_dependencies(self) -> None:
        self.frameworks.mkdir(parents=True, exist_ok=True)
        pending = deque(macho_files(self.app))
        seen: set[Path] = set()
        while pending:
            binary = pending.popleft().resolve()
            if binary in seen and binary not in self.newly_copied:
                continue
            self.newly_copied.discard(binary)
            seen.add(binary)
            dependencies, rpaths, identities = self.metadata(binary)
            for name in dependencies:
                if name.startswith(SYSTEM_PREFIXES):
                    continue
                source = self.resolve_dependency(name, binary, rpaths)
                if str(source).startswith(SYSTEM_PREFIXES):
                    continue
                destination = self.copy_dependency(source)
                desired = "@loader_path/" + os.path.relpath(destination, binary.parent)
                if desired != name:
                    run(["/usr/bin/install_name_tool", "-change", name, desired, str(binary)])
                pending.append(destination)
                framework = framework_root(destination)
                if framework:
                    pending.extend(macho_files(framework))
            if identities and inside(binary, self.frameworks):
                identity = "@rpath/" + str(binary.relative_to(self.frameworks))
                if identities != {identity}:
                    run(["/usr/bin/install_name_tool", "-id", identity, str(binary)])
            # LC_RPATH values from the build machine are unnecessary after the
            # dependency closure is made loader-relative. Remove absolute ones.
            for rpath in rpaths:
                if rpath.startswith("/") and not rpath.startswith(SYSTEM_PREFIXES):
                    run(["/usr/bin/install_name_tool", "-delete_rpath", rpath, str(binary)])

    def verify_dependencies(self) -> None:
        for link in self.app.rglob("*"):
            if link.is_symlink() and not inside(link, self.app):
                raise RuntimeError(f"Bundle symlink escapes the application: {link}")
        binaries = macho_files(self.app)
        if not binaries:
            raise RuntimeError("The application contains no Mach-O executable")
        for binary in binaries:
            actual_arches = set(run(["/usr/bin/lipo", "-archs", str(binary)]).split())
            missing = set(self.architectures) - actual_arches
            if missing:
                raise RuntimeError(f"{binary} lacks requested architecture(s): {', '.join(sorted(missing))}")
            dependencies, rpaths, _ = self.metadata(binary)
            if self.minimum_os:
                for minimum in self.minimum_versions.get(binary.resolve(), []):
                    if os_version(minimum) > os_version(self.minimum_os):
                        raise RuntimeError(f"{binary} requires macOS {minimum}, above the app's "
                                           f"advertised minimum {self.minimum_os}")
            for name in dependencies:
                if name.startswith(SYSTEM_PREFIXES):
                    continue
                if name.startswith("/"):
                    raise RuntimeError(f"External absolute dependency remains in {binary}: {name}")
                resolved = self.resolve_dependency(name, binary, rpaths)
                if not inside(resolved, self.app):
                    raise RuntimeError(f"Dependency escapes the application: {binary}: {name} -> {resolved}")
            for rpath in rpaths:
                if rpath.startswith("/") and not rpath.startswith(SYSTEM_PREFIXES):
                    raise RuntimeError(f"Development-machine RPATH remains in {binary}: {rpath}")


def replace_directory(source: Path, destination: Path) -> None:
    if not source.is_dir():
        raise RuntimeError(f"Missing runtime bundle: {source}")
    if source.resolve() == destination.resolve():
        return
    if destination.exists():
        shutil.rmtree(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, destination, symlinks=True)


def stage(args: argparse.Namespace, deployer: Deployer) -> None:
    for source in args.framework:
        destination = deployer.frameworks / source.name
        replace_directory(source, destination)
        deployer.record_framework(source, destination)
    plugins = deployer.app / "Contents/PlugIns"
    for source in args.plugin:
        destination = plugins / source.name
        replace_directory(source, destination)
        for binary in macho_files(destination):
            deployer.record_copy(source / binary.relative_to(destination.resolve()), binary)
    for source in args.library:
        deployer.frameworks.mkdir(parents=True, exist_ok=True)
        destination = deployer.frameworks / source.name
        shutil.copy2(source, destination)
        deployer.record_copy(source, destination)
    for source in args.helper:
        if not source.is_file() or not is_macho(source):
            raise RuntimeError(f"Missing or non-native OBS helper: {source}")
        destination = deployer.macos / source.name
        shutil.copy2(source, destination)
        deployer.record_copy(source, destination)
    resources = deployer.app / "Contents/Resources"
    resources.mkdir(parents=True, exist_ok=True)
    if args.emoji:
        shutil.copy2(args.emoji, resources / "noto-emoji.pack")
        # Keep the current editor lookup working; Contents/MacOS is signed as
        # part of the app and the link contains no duplicate copy of the pack.
        alias = deployer.macos / "noto-emoji.pack"
        if alias.exists() or alias.is_symlink():
            alias.unlink()
        alias.symlink_to("../Resources/noto-emoji.pack")
    if args.emoji_license:
        shutil.copy2(args.emoji_license, resources / "noto-emoji-LICENSE.txt")
    for name in ("yt-dlp", "ffmpeg", "deno"):
        chosen = getattr(args, name.replace("-", "_"))
        configured = chosen or os.environ.get("HARPIA_" + name.upper().replace("-", "_") + "_HELPER")
        if not configured:
            continue
        source = Path(configured).expanduser().resolve()
        if not source.is_file() or not is_macho(source):
            raise RuntimeError(f"Bundled {name} must be a standalone Mac executable, not a script: {source}")
        helpers = deployer.app / "Contents/Helpers"
        helpers.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, helpers / name)
        deployer.record_copy(source, helpers / name)
        (helpers / name).chmod((helpers / name).stat().st_mode | 0o111)

    # Include helpers retained from an earlier incremental deployment too.
    for name in ("yt-dlp", "ffmpeg", "deno"):
        helper = deployer.app / "Contents/Helpers" / name
        if helper.is_file() and is_macho(helper):
            deployer.helper_entitlements[helper.resolve()] = helper_profile(helper, name)


def helper_profile(source: Path, name: str) -> dict[str, bool]:
    # Deno uses MAP_JIT. PyInstaller yt-dlp loads its embedded Python libraries
    # after extraction. Scope those capabilities to their helper processes.
    profile = {}
    original_profile = run(["/usr/bin/codesign", "-d", "--entitlements", "-", str(source)], check=False)
    start, end = original_profile.find("<?xml"), original_profile.rfind("</plist>")
    if start >= 0 and end >= start:
        original_profile = plistlib.loads(original_profile[start:end + len("</plist>")].encode())
        for key in ("com.apple.security.cs.allow-jit", "com.apple.security.cs.allow-unsigned-executable-memory",
                    "com.apple.security.cs.disable-library-validation"):
            if original_profile.get(key) is True:
                profile[key] = True
    if name == "deno":
        profile["com.apple.security.cs.allow-jit"] = True
    elif name == "yt-dlp":
        profile["com.apple.security.cs.disable-library-validation"] = True
    return profile


def verify_layout(deployer: Deployer) -> None:
    plist_path = deployer.app / "Contents/Info.plist"
    with plist_path.open("rb") as file:
        info = plistlib.load(file)
    for key in ("CFBundleIdentifier", "CFBundleExecutable", "CFBundleShortVersionString",
                "NSCameraUsageDescription", "NSMicrophoneUsageDescription", "LSMinimumSystemVersion"):
        if not info.get(key) or "@" in str(info[key]):
            raise RuntimeError(f"Missing or unconfigured Info.plist value: {key}")
    deployer.minimum_os = str(info["LSMinimumSystemVersion"])
    os_version(deployer.minimum_os)
    required = [deployer.macos / info["CFBundleExecutable"], deployer.macos / "obs-ffmpeg-mux",
                deployer.frameworks / "libobs.framework/Resources/default.effect",
                deployer.app / "Contents/Resources/noto-emoji.pack",
                deployer.app / "Contents/Resources/Harpia.icns",
                deployer.app / "Contents/PlugIns/platforms/libqcocoa.dylib"]
    for item in required:
        if not item.is_file():
            raise RuntimeError(f"Required runtime file is missing: {item}")
    for module in ("mac-capture", "mac-avcapture", "obs-ffmpeg", "obs-filters", "obs-x264"):
        bundle = deployer.app / f"Contents/PlugIns/{module}.plugin"
        if not any((bundle / "Contents/MacOS" / filename).is_file() for filename in (module, module + ".so")):
            raise RuntimeError(f"Required OBS plugin is missing: {module}")
        if not (bundle / "Contents/Resources").is_dir():
            raise RuntimeError(f"Required OBS plugin resources are missing: {module}")
    if not list(deployer.frameworks.glob("libobs-opengl*.dylib")):
        raise RuntimeError("The OBS OpenGL graphics module was not deployed")
    for kind in ("multimedia", "imageformats", "tls"):
        if not list((deployer.app / "Contents/PlugIns" / kind).glob("*.dylib")):
            raise RuntimeError(f"Qt {kind} plugins were not deployed")


def sign(app: Path, identity: str, entitlements: Path,
         helper_entitlements: dict[Path, dict[str, bool]] | None = None) -> None:
    # Ad-hoc signatures have no Team ID. Hardened runtime's library validation
    # can therefore reject even our freshly re-signed bundled FFmpeg/Qt code.
    # Explicitly clear the flags for local builds, including on incremental
    # rebuilds of a bundle previously signed with runtime. Distribution builds
    # keep hardened runtime and re-sign every dependency with the same identity.
    flags = ["--force", "--sign", identity, "--options", "0" if identity == "-" else "runtime"]
    if identity != "-":
        flags += ["--timestamp"]
    with tempfile.TemporaryDirectory(prefix="harpia-sign-") as signing:
        for index, binary in enumerate(sorted(macho_files(app), key=lambda p: len(p.parts), reverse=True)):
            profile = (helper_entitlements or {}).get(binary)
            scoped_flags = []
            if profile:
                path = Path(signing) / f"helper-{index}.plist"
                path.write_bytes(plistlib.dumps(profile))
                scoped_flags = ["--entitlements", str(path)]
            run(["/usr/bin/codesign", *flags, *scoped_flags, str(binary)])
    bundles = [p for p in app.rglob("*") if p.is_dir() and not p.is_symlink()
               and p.suffix in {".framework", ".plugin", ".bundle", ".app"}]
    for bundle in sorted(bundles, key=lambda p: len(p.parts), reverse=True):
        run(["/usr/bin/codesign", *flags, str(bundle)])
    run(["/usr/bin/codesign", *flags, "--entitlements", str(entitlements), str(app)])
    run(["/usr/bin/codesign", "--verify", "--deep", "--strict", "--verbose=2", str(app)])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--macdeployqt", type=Path)
    parser.add_argument("--identity", default="-")
    parser.add_argument("--entitlements", type=Path)
    for name in ("framework", "plugin", "library", "helper", "search-dir"):
        parser.add_argument("--" + name, type=Path, action="append", default=[])
    parser.add_argument("--architecture", action="append", default=[])
    parser.add_argument("--emoji", type=Path)
    parser.add_argument("--emoji-license", type=Path)
    for name in ("yt-dlp", "ffmpeg", "deno"):
        parser.add_argument("--" + name, type=Path)
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("Deployment and verification require macOS's native tools")
    app = args.app.resolve()
    if app.suffix != ".app" or not (app / "Contents/MacOS").is_dir():
        parser.error(f"Not a built application bundle: {app}")
    deployer = Deployer(app, args.search_dir, args.architecture)
    try:
        if not args.verify_only:
            if not args.macdeployqt or not args.entitlements:
                parser.error("deployment requires --macdeployqt and --entitlements")
            print("Harpia: staging OBS runtime and application resources", flush=True)
            stage(args, deployer)
            extra = [f"-executable={binary}" for binary in macho_files(app)
                     if binary.parent != deployer.macos or binary.name != "Harpia"]
            command = [str(args.macdeployqt), str(app), "-always-overwrite", "-no-strip", "-verbose=1", *extra]
            run(command)
            print("Harpia: copying and relocating the complete Mach-O dependency closure", flush=True)
            deployer.deploy_dependencies()
        verify_layout(deployer)
        deployer.verify_dependencies()
        if args.verify_only:
            run(["/usr/bin/codesign", "--verify", "--deep", "--strict", "--verbose=2", str(app)])
        else:
            print("Harpia: signing nested code and application", flush=True)
            sign(app, args.identity, args.entitlements, deployer.helper_entitlements)
        print(f"Harpia: verified self-contained application at {app}", flush=True)
        return 0
    except (OSError, RuntimeError, ValueError, plistlib.InvalidFileException) as error:
        print(f"Harpia deployment failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
