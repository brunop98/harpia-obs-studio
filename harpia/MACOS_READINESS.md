# macOS readiness audit

Audited 2026-10-24 against commit `2613c8fbc`. This is a source audit from a
Linux environment; no native Mac build, permission flow or device test was run.
The findings below distinguish confirmed implementation gaps from behavior that
still needs testing on a Mac.

Harpia has useful macOS dependencies and OBS capture plugins, but it is not
ready to distribute as a reliable Mac recorder. The Qt/FFmpeg editor provides a
reusable foundation. Recording, application deployment and several platform
features need dedicated macOS work.

## First-release blockers

### 1. Build and deploy a complete Harpia application bundle

**Confirmed gap.** [harpia/CMakeLists.txt](CMakeLists.txt#L23) creates a plain
executable. The previous OBS app-bundle setup was removed from
[the macOS helpers](../cmake/macos/helpers.cmake#L40). There is no Harpia
`MACOSX_BUNDLE` setup, application Info.plist, Mac icon, dependency deployment or
complete Mac packaging script. The runtime-target dependencies build plugins
and helpers, but do not embed them in an application.

Add a `Harpia.app` containing:

- `Contents/MacOS`: Harpia and the `obs-ffmpeg-mux` helper.
- `Contents/Frameworks`: libobs, the graphics module, Qt, FFmpeg and required
  transitive libraries, with correct install names and relative RPATHs.
- `Contents/PlugIns`: OBS `.plugin` bundles and their resources, plus the Qt
  platform, image and multimedia plugins in locations Qt can discover.
- Application resources, a Finder/Dock icon and the emoji pack. Align
  [emoji lookup](editor/timeline/Emoji.cpp#L58) with the chosen bundle location.

These locations matter at runtime: [libobs uses NSBundle](../libobs/obs-cocoa.m#L38)
to locate plugins and its engine resources, and the
[mux output](../plugins/obs-ffmpeg/obs-ffmpeg-mux.c#L274) expects its helper beside
the executable. Harpia's extra
[relative plugin registration](core/ObsContext.cpp#L72) should be removed or
corrected; libobs already supplies a proper bundle-based path.

Reuse the existing [Xcode signing settings](../cmake/macos/xcode.cmake#L21),
then add nested-code signing, notarization, stapling and a Harpia DMG/ZIP
workflow. The remaining [DMG layout script](../cmake/macos/resources/package.applescript#L31)
still refers to `OBS.app`. Verify an extracted download on a clean Mac without
the build tools or development dependencies installed.

### 2. Fix monitor probing, capture settings and stream readiness

**Confirmed null-dereference path in source; not reproduced on a Mac.**
[Monitor probing](core/CaptureManager.cpp#L65) calls
`obs_get_source_properties("screen_capture")`.
[libobs](../libobs/obs-source.c#L973) invokes the registered properties callback
with `NULL`. The [SCK callback](../plugins/mac-capture/mac-sck-video-capture.m#L557)
unconditionally reads `sc->capture_failed` before its null check. With that
source registered, monitor probing can therefore crash before returning a list.

Use native display/SCK enumeration or valid-instance source properties, and make
the plugin callbacks safe for a null context. SCK populates its display lists
through a live instance's
[content callback](../plugins/mac-capture/mac-sck-video-capture.m#L527), so fixing
the dereference or adding one key to the static probe is insufficient.

**Additional confirmed settings mismatch.**
[Display selection](core/CaptureManager.cpp#L43) probes `monitor_id`, `monitor`,
`screen` and `display`; the Mac plugins expose `display_uuid`. Even after the
crash is fixed, that mismatch prevents the chosen display from being supplied.
The [SCK defaults](../plugins/mac-capture/mac-sck-video-capture.m#L393) leave the
UUID empty; [display migration](../plugins/mac-capture/window-utils.m#L249)
then resolves to display ID zero. [Stream initialization](../plugins/mac-capture/mac-sck-video-capture.m#L105)
can report success while creating no stream for that invalid target. Current
[readiness checks](ui/MainWindow.cpp#L2663) do not reject an empty display list,
allowing a blank capture to appear ready once the earlier probing issue is
resolved.

Implement a Mac source-settings adapter that writes the display UUID, maps
cursor visibility to `show_cursor` rather than the current
[`capture_cursor`](core/CaptureManager.cpp#L198), and verifies permission plus
an actual captured frame before declaring the recorder ready. Detect capture
stream failure or revocation during a recording too.

### 3. Add native permissions and recovery

**Confirmed missing integration.** Harpia has no Mac application privacy plist
or dedicated screen/microphone/camera permission workflow.
[Camera authorization](platform/mac/MacCameraAccess.cpp#L5) always returns
`Unknown`; some [camera help](ui/PresetEditorDialog.cpp#L1095) points users to
Windows settings or Windows build components.

Add a stable bundle identifier, camera and microphone usage descriptions,
version-appropriate capture metadata and the capabilities needed by the chosen
signing/distribution model. Implement native permission checks and requests,
clear System Settings guidance, and recovery after denial, revocation and
regrant. Request additional Accessibility/Input Monitoring permissions only
for features that actually use the corresponding APIs. Test the signed app's
behavior across updates so users do not continually lose permission identity.

### 4. Connect native Mac system audio

**Confirmed feature gap.** [AudioManager](core/AudioManager.cpp#L26) selects
`coreaudio_output_capture`. That legacy source enumerates virtual audio devices
such as BlackHole/Loopback/Soundflower; it does not directly capture ordinary
Mac speaker output. See [its device filtering](../plugins/mac-capture/audio-device-enum.c#L8).

The vendored plugin already exposes
[`sck_audio_capture`](../plugins/mac-capture/mac-sck-audio-capture.m#L250) for
desktop and application audio on macOS 13+. Integrate its settings, app bundle
identifiers, audio routing and permission behavior into the recorder. The
current per-app source is [Windows-specific](core/AppAudioSource.cpp#L304).
The editor's [SystemAudioCapture](editor/SystemAudioCapture.cpp#L561) also
returns unavailable off Windows, so it needs a Mac backend separately.
The SCK audio properties callback has the same null-context assumption at
`sc->capture_failed`; correct it before exposing native audio through a static
properties probe.

Preserve microphone capture through CoreAudio/Qt, prevent self-monitoring
feedback or duplicate capture, and validate microphone/system audio sync and
independent tracks. This requires more than replacing one source ID.

## Reliability and feature parity

| Area | Evidence/status | Work needed |
| --- | --- | --- |
| Recording controls in output | [Controls](ui/RecorderControlsOverlay.cpp#L280) and [region frames](ui/RegionTool.cpp#L73) use Windows-only capture exclusion. Mac capture excludes no windows by default. | Add selective ScreenCaptureKit exclusions. Preserve intended mouse effects: excluding all Harpia windows would also remove the mouse-effect overlay. |
| Retina and multiple displays | [Qt/OBS display mapping](ui/MainWindow.cpp#L1437) uses native identity on Windows but falls back to list order on Mac. DPR arithmetic exists; a universal Retina failure is not proven. | Map display UUID/native IDs to Qt screens. Test mixed scaling, negative coordinates, rearrangement, hot-plug and region cropping in physical pixels. |
| Window picking/capture/follow | [MacWindowList](platform/mac/MacWindowList.cpp#L10) is a stub; [native window capture selection](core/CaptureManager.cpp#L31) returns null on Mac. | Implement native window enumeration/identity and OBS/SCK source settings, or explicitly disable unsupported modes. |
| Focus auto-pause | [MacForegroundWatcher](platform/mac/MacForegroundWatcher.cpp#L5) returns no process. | Implement foreground app tracking, stable app identity and pause/resume handling. |
| Global recording shortcuts | [GlobalHotkeys](core/GlobalHotkeys.cpp#L110) falls back to focused-window shortcuts outside Windows. | Add a Mac global-hotkey backend, conflict handling and native key labels; test F-key/system shortcut conflicts. |
| Click ripples | [MouseFxOverlay](ui/MouseFxOverlay.cpp#L95) detects mouse buttons only on Windows. | Implement global Mac click events with the appropriate permission behavior, or hide this option. |
| Hardware recording encoder | [EncoderFactory](core/EncoderFactory.cpp#L25) lists NVENC/AMF/QSV/VAAPI, not Apple VideoToolbox; [plugin selection](../plugins/CMakeLists.txt#L46) has no Apple encoder plugin. | Add VideoToolbox integration and encoder-specific quality controls. Current fallback is software H.264; HEVC has no software fallback. Measure 1080p/4K throughput, battery use and heat. |
| Editor/export GPU resources | [ShaderRenderer](editor/shader/ShaderRenderer.cpp#L143), [GpuFxGl](editor/shader/GpuFxGl.cpp#L234) and [SpotlightGl](editor/shader/SpotlightGl.cpp#L185) create offscreen surfaces on their calling thread, including export workers. | Follow Qt's GUI-thread creation/destruction requirement for window-backed offscreen surfaces while keeping rendering contexts on their worker. Reproduce and verify on Mac; no Mac crash was reproduced in this audit. |
| Custom shader failure | [ShaderComponent](editor/component/ShaderComponent.cpp#L107) passes the frame through when rendering fails. | Report unavailable custom effects clearly; verify preview/export parity. Built-in effects have CPU fallbacks. |
| API-key storage | [SecretStore](editor/subtitles/SecretStore.cpp#L23) uses reversible XOR/base64 outside Windows. | Add macOS Keychain storage and migrate existing saved keys. |
| URL-download helpers | [YtDlp](editor/ytdlp/YtDlp.cpp#L343) searches an override or PATH. Finder launch may lack Homebrew tool paths. | Deploy architecture-matched helpers or discover explicit paths for yt-dlp, FFmpeg CLI and required JavaScript runtimes. Test from Finder without a shell environment. Keep cookie-file fallback and add Mac-specific cookie/Keychain error guidance. |
| Media moved from Windows | [Project saving](editor/VideoEditorWindow.cpp#L12957) retains native source paths. Manual relinking already exists. | Test Windows-to-Mac relinking and consider portable relative references/media collection. Verify fonts and caption layout across machines. |
| Audio-device compatibility | [AudioPreview](editor/AudioPreview.cpp#L98) can silently decline devices without float stereo 48 kHz support. | Test built-in, Bluetooth and USB devices, reconnect/default-device changes and format conversion; expose failures and use a supported-format fallback where needed. |

OpenGL deprecation alone does not establish that the application needs a Metal
rewrite before its first functional Mac release. The repository has a Mac
OpenGL backend and transitive OpenGL linkage. Validate graphics-context creation,
effects, export and performance on real target Macs, then decide whether a Metal
migration is necessary for the supported OS versions and performance goals.

## Build policy and validation

Choose the support policy before implementing compatibility layers:

- **Recommended first target: native Apple Silicon, macOS 13 or newer.** The
  current [preset](../CMakePresets.json#L45) targets macOS 12.0, but its selected
  [`screen_capture` source](core/CaptureManager.cpp#L18) is only registered from
  [12.5](../plugins/mac-capture/mac-sck-common.m#L3), and native SCK audio is
  [registered from 13.0](../plugins/mac-capture/plugin-main.c#L22). Supporting
  earlier versions requires explicit video/audio fallbacks and limitations.
- The [toolchain checks](../cmake/macos/compilerconfig.cmake#L8) require the
  Xcode generator and Xcode/macOS SDK **26.5**. Provision that toolchain or
  deliberately review and retarget those requirements against the APIs in this
  checkout. The build SDK requirement is separate from the runtime OS minimum.
- Universal dependency archives do not make Harpia universal. The current
  preset builds one architecture. If Intel support is desired, build/test an
  Intel artifact or universal2 app and verify every embedded binary's slices.
- Verify the Mac dependency archive actually supplies required FFmpeg codecs
  and filters: editor/share MP4 uses libx264/AAC; WebM additionally needs VP9.
  Missing codecs are a validation risk, not a demonstrated archive defect.

On a Mac with the required toolchain, the existing starting point is:

```sh
cmake --preset macos -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build_macos --config RelWithDebInfo --target harpia-recorder
```

These commands build the current targets; the application deployment work above
is still required to produce a distributable.

Add native macOS build/package CI and a portable test runner. The existing
[A/V runner](tests/run_avsync.sh#L177) uses `nproc` and GNU linker group flags
that stock macOS does not support. Both runners also have Linux-specific `moc`
fallback paths. CMake/CTest targets would allow consistent Qt discovery,
compiler selection and linking. Headless unit checks should be supplemented by
real Cocoa, graphics, permission and audio tests.

Release acceptance should include:

1. Finder launch from a downloaded, signed/notarized app on a clean Mac;
   plugins, engine effects, Qt plugins, emoji and mux helper are found.
2. First-run permission grant, refusal, regrant and revocation; useful errors
   instead of blank captures or silent audio.
3. Full-display and region recording on built-in Retina and external monitors;
   cursor visibility, display switching, resize/hot-plug and overlay exclusion.
4. Microphone, system and per-app audio, separate tracks and sustained A/V sync;
   headphones/Bluetooth/USB device changes.
5. MP4 recording with pause/resume and interruption recovery; hardware encoding
   performance, file size, CPU load, heat and memory over a sustained session.
6. Project/media loading, multi-clip editing, images, audio, keyframes, captions,
   text styles and preview/export parity, including GPU/custom effects.
7. Export/cancel/failure cleanup for MP4/WebM/GIF, batch export and Discord
   49 MB parts; verify actual codecs and durations.
8. Recent projects, relinking Windows-created projects, downloaded URL media,
   helper discovery, native shortcuts, Finder reveal and Trash.

## Suggested implementation order

1. Establish the native Mac build and a deployed `.app` with stable identity,
   dependencies, permission metadata and a basic recording smoke test.
2. Fix the monitor-probing crash path, display/cursor settings, stream readiness
   and native permissions; get a reliable screen-plus-microphone recording.
3. Integrate SCK system/application audio and fix display mapping and overlay
   exclusion. Add VideoToolbox for sustained recording performance.
4. Validate/fix editor GPU and audio-device behavior, helper discovery and
   Keychain storage; implement or clearly gate the remaining platform features.
5. Complete automated/native test coverage and signed/notarized distribution
   validation before advertising Mac support.

Existing pieces to retain include Mac idle detection, the CoreAudio microphone
and AVCapture webcam source IDs, Qt/FFmpeg editor logic, writable
`QStandardPaths` locations, native Finder reveal and Qt Trash support. Qt's
standard shortcuts and `ControlModifier` already map to Command appropriately
on macOS; hard-coded `Ctrl+...` explanatory text still needs native labels.
