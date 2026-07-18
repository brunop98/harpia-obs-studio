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

You do *not* need to install Qt6/CEF/obs-deps manually — the CMake configure step
downloads the prebuilt dependencies from `buildspec.json` automatically.

### Quick build (recommended)

```powershell
# From the repo root, in PowerShell:
.\harpia\scripts\Build-Harpia.ps1
```

This configures (fetching deps on first run) and builds just `harpia-recorder`,
then prints the path to `harpia.exe`. Options: `-Configuration Release`,
`-Reconfigure`, `-Target all` (whole solution).

### Manual build

```powershell
cmake --preset windows-x64                 # configure + auto-download deps
cmake --build --preset windows-x64 --config RelWithDebInfo --target harpia-recorder
```

The binary lands at
`build_x64\rundir\RelWithDebInfo\bin\64bit\harpia.exe`.

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
    MainWindow              record/pause button, recent-10 drag-out list
    RecentListWidget        list items draggable into other apps as files
    RegionOverlay           region select + overlay (stub)
```

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

- One-click **Record / Pause / Resume / Stop** (H.264 → MP4 via `ffmpeg_muxer`)
- Full-monitor capture at the primary screen's native resolution
- **GPU compression** option: prefers NVENC/AMF/QSV when enabled, else x264
- **Presets**: create / edit / duplicate / delete, persisted as JSON. Each preset
  sets format, fps, resolution mode, output folder, GPU compression, idle timeout,
  and filename template. Selectable from the main window.
- **Filename templates** with date/time tokens
- **Recent 10 recordings**, draggable straight into other applications
- **Clip Library window**: grid of every recording on disk with "5 minutes ago",
  size, and originating preset; right-click **Open / Copy / Rename / Delete**;
  **Delete** key removes selected clips; drag-out into other apps
- **Region selection**: drag-to-select a screen region (applied via `crop_filter`)
  with an always-on-top overlay outlining the captured area
- **Idle auto-pause** driven by the active preset's timeout (Windows via
  `GetLastInputInfo`)

## Planned follow-ups (interfaces already defined)

- Real video **thumbnails** in the Clip Library (currently a file-type icon;
  `ThumbnailCache` interface is in place)
- **GIF** output path polish and full resolution-scaling matrix
- Multi-monitor selection in capture and region picker
- macOS / Linux idle-detection backends
