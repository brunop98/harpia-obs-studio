# Building and validating the macOS port

This branch implements the macOS work identified in [the source audit](MACOS_READINESS.md).
It has been built and regression-tested on Linux, including forced Mac GPU
surface ownership tests. A native Mac build, signed distribution and physical
screen/audio/camera checks have not been verified in this environment. The CI
workflow explicitly reports when its required Apple toolchain is unavailable.

## Build a development application

Requirements:

- macOS 13 or newer; Apple Silicon is the primary initial target.
- Full Xcode 26.5+ and macOS SDK 26.5+, as required by this checkout. This build
  SDK requirement is separate from the application's macOS 13 runtime minimum.
- CMake 3.28+, Python 3.10+ for the deployment scripts, and Git.
- FFmpeg CLI on PATH to generate/regression-test media (`brew install ffmpeg`).

For the Mac equivalent of `build-and-run-harpia.bat`, double-click
[`build-and-run-harpia.command`](scripts/build-and-run-harpia.command) in Finder,
or run:

```sh
./harpia/scripts/build-and-run-harpia.sh
./harpia/scripts/build-and-run-harpia.sh Debug
./harpia/scripts/build-and-run-harpia.sh --check-tools
```

The launcher finds compatible tools on PATH, in common Homebrew/MacPorts
locations, in `CMake.app`, and in Python framework installations. It checks full
Xcode and its SDK, compiler, first-run setup, CMake, Python, Git and bundle tools
before configuring. Missing or outdated tools produce download links and stop
the build. `--check-tools` performs discovery without building or launching.
The `.command` launcher keeps errors visible in an interactive Terminal window.

Tool downloads: [Xcode](https://developer.apple.com/xcode/),
[CMake](https://cmake.org/download/),
[Python](https://www.python.org/downloads/macos/),
[Git](https://git-scm.com/download/mac), and
[FFmpeg CLI](https://ffmpeg.org/download.html#build-mac) for `--test`.
Qt and the OBS/FFmpeg build libraries come from the pinned dependency archives.
That Qt SDK omits `macdeployqt` and QtTest. When needed, configuration downloads
the checksum-verified matching Qt 6.11.1 source and builds only those components
against the selected SDK. They are installed under the build directory's
`harpia-qt-tools` folder; the original Qt runtime is kept in its dependency
prefix. These component builds disable the SDK publisher's `QT_USE_CCACHE`
setting, so `ccache` is optional on developer and CI machines.
Sources are cached under `.deps/qt-tools`. A compatible complete local
qtbase tree can be supplied with the `HARPIA_QTBASE_SOURCE_DIR` CMake cache entry
for offline builds. Configure/build/install diagnostics for these components
are saved under `harpia-qt-tools/<version-and-key>/*.log`.

An explicit `DEVELOPER_DIR` is respected. Otherwise a compatible active or
installed Xcode is selected for this process. The selected SDK is passed to
CMake explicitly, including when the shell has an older `SDKROOT` value.
The selected Python is also used for CMake and packaging. Existing build caches
are retained; use `--build-dir PATH` for a separate checkout/toolchain build.

The `macos-ci` preset disables Xcode's compilation cache and clears any prior
CAS path in the build cache. This avoids Xcode's `CAS filesystem cannot be
initialized ... RootID does not exist` failure on hosted runners.

If the native CI build fails, its run summary shows the first compiler/build
diagnostic with context. Download the `Harpia-macOS-build-log` artifact for the
complete output, including errors printed much earlier than Xcode's final
`CompileC` / exit-code summary.

The launcher accepts the options below and opens the resulting `Harpia.app`.
For tests and a verified package without launching, use:

```sh
./harpia/scripts/build-macos.sh --arch arm64 --test --package --check-runtime
```

The build downloads the dependency versions/hashes in `CMakePresets.json`.
`harpia-deploy` builds the recorder and runtime modules, creates `Harpia.app`,
deploys Qt plus OBS/FFmpeg dependencies, repairs load paths and signs the nested
code/application. Re-running this target also refreshes plugin-only changes.
The development signing default is ad-hoc (`-`). These local builds explicitly
clear hardened runtime flags: ad-hoc signatures have no Team ID, so hardened
library validation can reject bundled FFmpeg/Qt libraries at launch. Builds
using a Developer ID identity retain hardened runtime and sign all nested code
with that identity. No system security settings need to be changed.

Screen capture is deferred until a recording/countdown starts, then released
after stopping, cancellation or a failed start. Keeping the recorder or editor
open while idle does not run a full-resolution ScreenCaptureKit video stream.

Open diagnostics with **Command+Shift+L** in the recorder or editor. If the UI
is busy, double-click `harpia/scripts/open-harpia-logs.command` in Finder; it
opens `~/Library/Application Support/harpia-recorder/logs` without launching
Harpia. The in-app viewer shows the last 512 KiB of large sessions to keep it
responsive; Open Log File gives access to the complete log. Large UI event-loop
delays are logged on Mac (sleep/wake can also produce these warnings).

Outputs:

- `build_macos/harpia/RelWithDebInfo/Harpia.app`
- ZIP, SHA-256 and runtime JSON under `build_macos/dist` when packaging/checking.

The package verifier relocates the app, checks Mach-O load commands, architecture
slices, minimum OS versions, required helpers/resources and signatures. With
`--check-runtime`, it launches that relocated app's `--check-runtime` mode to
load OBS and inspect the real capture/audio property callbacks. This mode does
not create recording sources or request screen/camera/microphone access.

Other options include `--configuration Debug`, `--arch x86_64`,
`--arch universal`, `--build-dir PATH`, and `--run`. Every embedded dependency
must support the selected architecture(s) and the advertised minimum macOS
version; deployment fails if it does not.

## Sign a distributable

Supply your own installed Developer ID identity and an existing notarization
profile; credentials are not stored in the repository:

```sh
CODESIGN_IDENT='Developer ID Application: Your Company (TEAMID)' \
HARPIA_NOTARY_PROFILE='your-existing-notary-profile' \
./harpia/scripts/build-macos.sh --arch arm64 --package --check-runtime
```

Packaging submits the ZIP with `notarytool`, checks the Accepted result, staples
the ticket, assesses Gatekeeper and recreates the ZIP/checksum. An ad-hoc build
is useful for development; it does not establish notarized distribution.
Keep the stable `com.harpia.recorder` bundle identity/team for permission and
Keychain continuity across releases.

Optional `HARPIA_YT_DLP_HELPER`, `HARPIA_FFMPEG_HELPER` and `HARPIA_DENO_HELPER` paths can
stage URL-download helpers under `Contents/Helpers` (environment variables read by
`harpia/scripts/deploy-macos.py`). Bundled helpers must be standalone Mac
Mach-O executables and satisfy the app's architecture/OS-floor checks. Python
or shell wrappers, including pip/Homebrew yt-dlp scripts, cannot be bundled this
way. External runtime discovery can use installed tools and their interpreters;
it also checks Homebrew locations and respects explicit user-selected tools
and command-line overrides.

## Implemented behavior

- Native display UUID enumeration avoids the former null-context SCK property
  query. Cursor settings use the Mac source's actual keys. Screen recording
  waits for a valid first frame and detects interrupted capture streams.
- Permission requests run only for requested screen/system audio, microphones
  and cameras, with System Settings guidance and regrant recovery. Audio-only
  recording does not request unused webcam access. Editor narration requests its
  own screen/microphone permission when needed.
- SCK system/app audio avoids virtual loopback devices and disables implicit
  audio on the screen source, preventing duplicate sound or PC-audio mute leaks.
  App filters use stable bundle identifiers and refresh on app lifecycle changes.
  Choose/start the target application before beginning an Only-app recording.
- Native picking/follow-window crops map logical Quartz coordinates into the
  selected display's captured pixels. Foreground app identity supports focus
  auto-pause. Global Carbon hotkeys support keyboard-layout changes and retain
  focused-window fallback when registration conflicts with another shortcut.
- Recording controls, framing, guides and dialogs are selectively excluded from
  SCK display capture while intended mouse effects remain visible.
- H.264/HEVC VideoToolbox encoders are advertised only after opening a hardware
  session. Auto uses constant quality where supported; otherwise it logs and uses
  a resolution/frame-rate-based bitrate target. Explicit bitrates remain target
  bitrates. CPU H.264 remains a fallback. New Mac presets prefer hardware.
- Editor MP4/MOV/MKV exports offer Automatic, Apple hardware and Software
  encoding. Automatic tries an actual VideoToolbox session, then reports any
  software fallback in progress and the session log. Lossless and 4:4:4 use
  software; an explicit incompatible hardware choice is rejected. Sharing and
  proxy encodes use the same automatic H.264 path. WebM/GIF keep their own
  encoders. Hardware quality is a separate scale and may produce different file
  sizes; native speed, power and output-quality measurements remain required.
- HEVC MP4/MOV recordings/remuxes use the `hvc1` tag for Apple playback; MKV and
  H.264 retain their container behavior.
- GUI-owned offscreen surfaces are leased by export workers without waiting for
  the GUI thread during joins. Audio preview negotiates supported formats and
  streams bounded conversion buffers; failures appear in the editor/log.
- API keys use the login Keychain, with legacy migration verified before removing
  old settings. Finder `.harpiaproj` events queue until the editor can open them,
  and reuse the normal relink/error workflow. Emoji resources use bundle paths.
- Editor shortcut tips use the current configured bindings and native modifier
  labels, including mouse gestures. Open options update after a rebind or reset.
- Failed or missing custom shader effects show a nonmodal preview warning with
  copyable details. Exports report the named effect and reason instead of
  silently omitting it. The same diagnostics are maintained on the main editor
  development branch for every platform.

## Automated checks

From the full application build:

```sh
cmake --preset macos -DHARPIA_BUILD_TESTS=ON -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build_macos --config RelWithDebInfo --target harpia-tests --parallel 4
ctest --test-dir build_macos -C RelWithDebInfo --output-on-failure --no-tests=error
```

The Qt/FFmpeg editor harness can also be configured directly, without libobs:

```sh
cmake -S harpia/tests -B build_editor_tests -DCMAKE_PREFIX_PATH=/path/to/qt-and-ffmpeg
cmake --build build_editor_tests --target harpia-tests --parallel 4
ctest --test-dir build_editor_tests --output-on-failure
```

CMake discovers Qt and FFmpeg, creates real media fixtures, and runs project,
preview, export, audio, text, GPU-lifetime, settings-isolation, Keychain-migration,
helper-discovery and packaging checks. Test settings use private INI files;
Mac editor tests use an in-memory Keychain backend. The production login
Keychain and Harpia preferences are not used for test storage.

## Native release acceptance still required

Run the actual Cocoa app from Finder on clean target Macs, not only headless
widget tests. Verify first-run denial/grant/regrant; signed app updates; Retina
and mixed-monitor capture/cropping/hot-plug; no recorded controls; mouse effects;
mic/system/Only/Except audio and app restarts; headphones/Bluetooth/USB changes;
webcam formats; Fn/system shortcut conflicts; sustained 1080p/4K recording,
pause/resume, A/V sync, cancellation/shutdown and interruption recovery.

Open projects moved from Windows and verify relinking, fonts, keyframes,
preview/export effects, MP4/MOV/WebM/GIF exports and Discord part sizes. Test
Keychain locked/denied states and URL downloads from a Finder-launched app with
no shell PATH. Repeat package/notarization/Gatekeeper validation on an app moved
outside the build tree. Intel/universal builds need their own native validation.
