# Harpia Recorder

A screen recorder and video editor for Windows, built directly on **libobs** —
the capture and encoding core of OBS Studio — with its own Qt 6 interface.
Inspired by PowerRec: one click to record a monitor, a region or a window, then
trim, cut or fully edit the result and export it, without leaving the app.

It reuses OBS's capture, encoder and muxer plugins but none of the OBS Studio
frontend. The repository is the OBS tree slimmed down to what the recorder
needs (see [The OBS tree](#the-obs-tree)).

## Features

### Recording

- **What to record:** an entire monitor, a custom region, or **audio only**.
  Regions can be saved and reused by name.
- **Pick a window:** point at any window and click — the area becomes that
  window as you see it (without its invisible border or shadow). Optionally
  **follow** it as it moves or resizes, even while recording.
- **Region frame on the desktop:** resize from its handles, move it from the
  tab above it, start recording from the button under it; it stays out of the
  recording itself.
- **Follow Mouse:** the region pans to keep the pointer in view.
- **Multiple areas:** lay out several same-size areas, across monitors; the
  recording switches to whichever one the mouse moves into.
- **Zoom** (F4) and **spotlight** effects while recording; click ripples and a
  cursor highlight.
- **Webcam** recorded alongside as its own file, with a live preview.
- **Audio:** desktop sound and any number of microphones, each with a level
  meter and volume.
- **Encoders:** H.264, HEVC and AV1 — NVENC, AMF or Intel QSV on the GPU when
  available, x264 otherwise. **Containers:** MP4, MKV, MOV, AVI and GIF.
- **Presets** for everything above: format, codec, frame rate, folder, file
  name template, webcam, audio, region behaviour.
- **Global hotkeys** (F9 record, F10 pause, F4 zoom), a countdown, floating
  recording controls, **idle auto-pause** and pause-when-another-app-is-focused.
- **Multi-monitor** with per-preset display choice, and per-monitor DPI.

### Editing

Three modes, each one step up from the last:

- **Simple Trim** — keep one section, with crop and speed.
- **Multi-Cut** — keep several sections, each at its own speed; randomise
  their order; export them joined or **each to its own file**.
- **Full Editing** — a multi-track timeline:
  - video, image, text, effect and audio clips; overlapping clips become
    **transitions**; marquee selection, groups, ripple, snapping;
  - **keyframes** per channel (position, scale, rotation, opacity) with easing
    curves, and **curved motion paths** with Bezier handles on the preview;
  - **components** stacked on any clip: transform and speed, ShaderToy-style
    **shaders** (`.frag`), **JavaScript** transform scripts, typing, subtitles,
    bullets and more — written in a watched folder and picked up live;
  - **text** with presets and Unity-style **rich text** tags
    (`<b>`, `<color=#f80>`, `<size=150%>`…);
  - **Random text**: one caption with several alternatives, exported as one
    video per combination (with saved lists of alternatives);
  - **Inverse Selection** masks, effect clips, **sounds for events**, tags;
  - **subtitles from speech** (OpenAI transcription; the API key is kept with
    Windows DPAPI, never in project files);
  - **voiceover** recording with countdown and play-along;
  - images pasted from the clipboard, **video from a URL** (yt-dlp, when
    installed), proxy media for heavy sources;
  - an **editing console** — a line of JavaScript against the timeline;
  - undo/redo for every edit, including selection.
- **Projects** save and open with every setting, and autosave.
- **Export:** MP4, MKV, MOV, WebM or GIF; size, quality, 4:4:4 chroma, a
  selected range only, batch export of cuts, and text variations.

### Library and sharing

- A strip of recent recordings and a **Clip Library** window, with
  thumbnails, rename, delete, drag-out to other apps, and audio extraction.
- **Share copies:** a compressed, fast-start MP4 next to the original.

### Reliability

- A start-up **dependency check** with readable messages instead of a failure
  on the first Record press.
- **Crash reports** with the crashing thread's stack (function, file and line)
  and a minidump, shown on the next start.
- Session logs, an error log panel, disk-space and readiness warnings, single
  instance, and recovery of an interrupted recording.

## Building (Windows)

Harpia builds as part of this CMake project (`ENABLE_HARPIA`, on by default).

### Prerequisites (install once)

- **Visual Studio 2022** with the **Desktop development with C++** workload
- **CMake ≥ 3.28** — `winget install Kitware.CMake`, then **reopen the terminal**
- **Git**
- *Optional, for webcam and Intel QSV:* the **C++ ATL for latest v143 build
  tools (x86 & x64)** Visual Studio component. `win-dshow` (webcam) and
  `obs-qsv11` (QSV) need ATL; without it the build **skips just those two** and
  everything else still builds.

Qt 6, FFmpeg and the other prebuilt libraries are downloaded by the CMake
configure step (the `dependencies` preset in `CMakePresets.json`) — you do not
install them by hand.

> **Only want to run Harpia on another PC?** Don't build there — it needs no
> toolchain at all. Build once with `-Package` (below) and copy the
> `build_x64\dist\Harpia` folder across. Running the build script on a PC
> without the C++ toolset is what produces CMake's
> *"Visual Studio 17 2022 … could not find any instance of Visual Studio"*.

### Quick build (recommended)

```powershell
# From the repo root, in PowerShell:
.\harpia\scripts\Build-Harpia.ps1
```

This configures (fetching dependencies on first run), builds `harpia-recorder`
and prints the path to `harpia.exe`. Options: `-Configuration Release`,
`-Reconfigure`, `-Target all`, `-Run`, `-Package`.

### Manual build

The repo's `windows-x64` preset pins a Visual Studio generator and Windows SDK
that may not match your install, so configure `build_x64` directly:

```powershell
# Fresh configure; add -G "Visual Studio 17 2022" if CMake picks the wrong VS.
cmake -S . -B build_x64 -A x64

# Reconfigure an existing tree (reuses the cache's generator):
cmake -S . -B build_x64

cmake --build build_x64 --config RelWithDebInfo --target harpia-recorder
```

The binary lands at `build_x64\rundir\RelWithDebInfo\bin\64bit\harpia.exe`.

### Making a distributable

```powershell
.\harpia\scripts\Build-Harpia.ps1 -Package
```

This assembles `build_x64\dist\Harpia\`: `bin\64bit` (the exe, its DLLs, the
`obs-ffmpeg-mux` helper, Qt's `platforms\`), `obs-plugins\64bit` and `data\`,
with the Visual C++ runtime next to the exe. Zip the folder to share it.

At run time nothing auto-installs: the exe needs its DLLs, the OBS plugins and
the Visual C++ runtime beside it, which the build `rundir` and the package
both provide. `OBS_PLUGINS_PATH` / `OBS_PLUGINS_DATA_PATH` override where the
plugins are looked for.

### Other platforms

The recorder has Linux and macOS code paths (capture, audio, idle detection),
but Windows is the platform it is developed and tested on. Some features are
Windows-only for now — window picking, click ripples, global hotkeys.

## Tests

The tests build with plain `g++` against Qt 6 and FFmpeg — no libobs, no
CMake — and run headless (`QT_QPA_PLATFORM=offscreen`):

```sh
harpia/tests/run_units.sh  [work-dir]   # logic, widgets, compositor, file formats
harpia/tests/run_avsync.sh [work-dir]   # export, A/V sync, and the real editor window
```

Each prints `ALL PASSED (0 failures)` per program and exits non-zero on any
failure. The editor window tests drive the real `VideoEditorWindow` with mouse
and keyboard events. The recorder's libobs-facing code (`core/`,
`MainWindow`) is not covered here — it is checked on a Windows build.

## Project layout

```
harpia/
  main.cpp          start-up: crash guard, single instance, libobs, main window
  core/             the recorder's engine (libobs-facing, no widgets)
                    ObsContext, CaptureManager, RecordingController, EncoderFactory,
                    AudioManager, WebcamRecorder, AudioOnlyRecorder, Remuxer,
                    FollowMouse, MultiArea, ZoomMode, SpotlightFx, GlobalHotkeys,
                    ShareExporter, CrashGuard, Logger
  model/            Preset / PresetStore, RegionStore, FileNameTemplate
  platform/         per-OS pieces: idle time, foreground app, camera access,
                    window list (win/, nix/, mac/)
  library/          ClipLibrary, ThumbnailCache
  ui/               the recorder's windows and overlays: MainWindow, RegionTool,
                    WindowPicker, MultiAreaOverlay, PresetEditorDialog,
                    ClipLibraryWindow, StartupSplash …
  editor/           the video editor: VideoEditorWindow, ClipExporter,
                    BatchExport, ExportOptionsDialog, voiceover, previews …
    timeline/       the Full Editing model, view and compositor; keyframes,
                    motion paths, transitions, rich text, text variations
    component/      the component system and the built-in components
    shader/  script/  subtitles/  ytdlp/
  resources/        icons
  scripts/          Build-Harpia.ps1
  tests/            run_units.sh, run_avsync.sh and the test programs
  third_party/      QuickJS (the JavaScript engine for scripts and the console)
```

## Conventions

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

**libobs references.** Every libobs getter that returns a reference is
released. In particular `obs_get_output_source()` adds one, so channels are
only ever asked through `core/ObsChannel.hpp` — the unit suite fails on any
other call. Outputs are stopped *and waited for* before they are released
(`core/ObsOutputs.hpp`).

## How it uses libobs

- **Start-up:** `obs_startup` → `obs_add_module_path` → `obs_load_all_modules2`
  → `obs_post_load_modules`; `obs_reset_video` / `obs_reset_audio2`.
- **Capture:** `monitor_capture` inside a private scene
  on output channel 0 — the scene item carries the zoom transform; the region
  is a `crop_filter` (a picked window is a region too); extra monitors for
  Multi-Area are further scene items.
- **Record:** `obs_video_encoder_create` / `obs_audio_encoder_create` →
  `obs_output_create("ffmpeg_muxer")` (GIF through `ffmpeg_output`); pause with
  `obs_output_pause`. Audio only taps the mix with a raw audio callback.
- **Webcam:** a camera source in its own `obs_view` and video mix, with its own
  encoder and muxer.

The editor does not use libobs: it decodes, composites and encodes with FFmpeg
and Qt directly.

## The OBS tree

The repository root is OBS Studio trimmed to the recorder's needs: `libobs`,
its D3D11 and OpenGL back-ends (macOS uses OpenGL too), and these plugins —
screen, window and audio capture per platform, the webcam (`win-dshow`,
`linux-v4l2`, `mac-avcapture`), `obs-x264`, `obs-nvenc`, `obs-qsv11`, AMF and
VA-API via `obs-ffmpeg` (with the ffmpeg muxer), and `obs-filters` reduced to
the crop filter. The OBS Studio frontend, scripting, streaming, translations
and every other plugin are removed, so **stock OBS Studio no longer builds
from this tree** — recover it from git history if ever needed.

## License

GNU General Public License v2 or later, like OBS Studio — see `COPYING` at the
repository root. libobs and the plugins are the work of the OBS Studio
contributors (`AUTHORS`); QuickJS is under its own MIT license
(`third_party/quickjs/LICENSE`).
