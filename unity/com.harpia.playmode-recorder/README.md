# Harpia Play Mode Recorder

Records Unity's Game view with **Harpia Recorder and Editor** every time you
enter Play Mode, so every play session becomes a video without pressing a key.

| In Unity | Harpia does |
| --- | --- |
| Enter Play Mode | starts recording the Game view, with its current preset (quality, frame rate, codec, audio, output folder...) |
| Pause / Unpause | pauses / resumes the recording (paused time is cut out) |
| Leave Play Mode | stops and saves; runs shorter than the minimum (3 s by default) are deleted instead |

If Harpia isn't running, the Console says so and Play Mode carries on as normal.
A recording you started yourself in Harpia is never touched: Unity can neither
take it over nor stop it.

This is an **experimental** package (Harpia branch `claude/unity-play-mode-recorder`).

## Setup

1. **Harpia**: build and run Harpia from that branch. *Settings > System >
   Unity* is on by default ("Let Unity start recordings"), on port **47811**.
2. **Unity** (2021.3 or newer): *Window > Package Manager > + > Add package
   from disk...* and pick `unity/com.harpia.playmode-recorder/package.json` from
   the Harpia repository. (Or *Add package from git URL...* with
   `https://github.com/brunop98/harpia-obs-studio.git?path=/unity/com.harpia.playmode-recorder#claude/unity-play-mode-recorder`
   if Unity can reach the repository.)
3. *Tools > Harpia Recorder > Test Connection* -- the Console should say
   "Connected to Harpia Recorder and Editor ...".
4. *Tools > Harpia Recorder > Show Recording Area* -- Harpia outlines the
   rectangle it would record. It should sit exactly on the game picture.
5. Press Play.

## Settings

*Edit > Preferences > Harpia Recorder* (per user, per machine):

- **Record Play Mode** -- on/off (also *Tools > Harpia Recorder > Record Play Mode*).
- **Area** -- *Game Picture* (just the rendered game) or *Whole Game View*
  (everything under the Game view's toolbar, letterbox bars included).
- **Don't keep runs shorter than (s)** -- 3 by default; 0 keeps every run.
- **Harpia port** -- must match Harpia's *Settings > System > Port*.
- **Start delay (ms)** -- how long to wait after entering Play Mode before
  measuring the Game view (lets *Maximize on Play* settle).
- **Log each start and stop** -- problems are always logged.

What comes from Harpia's preset: everything about the video and sound. What
does not, for these recordings only: the area (it is the Game view), the
countdown (none -- the game is already running), and the preset's automatic
pauses (idle, focus, pointer leaving the area) and Follow Mouse / Multi-Area:
Unity decides when to pause. Harpia puts your own area back afterwards.

## Troubleshooting

- **"Harpia isn't running"** -- start Harpia; check *Settings > System > Unity*
  is on and both sides use the same port.
- **"already recording (started by hand)"** -- stop your own recording in
  Harpia first; Unity leaves it alone on purpose.
- **The outline is off** -- Unity measures windows in points and Harpia records
  physical pixels; they are converted with the display scaling. With displays
  at *different* scalings the conversion can be off: try the other *Area*, or
  put Unity on a display with the same scaling as the main one.
- **Moving or resizing the Game view during Play Mode** -- the area is measured
  once, when the recording starts.

## How it talks to Harpia

Plain HTTP + JSON on `127.0.0.1` (never reachable from other computers; requests
from web pages are refused). Handy for a quick check without Unity:

```sh
curl http://127.0.0.1:47811/status
curl -X POST -H "X-Harpia-Client: curl" -d '{"x":0,"y":0,"width":1280,"height":720}' http://127.0.0.1:47811/area/show
```

Commands: `GET /status`, `POST /record/start` (`x`, `y`, `width`, `height` in
physical desktop pixels, plus `client` and `label`), `POST /record/pause`,
`POST /record/resume`, `POST /record/stop` (`discardIfShorterThanMs`),
`POST /area/show`. See `harpia/core/RemoteControl.hpp`.

## Tests

`Tests~` (ignored by Unity) runs the package's Unity-free core
(`HarpiaClient.cs`, `CommandQueue.cs`) under Mono against the real Harpia
server code; Harpia's `tests/run_units.sh` does this when `mcs` is installed.
