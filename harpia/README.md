# Harpia Recorder and Editor

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
  **follow** it as it moves or resizes. During a recording the frame moves
  with the window while keeping the take's original dimensions.
- **Region frame on the desktop:** resize from its handles, move it from the
  tab above it, start recording from the button under it; it stays out of the
  recording itself.
- **Follow Mouse:** the region pans to keep the pointer in view.
  While recording, the floating controls' **padlock** holds the area at its
  current position so you can use sliders or other controls elsewhere. Drag
  the grip above the locked area to reposition it; it stays locked at the new
  position. Click the padlock again to resume following. The lock survives
  Pause/Resume and resets when the take ends; it does not change the profile's
  Follow Mouse setting.
- **Multiple areas:** lay out several same-size areas, across monitors; the
  recording switches to whichever one the mouse moves into. For Custom Region
  presets, select **Recording areas → Region movement → Multiple areas** in
  Preset Settings. Or right-click the recording frame and choose **Multiple
  areas → Arrange areas…** to enable it and place areas directly. Switching it
  off keeps the layout for next time; layout changes are locked during a take.
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
- **Record editing tutorials:** the recorder and video editor work in separate
  windows. Keep editing while recording, and use the floating Record/Pause/Stop
  controls or recording shortcuts with the editor focused. Editor dialogs leave
  those controls usable. Record the monitor, draw a region, or use **Pick a
  window…** to select the editor on supported platforms. Turn off a profile's
  focus restriction if it is set to pause while using another application.
- **Auto Split:** set **Auto split the recording file** under **Preset Settings
  → Recording** to split by recorded minutes or approximate file size. Each
  preset saves its own enable switch, split mode and limits across restarts.
  Existing saved Auto Split preferences are carried over to existing presets.
  Capture continues while the muxer opens the next file. After stopping, all
  parts are saved as `_part001`, `_part002`, and so on, including the shorter
  final part. Paused time is excluded. Supports MP4, MKV, MOV and AVI video;
  a separate webcam file stays continuous and recording markers cover the full
  take. Splits wait for video keyframes; file sizes also include container
  overhead, so limits are approximate. For a strict Discord upload cap,
  use **Optimize for sharing → Discord** after recording.
- **GPU compression:** **Preset Settings → Advanced → Use GPU compression
  while recording** keeps each preset's saved preference. GIF hides this option.
  Stop requests are idempotent; pause automation is suspended while stopping.
  Muxer error recovery reads only available helper diagnostics, so an empty
  stderr pipe cannot block shutdown. Windows writes also handle partial progress
  and report the OS error code.
- **Multi-monitor** with per-preset display choice, and per-monitor DPI.
  The recorder and editor each reopen on their last monitor, with a fallback
  to an available screen when that monitor is disconnected.

### Editing

In every timeline, hold the middle mouse button and drag left or right to pan
the visible time window. Scroll to zoom in when the whole timeline is fitted.
This also works in the voiceover and keyframe lanes.
The mouse's Back and Forward side buttons run Undo and Redo in the editor.

Three modes, each one step up from the last:

- **Simple Trim** — keep one section, with crop and speed.
- **Multi-Cut** — keep several sections, each at its own speed; randomise
  their order; export them joined or **each to its own file**. Use **Source
  video** above the tracks to switch videos and keep cuts from all of them
  in the same output. **Add video…** opens a thumbnail grid with **Harpia
  Editor**, **Downloads**, and **Videos** tabs. Each scans its folders and
  subfolders in the background; thumbnails load as they come into view.
  The picker remembers your last selected tab across launches.
  Use **Order by** to sort by newest date, largest size, or name (A to Z).
  Select a video to load it without removing cuts. **Import from folder…**
  at the bottom opens the system file picker for another video.
- **Full Editing** — a multi-track timeline:
  - video, image, text, effect and audio clips; overlapping clips become
    **transitions**; marquee selection, groups, ripple, snapping;
  - **Auto ripple on speed change:** enable it in a track header's right-click
    **Ripple** menu to move following clips when a clip gets faster or slower, preserving
    existing spacing. The option saves per track; undo restores the speed and
    shifted positions together;
  - **Set image time:** right-click an image or text clip → **Timing** →
    **Set image time: 0.5 s / 1 s / 2 s / 3 s / 5 s**, or **Custom…** to type any
    length. It applies to every selected image and text clip on unlocked tracks.
    Later clips on the same track move along so a slideshow stays back to back,
    and clips linked to them move too; one undo step;
  - **Add → Add from Library…** opens your recorded videos beside the current
    project. Drag a recording onto a timeline lane, or double-click to add it
    at the playhead;
  - **markers** in the ruler above the tracks: double-click to add one,
    drag to move it, or right-click to rename/delete it. Named markers draw
    dotted guides across every track and provide snap points. Drag the
    yellow **End** marker, or choose **Set video end here**, to set the end
    of playback and full-timeline exports. Later clips remain editable;
    **Reset video end** restores the automatic end. Markers and the end
    boundary are saved with the project and support undo/redo;
  - **beat markers:** right-click a music or video clip and choose **Detect
    beats…**, or use **Tools → Detect beats…**. Review the estimated tempo and
    add every beat, every second beat or every fourth beat in one undo step.
    Analysis runs locally on the kept source range (up to 30 minutes), respects
    trim, playback speed and timeline position, and keeps existing markers and
    the video end. Best with steady music. Generated markers are fixed timeline
    markers; moving or retiming the music does not move them;
  - **random clip options:** each choice has **Video…** to select its source
    and **Slip…** to preview and choose its moment. Drag the Slip filmstrip or
    enter **Source start** in seconds with millisecond precision. Options share
    one timeline position, duration, speed and look, while their source times
    stay independent. Cancel leaves the option unchanged; OK is one undo step;
  - **persistent groups:** select clips and press **G** to group them, including
    linked partners and every member of touched groups. Groups merge flat; **G**
    on one existing group ungroups it without breaking media/narration links.
    Click a member to select its group, **Ctrl-click** to add/remove a group,
    **Alt-click** to select one member. Groups move only in time, keeping their
    tracks and offsets, including at time zero; any locked member blocks moving.
    Speed scales the whole arrangement and gaps around its earliest start,
    retaining relative speeds within 0.1×–20×. Other Inspector edits, splitting,
    trimming and deletion affect only explicitly selected clips. Alt-selection
    is preserved by Undo/Redo. Copies get new group and media-link IDs;
  - **Spark favorites:** click **☆ / ★** on a video/image clip, or use its
    **Favorite for Spark** menu action. Under **Organize**, **Must include in
    Spark** (orange ★) puts a clip in every Spark video, and **Opening clip for
    Spark** (▶) starts every video with it. Clicking a lit badge clears its
    marks. Marks belong to individual edited clips, persist in projects, respect
    locks and are undoable;
  - **keyframes** per channel (position, scale, rotation, opacity) with easing
    curves, and **curved motion paths** with Bezier handles on the preview;
  - **components** stacked on any clip: transform and speed, ShaderToy-style
    **shaders** (`.frag`), **JavaScript** transform scripts, typing, subtitles,
    bullets and more — written in a watched folder and picked up live;
    Speed shows the kept duration at 1× and the resulting duration below its
    Factor control, updating as you adjust it (totals for multiple clips);
    [CRT](CRT_EFFECT.md) has brightness-preserving scanlines, beam controls,
    phosphor masks, glow, brightness and gamma controls;
  - **component presets:** open a component's chevron menu and choose **Save
    as preset…** to reuse its settings, keyframes and In/Out timings in any
    project. Use **Apply preset** on an existing component, or **Add Component
    → Presets** to find saved looks by name. Transform and Speed support presets
    too. Applying to several selected clips is one undo step; locked tracks are
    respected. Sound presets retain their audio. Custom shaders and scripts
    still need to be installed;
  - **effect diagnostics**: a nonmodal preview warning names a missing or
    failed custom shader; **Details…** shows a copyable report and recovery
    steps. Repeated frames log the same problem once. Exports refuse to
    silently omit the effect; restore/fix its `.frag` file or disable it;
  - **text** with presets and Unity-style **rich text** tags
    (`<b>`, `<color=#f80>`, `<size=150%>`…);
  - text **Background → Shape → Fit text** creates a rounded sticker following
    each line's width, including wrapped lines. Colour, opacity, padding and
    corner radius stay editable. **Rectangle** keeps the full-block background;
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
- **Projects** save and open with every setting, and autosave. **File → New project**
  (**Ctrl+N**) starts a blank project in the current window, with Save / Don’t Save /
  Cancel for existing edits. The new project has its own save path and undo history.
  The opening screen offers the three most recent projects to continue with one click.
- **Right-click menus:** common clip actions stay at the top. Timing, Organize,
  Tags, Audio and captions, and Export collect related commands in one level of
  submenus for video, image, text and effect clips. Track menus group audio,
  ripple, appearance and arrangement controls. Component menus keep Copy, Paste
  and Duplicate handy, with Save / Apply / Delete presets together in one flat
  Presets submenu. Short menus remain direct; there are no submenus inside these
  submenus.
- **Export:** MP4, MKV, MOV, WebM or GIF; size, quality, 4:4:4 chroma, a
  selected range only, batch export of cuts, and text variations.
  **Full Editing → right-click selected clips → Export → Export clips separately…**
  writes one file for each selected video, image or text clip, with that clip's
  trims, speed, crop, transforms, effects and linked audio. Each file starts at
  zero and uses the clip's full trimmed duration, including beyond the project
  end marker. Other picture layers, background music, and separate voiceover
  are excluded. Normal group selection exports its picture members separately;
  Alt-click selects one member. Locked tracks can export without being edited.
  The window shows each clip's timeline range and duration, creates numbered
  names from a shared clip name, and lets you edit individual filenames. Changing
  the shared name keeps manual edits; **Apply shared name to all clips** resets
  the list. Duplicate names (ignoring case), empty names and invalid filenames
  must be fixed before exporting. Format, quality, resolution, frame rate and
  audio settings apply to the whole batch. The files go into a new folder named
  after the shared clip name; an existing folder is kept and a numbered folder
  is created instead. Export progress, cancellation and optional opening of the
  finished folder use the normal batch workflow. The editor remains unchanged.
  **Full Editing → Export → Spark** exports unique footage orders from copies
  of the whole timeline, even if Single video's scope is Selection. Every
  footage block is shuffled, including clips and stars after the end marker;
  every export still stops at the unchanged marker, so whatever lands past it is
  cut. Two orders that differ only past the marker are the same video, so the
  unique-order count is the number of different videos up to the marker (shown
  as "at least" when there are too many to count exactly). Without an end
  marker, videos run the full length. Groups and media links form blocks that keep their internal timing,
  tracks, trims and speeds. Starred blocks shuffle first, followed by ordinary blocks. Independent
  blocks start end to end at zero; unrelated background audio, text/effects and
  separate voiceover stay at their original times. Event sounds are rebuilt to
  follow the new footage. Locked footage blocks disable Spark with an explanation;
  locked background tracks are fine. Equivalent copies count as the same order.
  Each **Random Text** independently picks one option (including its current
  text) per Spark video, held for that clip throughout the video. Spark samples
  these choices without generating all combinations or multiplying the footage
  order count. Plain Text clips and Random Clip footage choices stay as edited.
  **Spark settings**, on the Spark tab and saved with the project:
  - **Favorites per video:** all of them (the default), or a random number
    from–to. A video leaves the favorites it does not pick out entirely, linked
    media included, and gets shorter by their time; it still stops at the end
    marker when its footage reaches it.
  - **Favorites go:** **Always first**; **Spread out**, evenly through the
    part that is seen, starting with one; or **At the beginning and the end**:
    half open the video, the rest finish it before the end marker.
  - **Must include** clips are favorites every video picks; they count toward
    the range. An **Opening clip** starts every video ahead of the favorites;
    several opening clips take turns, and the others are left out of that
    video. Opening clips do not count toward the range.
  The count updates as you change the settings. With many favorites it may read
  "at least 10000".
  Request 1–1,000 videos (default 10); Spark shows how many unique orders exist
  and exports at most that many. Format, quality, resolution, frame rate, folder
  and audio come from Single video. Files use a new unique folder and numbered
  names such as `my_video_spark/my_video_spark_01.mp4`. Exporting leaves the
  editor unchanged and respects its video-end marker. Render failures include [error codes and copyable diagnostic details](EXPORT_ERRORS.md).

### Library and sharing

- A strip of recent recordings and a **Clip Library** window, with
  thumbnails, rename, delete, drag-out to other apps, and audio extraction.
- **Share copies:** a compressed, fast-start MP4 next to the original.
- **Send to phone:** available after export, on completed sharing copies and
  in the library's right-click menu, including multiple selected files.
  **LocalSend** is optional: Harpia finds an installed copy or recommends the
  free app with its [download link](https://localsend.org/download). Install it
  on your computer and iPhone, open it on the phone, and use the same Wi-Fi.
  Harpia opens LocalSend with the selected files; choose your phone there to
  start transferring. Portable installations can be selected with **Locate
  LocalSend…**. On Mac, **Send with AirDrop…** opens the native recipient
  picker. Windows uses LocalSend rather than Apple's AirDrop service.
- **Discord:** Optimize for sharing → Discord creates numbered MP4 parts of at
  most 49 MB each in a new folder, with a smaller final part and Copy All Parts.

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
`obs-ffmpeg-mux` helper, the NVIDIA/AMD/Intel encoder test helpers, Qt's
plugins), `obs-plugins\64bit` and `data\`, with the Visual C++ runtime next to
the exe. Both `-Package` and `zip-harpia.bat` validate critical runtime files,
including `libobs-d3d11.dll`, before creating a distributable. Debug packages
are refused because Visual Studio's debug runtime cannot be redistributed;
use `Release` or `RelWithDebInfo`. Zip the entire folder to share it.

Building only `harpia-recorder` also builds its graphics modules. On Windows,
each build restores their copies beside the executable if they were removed.

If the video engine reports error **-5**, Windows could not load its renderer
or one of its dependencies. Extract a fresh complete package and install or
repair Microsoft's [Visual C++ x64 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe).
The startup error includes a Windows loader check with the module path and
error number. Error **126** can mean a missing dependency even when the renderer
exists; **127** means a missing entry point, often incompatible library versions;
**193** indicates an incompatible or invalid binary. Error **-2** is a separate
graphics-device or driver support failure.

Startup and each recording attempt include diagnostic snapshots in **Error Logs**:
OS/architecture, display positions and DPI scaling, selected monitor, region
coordinates in device pixels, source and cropped sizes, scene transforms,
requested/actual video settings, encoder/bitrate, audio and split settings.
GPU model, driver and reported memory are logged when the graphics context is
available; Windows also lists display adapters without needing the renderer.
These snapshots are flushed to disk and do not run on every frame or mouse move.
To report a problem, reproduce it, open **Error Logs**, keep **Info** enabled,
then use **Copy** or **Open Log File** to include the complete current session.

For `harpia_region_crop` failures, look for the preceding `[crop_filter]` error:
it distinguishes missing `crop_filter.effect` data, an unavailable graphics
context, and shader compilation failure (including compiler details when
available). OBS can still print `private source ... created` after initialization
fails because it keeps a source wrapper; the capture snapshot reports whether
the actual plugin data is valid.

At run time nothing auto-installs: the exe needs its DLLs, the OBS plugins and
the Visual C++ runtime beside it, which the build `rundir` and the package
both provide. The runtime comes from Visual Studio's `VC\Redist\MSVC` folder,
no older than the compiler (if it is not found, configure warns and `-Package`
refuses). `harpia.exe` can be started from anywhere -- a shortcut, a taskbar
pin -- since it moves into its own folder at start-up, where libobs looks for
its data. `OBS_PLUGINS_PATH` / `OBS_PLUGINS_DATA_PATH` override where the
plugins are looked for.

### Other platforms

The recorder has Linux and macOS code paths (capture, audio, idle detection),
but Windows is the platform it is developed and tested on. Some features are
Windows-only for now — window picking, click ripples, global hotkeys.

See [macOS readiness](MACOS_READINESS.md) for the confirmed recording and
packaging gaps, implementation order and native validation needed for a Mac
release.

## Tests

The tests build with plain `g++` against Qt 6 and FFmpeg — no libobs, no
CMake — and run headless (`QT_QPA_PLATFORM=offscreen`):

```sh
harpia/tests/run_units.sh  [work-dir]   # logic, widgets, compositor, file formats
harpia/tests/run_avsync.sh [work-dir]   # export, A/V sync, and the real editor window
python3 harpia/tests/graphicsruntime_test.py [path-to-cmake] # targeted renderer build/staging
pwsh -File harpia/tests/windowspackage_test.ps1             # Windows package-layout fixtures
```

Each prints `ALL PASSED (0 failures)` per program and exits non-zero on any
failure. The editor window tests drive the real `VideoEditorWindow` with mouse
and keyboard events. The recorder's libobs-facing code (`core/`,
`MainWindow`) still needs native Windows validation. Pipe tests also run the
production Windows I/O code against controlled API responses on Linux:

```sh
harpia/tests/run_process_pipe.sh [work-dir]
OBS_LIB_DIR=/path/to/build/libobs harpia/tests/run_recording_stop.sh [work-dir]
# With a built libobs, run_avsync.sh also checks the real GPU settings dialog.
# On Linux this additional OBS test needs DISPLAY (xvfb-run works in CI).
OBS_LIB_DIR=/path/to/build/libobs xvfb-run -a harpia/tests/run_avsync.sh [work-dir]
```

Spark's end-to-end test opens red, green and blue footage in the real editor,
stars blue, requests 100 videos, and inspects the only two unique encoded orders:
Blue → Red → Green and Blue → Green → Red. It also checks star Undo/Redo,
numbered output names, preservation of the original timeline, and a 1.5-second
end marker with starred blue placed after it: blue still opens both videos, red or
green follows until the marker cuts it, and event-sound tails are capped. A third
pass marks red as the opening clip and sets 0–1 favorites per video: the two
videos are Red → Green and Red → Blue, and the settings are kept with the
project as one undo step. For recording
validation, build with NVENC enabled on Windows and repeat record/stop on the
RTX 3050; the Linux test build does not validate NVIDIA hardware or establish
why a muxer helper originally failed.

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

The [preview audit](PREVIEW_AUDIT.md) records preview fixes, regression coverage
and the limits of automated audio/GPU validation.

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
