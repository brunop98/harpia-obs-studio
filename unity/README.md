# Harpia Recorder for Unity

Records Unity's Game view with **Harpia Recorder and Editor** every time you
enter Play Mode, so every play session becomes a video without pressing a key.
A window in Unity holds the settings and shows Harpia's recordings, so you can
watch, edit or delete a take without leaving the editor.

| In Unity | Harpia does |
| --- | --- |
| Enter Play Mode | starts recording the Game view, with its current preset (quality, frame rate, codec, audio, output folder...) |
| Pause / Unpause | pauses / resumes the recording (paused time is cut out) |
| Leave Play Mode | stops and saves; runs shorter than the minimum (3 s by default) are deleted instead |

If Harpia isn't running, the Console says so and Play Mode carries on as normal.
A recording you started yourself in Harpia is never touched: Unity can neither
take it over nor stop it.

This is **experimental** (Harpia branch `claude/unity-play-mode-recorder`).

## Setup

1. **Harpia**: build and run Harpia from that branch. *Settings > System >
   Unity* is on by default ("Let Unity start recordings"), on port **47811**.
2. **Unity** (2021.3 or newer): copy the **`HarpiaRecorder`** folder into your
   project's **`Assets`** folder. That's all: the scripts are in an `Editor`
   folder, so they run in the editor only and never go into a build.
   (The scripts can live anywhere in the project, as long as they stay inside a
   folder named `Editor`.)
3. Open **Window > Harpia Recorder**. The top line should turn green:
   "Harpia Recorder and Editor ... -- Ready".
4. *Settings* tab: **What gets recorded (live)** shows the rectangle and the
   game's resolution; **Show Recording Area** has Harpia outline it on screen.
   It should sit exactly on the game picture.
5. Press Play.

To remove it, delete the `HarpiaRecorder` folder.

## The window

**Window > Harpia Recorder** (also *Tools > Harpia Recorder > Open Window*).

- **Top line**: whether Harpia is running and what it is doing (ready,
  recording with the time, paused, saving), and the preset it records with.
- **Record Play Mode** (toolbar): switches the automatic recording on and off.
- **Recordings** tab: Harpia's recordings, newest first (the same list as
  Harpia's Recent strip and Clip Library), with a thumbnail, length, size, age
  and preset. Type in the search box to filter by name or preset.
  - Click to select. **Double-click** or **Enter** plays it in your video player.
  - **Show in Explorer**, **Edit in Harpia** (opens Harpia's editor to trim,
    crop and export), and **Delete** (moves it to the recycle bin, after
    asking) are in the bar at the bottom and in the right-click menu.
  - The **Delete** key deletes the selected recording; the next one is then
    selected, so a few presses clear out the bad takes. The arrow keys move the
    selection.
  - The list refreshes when a recording finishes, when you come back to the
    window, and with **Refresh**.
- **Settings** tab (also *Edit > Preferences > Harpia Recorder*), with a live
  line saying what would be recorded right now -- for example *"1280x720 at
  (2560, 140); the game renders 1920x1080, shown at 67%; display scaling 150%;
  position from Windows"*:
  - **Record Play Mode** -- on/off.
  - **Area** -- *Game Picture* (just the rendered game) or *Whole Game View*
    (everything under the Game view's toolbar, letterbox bars included).
  - **Don't keep runs shorter than (s)** -- 3 by default; 0 keeps every run.
  - **Harpia port** -- must match Harpia's *Settings > System > Port*.
  - **Start delay (ms)** -- how long to wait after entering Play Mode before
    measuring the Game view (lets *Maximize on Play* settle).
  - **Log each start and stop** -- problems are always logged.

The settings are per user and per machine (EditorPrefs); nothing is written to
the project.

What comes from Harpia's preset: everything about the video and sound. What
does not, for these recordings only: the area (it is the Game view), the
countdown (none -- the game is already running), and the preset's automatic
pauses (idle, focus, pointer leaving the area) and Follow Mouse / Multi-Area:
Unity decides when to pause. Harpia puts your own area back afterwards.

## Recording only the game

Only the game's picture is recorded: not the Game view's toolbar, not its tab,
not the bars around a game whose aspect ratio differs from the view's. The
scripts read the Game view's own layout (by reflection, checked against
[Unity's C# reference source](https://github.com/Unity-Technologies/UnityCsReference)
for 2021.3, 2022.3 and Unity 6), the same rectangle Unity draws the game into:

- **Which view**: the one Play Mode renders into (the maximized one with *Play
  Maximized*). If that is the Device Simulator, nothing is recorded and the
  Console says to switch to the Game view.
- **Where in it**: under the toolbar, letterboxed as Unity letterboxes it;
  zoomed in, only the part that shows.
- **Display scaling**: the Game view's own display's, even when other displays
  use another scaling.
- **On the desktop**: on Windows, from the Game view's own window as Windows
  reports it, in physical pixels -- exact with any mix of displays and
  scalings. Elsewhere it is worked out from Unity's coordinates.

**Resolution.** The video is the picture as it appears on screen. A game that
renders at 1920x1080 in a Game view showing it at 67% is recorded at the size
it appears: 1280x720. For the game's full resolution, give
the Game view room for it: *Play Maximized*, a larger window, or *Free Aspect*
(the game then renders at the view's size). The live line in *Settings* and
the Console say when the picture is shown smaller than the game renders. The
video's own size and quality still come from Harpia's preset.

## Troubleshooting

- **"Harpia isn't running"** -- start Harpia; check *Settings > System > Unity*
  is on and both sides use the same port.
- **"already recording (started by hand)"** -- stop your own recording in
  Harpia first; Unity leaves it alone on purpose.
- **"Harpia has a window open"** -- Harpia does not start a recording, or open
  its editor, while one of its dialogs (Settings, the editor...) is open. Close
  it and try again.
- **The outline is off** -- the live line in *Settings* ends with "position
  from Windows" when the position was read from Windows (exact). If it says
  "estimated from Unity's screen coordinates" instead, and your displays use
  different scalings, the position can be off: put Unity on a display with
  the same scaling as the main one, and please report it.
- **"switch it to the Game view"** -- Play Mode is showing in the Device
  Simulator, which these scripts do not measure.
- **Moving or resizing the Game view during Play Mode** -- the area is measured
  once, when the recording starts.
- **"Could not delete ... (is it open in another program?)"** -- close the
  video player that has it open, then delete again.

## What's in the folder

`HarpiaRecorder/Editor/`:

| File | What it does |
| --- | --- |
| `PlayModeRecorder.cs` | Enter / pause / leave Play Mode -> start / pause / stop |
| `HarpiaRecorderWindow.cs` | Window > Harpia Recorder |
| `HarpiaSettings.cs` | The settings, and their page in Preferences |
| `HarpiaMenu.cs` | Tools > Harpia Recorder |
| `GameViewArea.cs` | Where the game's picture is on screen, in physical pixels |
| `HarpiaClient.cs`, `HarpiaClip.cs`, `Json.cs`, `CommandQueue.cs` | Talking to Harpia (plain .NET, no Unity) |

`Tests~/` is not needed in a project (Unity ignores it anyway); see *Tests*.

## How it talks to Harpia

Plain HTTP + JSON on `127.0.0.1` (never reachable from other computers;
requests from web pages are refused). Handy for a quick check without Unity:

```sh
curl http://127.0.0.1:47811/status
curl -H "X-Harpia-Client: curl" "http://127.0.0.1:47811/recordings?limit=5"
curl -X POST -H "X-Harpia-Client: curl" -d '{"x":0,"y":0,"width":1280,"height":720}' http://127.0.0.1:47811/area/show
```

| Request | |
| --- | --- |
| `GET /status` | running, state, elapsed time, preset |
| `POST /record/start` | `x`, `y`, `width`, `height` in physical desktop pixels, plus `client` and `label` |
| `POST /record/pause`, `POST /record/resume` | |
| `POST /record/stop` | `discardIfShorterThanMs` |
| `POST /area/show` | outline an area for a moment |
| `GET /recordings?limit=100` | the recordings, newest first |
| `GET /recordings/thumbnail?path=...` | PNG; `202` with `"pending":true` while it is being made |
| `POST /recordings/delete` | `{"path": "..."}` or `{"paths": [...]}`; to the recycle bin |
| `POST /recordings/edit` | `{"path": "..."}`; opens it in Harpia's editor |

Every request except `GET /status` needs an `X-Harpia-Client` header. Delete,
edit and thumbnail only accept a path that `GET /recordings` lists, so no other
file on the disk can be named. See `harpia/core/RemoteControl.hpp` and
`harpia/core/RemoteClips.hpp`.

## Tests

Both run under Mono; Harpia's `tests/run_units.sh` runs them when `mcs` is
installed.

- `Tests~/ClientTests.cs`: the scripts' Unity-free part (`HarpiaClient.cs`,
  `HarpiaClip.cs`, `Json.cs`, `CommandQueue.cs`) against the real Harpia server
  code.
- `Tests~/GameViewAreaTests.cs`: `GameViewArea.cs` against
  `Tests~/FakeUnityEditor.cs`, a small replica of the Unity internals it reads
  (same names, member kinds, visibility and inheritance as Unity's, geometry
  copied from the reference source): docked, maximized and toolbar-less Game
  views, letterboxing, zoom, 150% scaling, the Simulator. The Windows window
  lookup itself only runs in Unity on Windows.
