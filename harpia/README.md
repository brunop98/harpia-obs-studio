# Harpia Recorder

A minimal, fast screen recorder built directly on the **libobs** backend. It
reuses OBS's capture, encoder, and output plugins but ships its own tiny Qt
frontend instead of the full OBS Studio UI — inspired by PowerRec, trimmed to a
one-click recording workflow.

This is the **MVP**: a working record/pause/stop loop with a recent-recordings
list. The architecture is deliberately modular so the remaining features slot in
without refactoring.

## Building

Harpia builds as part of the OBS Studio CMake project. It is enabled by the
`ENABLE_HARPIA` option (ON by default).

### Prerequisites (Windows, install once)

- **Visual Studio 2022** with the **Desktop development with C++** workload
- **CMake ≥ 3.28** — `winget install Kitware.CMake`, then **reopen the terminal**
- **Git**
- *Optional, for webcam + Intel QSV:* the **C++ ATL for latest v143 build tools
  (x86 & x64)** VS component. `win-dshow` (webcam) and `obs-qsv11` (QSV encoder)
  require ATL; without it the build **auto-skips just those two** and everything
  else still builds (screen recording works, webcam/QSV are unavailable until you
  install ATL and reconfigure).

You do *not* need to install Qt6/obs-deps manually — the CMake configure step
downloads the prebuilt dependencies from `buildspec.json` automatically.

> **Slimmed tree:** this repo has been trimmed to build only the recorder. The
> stock OBS Studio frontend, scripting, streaming stack, and unused plugins
> (capture cards, cameras, browser, websocket, VST, transitions, …) are removed.
> Only `libobs` + a minimal plugin set (screen/audio capture, x264/NVENC/QSV/AMF/
> VideoToolbox encoders, ffmpeg muxer/output, crop filter) is built. As a result
> **stock `obs-studio` no longer builds from this tree** — recover it from git
> history if ever needed.

> **Only want to RUN Harpia on another PC?** Don't build there — that second
> machine needs no toolchain at all. Build once on a machine that has Visual
> Studio, with `-Package` (see *Making a distributable* below), and copy the
> `build_x64\dist\Harpia` folder across. Running `Build-Harpia.ps1` on a PC
> without the C++ toolset is what produces CMake's confusing
> *"Visual Studio 17 2022 … could not find any instance of Visual Studio"* —
> the script now says so up front rather than letting that be the first thing
> you see.

### Quick build (recommended)

```powershell
# From the repo root, in PowerShell:
.\harpia\scripts\Build-Harpia.ps1
```

This configures (fetching deps on first run) and builds just `harpia-recorder`,
then prints the path to `harpia.exe`. Options: `-Configuration Release`,
`-Reconfigure`, `-Target all` (whole solution).

### Manual build

The repo's `windows-x64` **preset pins a specific Visual Studio generator and
Windows SDK** that may not match your install, so configure `build_x64` directly
and let CMake pick your VS (this is what `Build-Harpia.ps1` does):

```powershell
# Fresh configure (auto-download deps). -A x64; add -G "Visual Studio 17 2022" if
# CMake picks the wrong VS.
cmake -S . -B build_x64 -A x64 -DENABLE_NEW_MPEGTS_OUTPUT=OFF -DENABLE_BROWSER=OFF

# Reconfigure an existing tree (reuses the cache's generator — omit -G/-A):
cmake -S . -B build_x64

cmake --build build_x64 --config RelWithDebInfo --target harpia-recorder
```

The binary lands at
`build_x64\rundir\RelWithDebInfo\bin\64bit\harpia.exe`.

### Runtime dependencies (do they auto-install?)

- **At build time**, the prebuilt libraries (Qt6, FFmpeg, obs-deps) are downloaded
  automatically by CMake's `buildspec` — you don't install them by hand. The
  toolchain (CMake, VS2022) is the only thing you install yourself.
- **At run time**, nothing auto-installs. `harpia.exe` needs its DLLs, the OBS
  plugins, and the Visual C++ runtime present. Running from the build `rundir`
  works because everything is already there.
- **On first launch**, Harpia runs a **dependency self-check**: if a required
  plugin (screen capture, x264, AAC, ffmpeg muxer) didn't load, it shows a clear
  message and logs it — instead of failing cryptically only when you hit Record.

### Making a distributable (run on another machine)

```powershell
.\harpia\scripts\Build-Harpia.ps1 -Package
```

This builds, then assembles `build_x64\dist\Harpia\` containing everything needed:
`bin\64bit` (the exe, all DLLs, the `obs-ffmpeg-mux` helper, Qt `platforms\`),
`obs-plugins\64bit`, and `data\`, plus the Visual C++ runtime bundled next to the
exe. Zip that folder to share it.

The recorder needs the OBS plugins (capture/encoders/ffmpeg muxer) available at
runtime. Because it builds inside the OBS tree, the plugins are produced
alongside it; `ObsContext` adds the standard OBS `obs-plugins/64bit` search
paths, and you can override them with the `OBS_PLUGINS_PATH` /
`OBS_PLUGINS_DATA_PATH` environment variables when running from a build tree.

## Architecture

```
harpia/
  main.cpp                  QApplication + backend bootstrap
  core/                     libobs-facing logic (no Qt)
    ObsContext              startup/shutdown, module load, video/audio graph
    CaptureManager          display capture source + region crop, output channel 0
    EncoderFactory          Preset -> encoder ids + output/container
    RecordingController     build pipeline, start/stop/pause/resume
  model/
    Preset / PresetStore    recording config + JSON persistence
    FileNameTemplate        {Year}{Month}… token expansion (extensible)
  platform/
    IdleMonitor             system-wide idle time (Windows: GetLastInputInfo)
  library/
    ClipLibrary             scan folders -> clip metadata (size, age, preset)
    ThumbnailCache          preview thumbnails (stub; Clip Library window)
  ui/
    MainWindow              PowerRec-style wide window: toolbar + big controls + strip
    PresetEditorDialog      edit one preset (format, fps, resolution, folder, …)
    ClipLibraryWindow       full grid of all clips with context actions
    RecentListWidget        list items draggable into other apps as files
    RegionTool              interactive resizable region overlay (handles/snap)
```

### Conventions

**Colours.** A colour is picked from a **colour field** — a swatch you click —
and whatever it colours **updates while you pick it**. Never a row of
Hue/Saturation/Brightness sliders, and never `QColorDialog::getColor`, which
only answers on OK.

Both halves are the same point: a colour is judged by looking at it in place.
Sliders make you solve backwards for a colour you can already picture; a picker
that stays silent until OK makes you pick, commit, look, and go back in.

`ui/ColorField.hpp` is the whole implementation — header-only, so it is one
`#include` away from anywhere:

- `pickColorLive(parent, title, initial, apply)` — `apply` runs on every
  movement inside the dialog and once more with the settled colour, so the
  caller does its saving, undo snapshot or repaint exactly once. **Cancel puts
  back the colour that was there**, not the last one hovered.
- `paintColorSwatch(button, colour)` / `paintMixedSwatch(button)` — the field
  itself; the colour is readable back off the button via its `harpiaColor`
  property.

A shader or script parameter declares a colour the same way, and gets the same
field in the Inspector:

```glsl
//@param uColor color #3F73B8 Colour
```

It arrives as a `uniform vec4` in 0..1, and travels everywhere else as a packed
`0xAARRGGBB` in a double — exact in 32 bits — so persistence, keyframes and the
props map need no special case. Colour keyframes interpolate per channel.

The one licensed variation: a swatch that lives in a property row (the component
Inspector) opens a **non-modal** dialog, so the rest of the UI stays usable
while it is up. It is still wired to `currentColorChanged`. The rule is
live-updating, not one particular function.

### Reused OBS backend APIs

- Startup: `obs_startup` → `obs_add_module_path` → `obs_load_all_modules2` →
  `obs_post_load_modules`
- Graph: `obs_reset_video` / `obs_reset_audio2`
- Capture: `obs_source_create("monitor_capture")` → `obs_set_output_source(0, …)`;
  region via `crop_filter` (absolute mode)
- Record: `obs_video_encoder_create` / `obs_audio_encoder_create` →
  `obs_output_create("ffmpeg_muxer")` → `obs_output_start/stop`
- Pause: `obs_output_pause` / `obs_output_paused`

## Implemented

- **PowerRec-style window**: wide (~10:4), compact, dark. Top toolbar (preset
  dropdown, New Preset, "only record while using the computer" toggle + timeout,
  Open Preset Folder); large centered **Record / Pause / Stop** with a live
  **recording timer**; bottom **horizontal thumbnail strip** of recent clips.
- One-click **Record / Pause / Resume / Stop** (H.264 → MP4 via `ffmpeg_muxer`)
- Full-monitor capture at the primary screen's native resolution
- **GPU compression** option: prefers NVENC/AMF/QSV when enabled, else x264
- **Presets**: create / edit (right-click the dropdown) / duplicate / delete,
  persisted as JSON. Each preset sets format, fps, resolution mode, output folder,
  GPU compression, idle timeout, and filename template.
- **Filename templates** with date/time tokens
- **Real thumbnails**: decoded from each clip with FFmpeg on a background thread,
  cached to disk (instant on later loads). Shown in the strip and Clip Library.
- **Recent recordings strip**: click to open; right-click for Open / Open
  containing folder / Copy file path; drag straight into other apps
- **Clip Library window**: grid of every recording on disk with "5 minutes ago",
  size, and originating preset; right-click **Open / Copy / Rename / Delete**;
  **Delete** key removes selected clips
- **Region selection**: drag-to-select a screen region on the captured display
  (applied via `crop_filter`) with an always-on-top overlay outlining the area
- **Multi-monitor**: each preset picks which display to capture (enumerated from
  the platform capture source); the picker, overlay, and canvas all follow it
- **GIF** output: recorded via `ffmpeg_output`, auto-downscaled (≤640px long edge)
  and fps-capped (≤15) with no audio track for reasonable file sizes
- **Idle auto-pause** driven by the toolbar toggle + the active preset's timeout,
  on **all three platforms** — Windows (`GetLastInputInfo`), macOS (CoreGraphics),
  Linux (GNOME **Mutter IdleMonitor** over D-Bus for Wayland, falling back to X11
  **XScreenSaver** via dlopen)

## Planned follow-ups

- Full palette-based (2-pass) GIF quality. This needs a post-recording transcode
  (record an intermediate, then `palettegen`/`paletteuse` via libavfilter) rather
  than the current single-pass live encode; slated for after the first green build
  so it can be validated in isolation.
- Broader resolution-scaling matrix (per-encoder scaled sizes)
- KDE/Wayland idle (currently GNOME Wayland + any X11)
