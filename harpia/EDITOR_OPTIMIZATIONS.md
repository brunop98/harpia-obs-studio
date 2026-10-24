# Editor optimizations — 2026-10-24

This pass removes repeated work from the existing editor paths. The measurements
below describe individual operations on the Linux cloud environment; they do not
predict a whole-editor frame-rate increase or Windows hardware performance.

## Changes

- **Playback:** Full Editing no longer serializes active clips and hashes a
  finished-frame cache key while playing. Playback already bypasses that cache
  so its decoders advance correctly. Paused scrubbing keeps its existing cache
  keys and invalidation behavior.
- **Captions:** whitespace expressions compile once per calling thread. Native
  word clocks remain shared when spelling already matches, and word highlighting
  reuses the resolved clock instead of parsing and reconciling the phrase twice.
  There is no retained cache of caption content or timestamps.
- **Color fields:** identical stylesheets are left in place instead of making Qt
  repolish unchanged buttons. Color metadata, text, tooltips, mixed selections,
  and actual color changes still refresh.
- **Style picker:** repeated Inspector synchronization keeps existing rows when
  the saved names are unchanged. Presets are still read from disk, and applying a
  style reads its current values, so another window or an external editor can
  update styles without stale cached data.
- **Inspector:** removed the second component-panel refresh from an update that
  already refreshes it through `syncClipInspector()`.

## Measurements

| Operation | Before | After |
| --- | ---: | ---: |
| Rich current-word caption evaluation | 34.19 µs | 6.95 µs |
| 10,000 unchanged color-swatch updates | 240.69 ms | 7.37 ms |
| Qt style-change events in those swatch updates | 10,000 | 0 |
| 1,000 preset refreshes, no custom styles | 16.92 ms | 4.48 ms |
| 1,000 preset refreshes, 20 custom styles | 141.93 ms | 95.16 ms |
| 1,000 preset refreshes, 100 custom styles | 640.84 ms | 468.40 ms |

Unchanged preset refreshes now cause zero removed/inserted rows. The previous
100-style case removed the list 1,000 times and inserted 108,000 rows.

A separate synthetic cache-key benchmark measured approximately 0.50 ms/frame
with four picture tracks and 1.00 ms/frame with eight, each with an active clip
carrying four components with eight keys. This work is now skipped in playback.
The simple one-track case cost about 0.017 ms/frame.

Caption measurements used matching optimized builds and the median of five
10,000-evaluation runs. UI measurements used matching editor builds and the same
Qt widgets and preset files. Shared cloud CPU timing varies between runs.

## Verification

- Before/after caption parity: 95,040 evaluations produced identical hashes of
  exact markup, resolved word text/times, and pop scale, including rich text,
  Unicode whitespace, edited timings, invalid tags, and out-of-order seeks.
- 18 focused test programs passed 557 checks, covering real editor controls,
  external preset edits, color changes, multi-clip edits, undo, narrow layout,
  playback starts/cuts, frame-cache invalidation, transitions, and export cadence.
- New regression programs: `captionperf_test`, `colorswatch_test`, and
  `inspectorreuse_test`, wired into the existing test runners.

Tests ran with Qt 6.8.2 and FFmpeg 7.1.5 on Linux. A native Windows build was not
run for this pass.
