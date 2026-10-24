# Preview audit — 2026-10-24

The audit checks the actual Qt editor, FFmpeg decoding and audio mixing, the
timeline compositor, and software OpenGL. It uses numbered video frames,
distinct audio tones, image comparisons and real editor controls rather than
assuming that a successful export means the preview is correct.

## Fixed

- **Scrubbing:** cached frames now match their decoded timestamp intervals.
  Integer frame-duration buckets could identify the wrong frame as exact at
  60 fps, fractional rates and variable frame rates. Delayed first frames and
  the last frame are covered too.
- **Source/proxy replacement:** results already decoding on the worker are
  discarded when their source generation changes, preventing old pixels from
  reappearing after the cache was cleared.
- **Memory:** decoded preview frames share a 512 MiB budget across sources,
  instead of allowing every imported source its own 512 MiB allocation. A
  single oversized frame can remain so its request can converge. This limit
  covers the decoder cache; compositor, caption, audio and codec buffers are
  separate allocations.
- **Playback:** Simple Trim and Multi-Cut now resolve the requested frame
  exactly, as Full Editing does. The initial frame is decoded before its work
  counts as playback time. Long GOPs no longer show earlier footage racing
  towards the clip's in-point. Multi-Cut also fits sources into its output
  canvas during playback, matching its paused preview.
- **Transitions:** outgoing and incoming clips have separate preview decoder
  positions, including two cuts from the same recording on the same track.
  A focused 30-frame overlap drops from 60 seeks to two while preserving pixels.
- **Modes and Reset:** mode switches discard old queued hover requests, restore
  the correct canvas and redraw the new mode. Full Editing's Reset now resets
  its own timeline and composition, rather than the Simple Trim marker.
- **Keyframes and masks:** visible transform and mask grips follow the selected
  clip's animated values during playback. The component inspector refreshes
  at the current playhead without rebuilding the motion path each frame.
- **Scripts:** Full, Half and Quarter preview quality provide the same logical
  source/crop/text dimensions to scripts. Transform script components now
  receive those dimensions too, fixing the bundled Tutorial Zoom's framing of
  letterboxed sources.
- **Audio:** mix identities include fade shapes, transition toggles, precise
  speeds/gains and source-file identity. Different speeds no longer share a
  rounded decoded-audio cache entry. Decoded WAVs remain alive while any
  preview/export mix still uses them. Timing fallback is anchored to the last
  audio position when mute, remixing or device state hands control back to the
  wall clock.
- **Text:** tags that produce no styling preserve plain-text shaping. The
  underline regression measures antialiased coverage rather than assuming the
  platform font draws a nearly white underline.

## Verification

**Result:** 47 preview-focused test programs passed, with 1,089 passing
assertions and no failures. These checks ran against the rebuilt editor on
Linux with Qt 6.8.2 and FFmpeg 7.1.5; they are a focused subset of the repository
tests, not a claim that every repository test was executed.

New regressions live in `audioaudit_test`, `previewgeometry_test`,
`previewinterval_test`, `previewcontrols_test`, `previewtransition_test` and
`previewlegacypacing_test`. They are included in `tests/run_avsync.sh`.

The existing checks cover image formats/crops/aspect, clip movement and resizing,
keyframe interpolation and editing, masks, component effects and fades,
preview-cache invalidation, audio import/gain/mute/solo/crossfades, audio/video
alignment, decoder responsiveness, playback timing, and CPU/GPU effect parity.
The GPU comparisons run in a temporary X display with software OpenGL.

## Limits

The cloud environment has no physical audio output device. PCM samples and
audio/video alignment are checked, but speaker behavior and Windows audio/GPU
drivers require validation on the user's machine. Software OpenGL is a real GL
rendering check, not a hardware performance benchmark. The full Windows
recorder executable was not built in this environment.

Selection outlines follow the clip's base/keyframed transform; evaluating
component/script-generated poses for those editing outlines remains a separate
limitation. The rendered picture does evaluate those poses. Cancellation can
also wait for the current source-audio decode to finish before the worker exits.
