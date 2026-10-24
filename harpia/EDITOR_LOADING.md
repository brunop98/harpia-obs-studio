# Editor loading feedback

Opening a project or importing video, images or audio displays a loading dialog
with the current activity and file path. Project imports, pasted files and file
drops share one dialog per operation and show the number of files already
processed. Work without a known total uses a busy indicator. Elapsed time is
shown; the dialog closes automatically, including on failure. A blank editor
opens without loading feedback.

File reading/parsing, FFmpeg video probing, still-image preparation, audio
conversion and waveform decoding run away from the GUI thread. Media is
registered and project snapshots are applied on the GUI thread. The small
`runLoadingTask` helper joins its worker before returning an owned result; tasks
must use copied inputs and must not access widgets or mutable editor state.

While loading, playback stops, autosave and coalesced edits wait, and modeless
tool results wait until the operation finishes. Deferred edits for a replaced
project are discarded. Active narration recording is finalized first. A loading
scope restores timers and closes its dialog on all return paths. The separate
project-loading flag is scoped too, so later imports still report errors.
Loading feedback hides before a relink picker or error message appears. The
operation cannot be dismissed halfway through an import.

Regression coverage:

- `loadingdialog_test`: modality, literal paths, progress bounds, dismissal and
  updates without event pumping.
- `loadingtask_test`: GUI responsiveness, owned results, exceptions and immediate
  completion.
- `loadingwindow_test`: actual video/image/audio loading, launch from the library,
  mixed file batches, project stages, restored waveforms, deferred tool results,
  missing media, invalid projects and error cleanup.

The first two run in `tests/run_units.sh`; editor integration runs in
`tests/run_avsync.sh` using generated media fixtures.
