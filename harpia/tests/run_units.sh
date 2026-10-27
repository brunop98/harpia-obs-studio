#!/bin/bash
# Small, fast checks that need no media and no window manager.
#
#   harpia/tests/run_units.sh [workdir]
#
# Each is a regression guard for a bug that was actually found here, not a
# restatement of the code: run them before touching the areas they cover.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
H="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$H/.." && pwd)"
WORK="${1:-$(mktemp -d)}"
mkdir -p "$WORK"
MOC="$(command -v moc || ls /usr/lib/qt6/libexec/moc /usr/lib/x86_64-linux-gnu/qt6/libexec/moc 2>/dev/null | head -1)"
# Qt6OpenGL is here for SpotlightGl, which Spotlight.cpp dispatches to.
CF="$(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core Qt6Test Qt6OpenGL Qt6Network)"
LF="$(pkg-config --libs Qt6Widgets Qt6Gui Qt6Core Qt6Test Qt6OpenGL Qt6Network)"

# Window monitor identity survives display reordering; disconnected monitors
# fall back to a usable screen, independently for recorder and editor.
g++ -std=c++17 -O1 -fPIC -I"$H" $CF \
	"$HERE/windowscreen_test.cpp" "$H/ui/WindowScreen.cpp" -o "$WORK/windowscreen_test" $LF

# Recursive video discovery streams bounded GUI batches and discards canceled
# or superseded results without blocking close or tab changes.
"$MOC" -I"$H" "$H/library/VideoFolderScanner.hpp" -o "$WORK/moc_VideoFolderScanner.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" $CF \
	"$HERE/videofolderscan_test.cpp" "$H/library/VideoFolderScanner.cpp" \
	"$WORK/moc_VideoFolderScanner.cpp" -o "$WORK/videofolderscan_test" $LF

# Recording split preferences, one-time overrides, dialog state and safe paths.
g++ -std=c++17 -O1 -fPIC -I"$H" $CF \
	"$HERE/autosplitdialog_test.cpp" "$H/ui/AutoSplitDialog.cpp" -o "$WORK/autosplitdialog_test" $LF
g++ -std=c++17 -O1 -fPIC -I"$H" $CF \
	"$HERE/recordingparts_test.cpp" -o "$WORK/recordingparts_test" $LF

# Preview failures stay visible and copyable without interrupting playback or
# logging the same compiler failure on every frame.
g++ -std=c++17 -O1 -fPIC -I"$H" $CF \
	"$HERE/previewwarning_test.cpp" -o "$WORK/previewwarning_test" $LF
# The colour emoji in text clips (editor/timeline/Emoji.cpp) decode their
# pictures with FFmpeg: wherever the compositor is built, so is that.
EMOJI_FLAGS="$(pkg-config --cflags --libs libavcodec libswscale libavutil)"

# yt-dlp from the editor: the -J reply, the argument list, progress lines,
# and finding (or not finding) the executable.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/ytdlp_test.cpp" "$H/editor/ytdlp/YtDlp.cpp" -o "$WORK/ytdlp_test" $LF

# Multi-Cut: cut shuffling and middle-button navigation in both lanes.
"$MOC" -I"$H" "$H/editor/TrackEditor.hpp" -o "$WORK/moc_TrackEditor_mc.cpp"
for name in multicutshuffle trackpan; do
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/${name}_test.cpp" "$H/editor/TrackEditor.cpp" "$WORK/moc_TrackEditor_mc.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/ui/UiText.cpp" -o "$WORK/${name}_test" $LF
done

# Two commands must never end up sharing a key: Qt fires neither.
"$MOC" -I"$H" "$H/editor/ShortcutRegistry.hpp" -o "$WORK/moc_ShortcutRegistry.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/shortcut_dupkey_test.cpp" "$H/editor/ShortcutRegistry.cpp" \
	"$WORK/moc_ShortcutRegistry.cpp" -o "$WORK/shortcut_dupkey_test" $LF

# The keyframe list, and the add/remove rules under it.
"$MOC" -I"$H" "$H/editor/KeyList.hpp" -o "$WORK/moc_KeyList.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/keylist_test.cpp" "$H/editor/KeyList.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/ui/UiIcons.cpp" \
	"$WORK/moc_KeyList.cpp" -o "$WORK/keylist_test" $LF

# The slider+spin pair behind every effect parameter.
"$MOC" -I"$H" "$H/editor/ParamSlider.hpp" -o "$WORK/moc_ParamSlider.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/paramslider_test.cpp" "$H/editor/ParamSlider.cpp" \
	"$WORK/moc_ParamSlider.cpp" -o "$WORK/paramslider_test" $LF

# The app-wide text scale and the role sizes derived from it.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/uitext_test.cpp" "$H/ui/UiText.cpp" -o "$WORK/uitext_test" $LF

# The colour convention: a swatch that carries its colour, and a Cancel that
# puts back what was there rather than the last thing hovered.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/colorfield_test.cpp" -o "$WORK/colorfield_test" $LF

# Auto-stop when the pointer leaves the recording region: the coordinate
# conversion and the countdown. No libobs needed -- CaptureManager.hpp is
# obs-free and RegionWatch is deliberately separable from MainWindow.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/regionwatch_test.cpp" "$H/core/RegionWatch.cpp" -o "$WORK/regionwatch_test" $LF

# Send to Full Editing: Trim / Multi-Cut results as timeline clips. The output
# time arithmetic is what rots silently -- a bad offset leaves a gap that looks
# like a decoding fault rather than an off-by-one.
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=0 -I"$H" -I"$ROOT" $CF \
	"$HERE/sendtofull_test.cpp" -o "$WORK/sendtofull_test" $LF

# Component In/Out ramps: the weight curve, and what the weight DOES. Both
# ramps zero is the default and must render byte-identically to before -- that
# compatibility check is the load-bearing one.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/componentenvelope_test.cpp" -o "$WORK/componentenvelope_test" $LF
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/envelopestack_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" -o "$WORK/envelopestack_test" $LF

# Global hotkeys: the QKeySequence -> Win32 (mods, VK) translation. A wrong VK
# registers FINE and then fires for a different key -- no error path reports
# it -- so the ABI constants are pinned here, on every platform.
"$MOC" -I"$H" "$H/core/GlobalHotkeys.hpp" -o "$WORK/moc_GlobalHotkeys.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/globalhotkeys_test.cpp" "$H/core/GlobalHotkeys.cpp" \
	"$WORK/moc_GlobalHotkeys.cpp" -o "$WORK/globalhotkeys_test" $LF

# The shared auto-resume gate: three auto-pause systems (idle, focus, region)
# used to resume off their own flag alone, putting slices of the wrong screen
# in the file. The cross terms in this matrix ARE the bug.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/autopause_test.cpp" -o "$WORK/autopause_test" $LF

# The crash report: handlers that write a description on the way down, and the
# next start that reads it. Crashes REAL child processes -- a handler with a
# typo is indistinguishable from no handler until the day it matters.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/crashreport_test.cpp" "$H/core/CrashGuard.cpp" -o "$WORK/crashreport_test" $LF

# Follow Mouse: the dead zone, the target arithmetic and the exponential chase.
# The chase caught a real design bug on its first run -- a target measured
# against the smoothed position never settles -- so the "settled means SETTLED"
# check is load-bearing.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/followmouse_test.cpp" "$H/core/FollowMouse.cpp" -o "$WORK/followmouse_test" $LF

# Multi-Area: the switcher's rules (gaps, hover delay, overlaps, pans) with an
# injected clock, and the Arrange overlay driven with real mouse/key events.
"$MOC" -I"$H" "$H/ui/MultiAreaOverlay.hpp" -o "$WORK/moc_MultiAreaOverlay.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/multiarea_test.cpp" "$H/ui/MultiAreaOverlay.cpp" "$WORK/moc_MultiAreaOverlay.cpp" \
	-o "$WORK/multiarea_test" $LF

# Automatic Zoom: the animated crop that libobs scales back up. Aspect drift is
# invisible on a still frame and unmissable on a face, so the aspect sweep --
# with its deliberately-stretched control -- is the load-bearing check here.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/zoommode_test.cpp" "$H/core/FollowMouse.cpp" -o "$WORK/zoommode_test" $LF

# The recording spotlight. Every number here is baked into a video file that
# cannot be un-baked, so the geometry, the darkness cap and the fade all get
# pinned -- including the dirty region, whose failure mode is a bright smear
# left behind the cursor in the FILE.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/spotlight_fx_test.cpp" -o "$WORK/spotlight_fx_test" $LF

# Extract Audio Only: the edit arithmetic. All of it operates on a buffer of
# samples, where a mistake is inaudible in review and obvious in the exported
# file -- a trim computed in samples rather than frames swaps the stereo
# channels for the rest of the export.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/audioextract_test.cpp" -o "$WORK/audioextract_test" $LF

# Audio Only recording: the tap's bookkeeping, its scratch file and the encoder.
# Every failure mode of a mode with no picture is silent, so two of these checks
# write real audio and read it back through FFmpeg -- in particular the float
# WAV header, which is what the user is left holding if the encode fails.
AVCF="$(pkg-config --cflags libavcodec libavformat libavutil libswresample libavfilter)"
AVLF="$(pkg-config --libs libavcodec libavformat libavutil libswresample libavfilter)"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF $AVCF \
	"$HERE/audiotap_test.cpp" "$H/core/AudioFileWriter.cpp" "$H/editor/AudioExtract.cpp" \
	"$H/library/ClipLibrary.cpp" \
	-o "$WORK/audiotap_test" $LF $AVLF

# Preset equality, which Cancel in the preset editor asks before warning that it
# is about to discard work. A field left out of the comparison makes two
# different presets compare equal, and the edit is thrown away silently -- so
# every field is pinned by changing it and demanding a difference.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/preset_equality_test.cpp" "$H/model/Preset.cpp" \
	-o "$WORK/preset_equality_test" $LF

# Which options apply in which capture mode. The alternative to this table is an
# `if (mode == Region)` at a dozen call sites, which drifts -- and the symptom of
# drift is a setting that looks switched on while doing nothing.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/modecapabilities_test.cpp" -o "$WORK/modecapabilities_test" $LF

# Composition guides: where each line goes, the paint, and -- the one that
# matters -- that they are never on screen during a recording the system
# cannot keep them out of.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/guides_test.cpp" "$H/ui/GuidesOverlay.cpp" "$H/model/Preset.cpp" \
	-o "$WORK/guides_test" $LF

# Recording Windows audio in the voiceover panel: the source ids beside the
# microphones', the capture where there is no Windows, and the silence trim.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/systemaudio_test.cpp" "$H/editor/SystemAudioCapture.cpp" "$H/editor/WavSilence.cpp" \
	-o "$WORK/systemaudio_test" $LF -lpthread

# PC audio from: all apps, only some, or all but one -- what is captured for
# each, the saved choice, and the picker.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/pcaudioapps_test.cpp" "$H/ui/PcAudioAppsDialog.cpp" \
	-o "$WORK/pcaudioapps_test" $LF

# Two actions on one key: nothing reports it and the second feature just never
# fires, so the rule is that it cannot be saved at all.
"$MOC" -I"$H" "$H/ui/ShortcutConflictDialog.hpp" -o "$WORK/moc_ShortcutConflictDialog.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/shortcutconflict_test.cpp" "$H/ui/ShortcutConflictDialog.cpp" \
	"$WORK/moc_ShortcutConflictDialog.cpp" -o "$WORK/shortcutconflict_test" $LF

# The component runtime: purity, stage order, dependencies, serialisation.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/component_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Transitions.cpp" \
	-o "$WORK/component_test" $LF

# Custom components, in JavaScript. Needs QuickJS, which takes ~11s to compile
# and would otherwise be paid on every run of what is meant to be the fast
# suite -- so the archive is cached outside $WORK and rebuilt only when the
# vendored sources actually change.
QJS="$H/third_party/quickjs"
QJSLIB="${TMPDIR:-/tmp}/harpia-qjs-units"
mkdir -p "$QJSLIB"
newest_src="$(ls -t "$QJS"/*.c "$QJS"/*.h 2>/dev/null | head -1)"
if [ ! -f "$QJSLIB/libqjs.a" ] || [ "$newest_src" -nt "$QJSLIB/libqjs.a" ]; then
	echo "  (building QuickJS for the component script test, one time)"
	for f in dtoa libregexp libunicode quickjs; do
		gcc -std=c11 -O1 -fPIC -D_GNU_SOURCE -I"$QJS" -c "$QJS/$f.c" -o "$QJSLIB/$f.o" &
	done
	wait
	ar rcs "$QJSLIB/libqjs.a" "$QJSLIB"/*.o
fi
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $CF \
	"$HERE/scriptcomponent_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/ScriptComponent.cpp" \
	-o "$WORK/scriptcomponent_test" "$QJSLIB/libqjs.a" $LF

# The editing console: one line of JavaScript against a copy of the model,
# read back only on success; pixels in, fractions stored; templates by name.
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $CF \
	"$HERE/console_test.cpp" "$H/editor/EditConsole.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/console_test" "$QJSLIB/libqjs.a" $LF

# Subtitles from speech: API JSON -> words -> captions -> clips aligned to the
# media clip; the Subtitle component; the audio downmix; the request body.
"$MOC" -I"$H" "$H/editor/subtitles/SpeechTranscriber.hpp" -o "$WORK/moc_SpeechTranscriber.cpp"
"$MOC" -I"$H" "$H/editor/subtitles/SubtitleDialog.hpp" -o "$WORK/moc_SubtitleDialog.cpp"
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $CF $AVCF \
	"$HERE/subtitles_test.cpp" \
	"$H/editor/subtitles/SpeechTranscriber.cpp" "$WORK/moc_SpeechTranscriber.cpp" \
	"$H/editor/subtitles/SubtitleDialog.cpp" "$WORK/moc_SubtitleDialog.cpp" \
	"$H/editor/subtitles/SecretStore.cpp" "$H/editor/subtitles/AudioForSpeech.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" $EMOJI_FLAGS \
	"$H/editor/VoiceoverMixer.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/component/ShaderComponent.cpp" "$H/editor/component/ScriptComponent.cpp" \
	"$H/editor/component/TransformScriptComponent.cpp" \
	"$H/editor/shader/ShaderRenderer.cpp" "$H/editor/script/TransformScript.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/subtitles_test" "$QJSLIB/libqjs.a" $LF $AVLF

# The searchable picker behind Add Component / Add effect: fuzzy ranking,
# folders until you type, keys, and that every close reports once.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/searchpicker_test.cpp" "$H/ui/SearchPicker.cpp" \
	-o "$WORK/searchpicker_test" $LF

# Components through the REAL compositor -- the path the preview and the
# exporter share. Determinism here is what "preview equals export" means.
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $CF \
	"$HERE/componentrender_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/component/ScriptComponent.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" $EMOJI_FLAGS "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/shader/SpotlightGl.cpp" "$H/editor/script/TransformScript.cpp" \
	-o "$WORK/componentrender_test" "$QJSLIB/libqjs.a" $LF

# Rich text in captions (<b>, <color=...>, <size=...>): the parser, drawn
# through the compositor, the typewriter and the tag-free labels.
for name in richtext captionanimation; do
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $CF \
	"$HERE/${name}_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/component/ScriptComponent.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" $EMOJI_FLAGS "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/shader/SpotlightGl.cpp" "$H/editor/script/TransformScript.cpp" \
	-o "$WORK/${name}_test" "$QJSLIB/libqjs.a" $LF
done

# Caption clock sharing and per-thread whitespace matching preserve edits and
# concurrent preview/export evaluation without retaining stale caption content.
g++ -std=c++17 -O1 -fPIC -pthread -I"$H" -I"$ROOT" $CF \
	"$HERE/captionperf_test.cpp" -o "$WORK/captionperf_test" $LF

# Repeated colour reads do not repolish widgets, while changed/mixed colours
# still update their appearance and picker metadata.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/colorswatch_test.cpp" -o "$WORK/colorswatch_test" $LF

# Loading tasks keep GUI timers alive, transfer owned results and propagate errors.
g++ -std=c++17 -O1 -fPIC -pthread -I"$H" -I"$ROOT" $CF \
	"$HERE/loadingtask_test.cpp" -o "$WORK/loadingtask_test" $LF
# Loading feedback reports real item counts and stays visible until completion.
"$MOC" -I"$H" "$H/ui/LoadingDialog.hpp" -o "$WORK/moc_LoadingDialog.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/loadingdialog_test.cpp" "$H/ui/LoadingDialog.cpp" "$WORK/moc_LoadingDialog.cpp" \
	-o "$WORK/loadingdialog_test" $LF

# Silence detection and globally synchronized timeline cuts do not rewrite media.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/silencetrim_test.cpp" "$H/editor/SilenceTrim.cpp" -o "$WORK/silencetrim_test" $LF

# Musical pulse detection rejects silence/steady tones and follows real attacks.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/beatdetector_test.cpp" "$H/editor/BeatDetector.cpp" -o "$WORK/beatdetector_test" $LF

# Named timeline markers and the output end survive saves, slices and ripple edits.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/markersmodel_test.cpp" "$H/editor/SilenceTrim.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	-o "$WORK/markersmodel_test" $LF

# An explicit video end bounds the audio prefix without shifting its fades.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF $AVCF \
	"$HERE/markeraudio_test.cpp" "$H/editor/TimelineAudio.cpp" \
	"$H/editor/VoiceoverMixer.cpp" "$H/editor/AudioRetimer.cpp" \
	-o "$WORK/markeraudio_test" $LF $AVLF

# Social media text styles use installed fonts and preserve caption content
# when applied or saved through the existing style/project JSON format.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/textstylepresets_test.cpp" -o "$WORK/textstylepresets_test" $LF

# Batch export names, folders and summaries (header-only helpers).
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/batchplan_test.cpp" -o "$WORK/batchplan_test" $LF

# Text variations: counting, order, substitution and file names (header-only).
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/textvariations_test.cpp" -o "$WORK/textvariations_test" $LF

# Random clips: the shown clip and the list in step, what may be added and
# removed, trims applied to every clip, and versions counted in with texts.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/clipvariations_test.cpp" -o "$WORK/clipvariations_test" $LF

# Opaque frames tagged RGBX8888 composite to the same bytes as RGBA8888 in
# every pose, and a mask still cuts them (the compositor retags first).
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $CF \
	"$HERE/opaqueframe_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/component/ScriptComponent.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" $EMOJI_FLAGS "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/shader/SpotlightGl.cpp" "$H/editor/script/TransformScript.cpp" \
	-o "$WORK/opaqueframe_test" "$QJSLIB/libqjs.a" $LF

# Large stills: what is heavy, the working and preview copies, and a crop that
# cuts the same part of a smaller copy through the real compositor.
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $CF \
	"$HERE/stillweight_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/component/ScriptComponent.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" $EMOJI_FLAGS "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/shader/SpotlightGl.cpp" "$H/editor/script/TransformScript.cpp" \
	-o "$WORK/stillweight_test" "$QJSLIB/libqjs.a" $LF

# The preview caches are invisible: the finished-frame key changes exactly when
# the frame would, and a caption from the bitmap cache matches one drawn directly.
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $CF \
	"$HERE/previewcache_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/component/ScriptComponent.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" $EMOJI_FLAGS "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/shader/SpotlightGl.cpp" "$H/editor/script/TransformScript.cpp" \
	-o "$WORK/previewcache_test" "$QJSLIB/libqjs.a" $LF

# Effects on the GPU give the CPU's answer: every effect both ways, compared.
# Needs a GL 3.3 context, so it runs under Xvfb when that is installed.
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=1 -DHARPIA_SRC="\"$H\"" -I"$H" -I"$QJS" -I"$ROOT" $CF \
	"$HERE/gpufx_test.cpp" "$H/editor/shader/GpuFxGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/component/ScriptComponent.cpp" "$H/editor/component/ShaderComponent.cpp" \
	"$H/editor/shader/ShaderRenderer.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" $EMOJI_FLAGS "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/shader/SpotlightGl.cpp" "$H/editor/script/TransformScript.cpp" \
	-o "$WORK/gpufx_test" "$QJSLIB/libqjs.a" $LF

# The Color Overlay shader and the dropdown a shader can declare: parsed, a
# dropdown in the component, every blend rendered on a picture with soft and
# transparent edges, and the GPU chain agreeing with the CPU path.
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=1 -DHARPIA_SRC="\"$H\"" -I"$H" -I"$QJS" -I"$ROOT" $CF \
	"$HERE/coloroverlay_test.cpp" "$H/editor/shader/GpuFxGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/component/ScriptComponent.cpp" "$H/editor/component/ShaderComponent.cpp" \
	"$H/editor/shader/ShaderRenderer.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" $EMOJI_FLAGS "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/shader/SpotlightGl.cpp" "$H/editor/script/TransformScript.cpp" \
	-o "$WORK/coloroverlay_test" "$QJSLIB/libqjs.a" $LF

# Effects as components: every FxType registered, rendering pixel-for-pixel
# what the old path did, and an unmigrated project opening with its grade.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/effectcomponent_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/effectcomponent_test" $LF

# Shaders and transform scripts as components: one type per file in the folder,
# //@param lines becoming properties, a script composing with the clip's pose,
# and an old project's `scripts` list migrating exactly once.
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$ROOT" -I"$QJS" $CF \
	"$HERE/assetcomponent_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/component/ShaderComponent.cpp" \
	"$H/editor/component/TransformScriptComponent.cpp" \
	"$H/editor/script/TransformScript.cpp" "$H/editor/shader/ShaderRenderer.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/assetcomponent_test" "$QJSLIB/libqjs.a" $LF

# An effect clip grades only inside its own transform area -- and a clip nobody
# moved still covers the whole canvas, which every existing project relies on.
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$ROOT" -I"$QJS" $CF \
	"$HERE/effectarea_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" $EMOJI_FLAGS "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/script/TransformScript.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/effectarea_test" "$QJSLIB/libqjs.a" $LF

# A track's lock/hide/mute toggles stay inside the gutter at every width the Dev
# panel can set, and nothing paints over them.
"$MOC" -I"$H" "$H/editor/timeline/TimelineView.hpp" -o "$WORK/moc_TimelineView.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/gutterspill_test.cpp" "$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/gutterspill_test" $LF

# The volume rubber-band on an audio clip: the line's height IS the level, the
# grab is the line and not the clip, and every step emits clipsChanged -- which
# is what rebuilds the audio the preview plays.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/clipvolume_test.cpp" "$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/clipvolume_test" $LF

# Batch style for a caption lane: every text clip on the lane, nothing else,
# not a locked lane, and one coalesced undo step.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/captionlane_test.cpp" "$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/captionlane_test" $LF

# The track list: a typed name survives renumbering, an effect lane can be
# hidden but not muted, only self-made lanes vanish when emptied, and a locked
# lane takes no clip from a menu action either. Plus the two sound controls:
# Solo (a set, not a radio button; mute still wins) and a per-lane gain. A
# duplicated lane keeps a typed name, numbered; a clip's volume counts on a
# video lane too.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/tracks_test.cpp" "$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/tracks_test" $LF

# Slip: the footage moves inside a clip that keeps its place and length; its
# linked sound follows, it stops at the source's ends, one drag is one undo.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/slip_test.cpp" "$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/slip_test" $LF

# One duration for selected videos: trim at each clip's speed, cap at the
# shortest clip, preserve the selection, and accept/cancel through the menu.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/clipduration_test.cpp" "$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/clipduration_test" $LF

# Sounds for events: a rule per kind of thing ("every crossfade plays this
# whoosh"), its clips derived and rebuilt on every change on a locked Sounds
# lane; freeze hands them over; the JSON round trip; the built-in sounds.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/soundrules_test.cpp" "$H/editor/timeline/SoundRules.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	-o "$WORK/soundrules_test" $LF

# Sound Library and presets: the built-ins always present, your file wins,
# a missing file falls back; presets carry tags and rules by name and slot,
# and merging maps tags, skips duplicates and counts missing sounds.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/soundlibrary_test.cpp" "$H/editor/timeline/SoundLibrary.cpp" \
	"$H/editor/timeline/SoundPresets.cpp" "$H/editor/timeline/SoundRules.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	-o "$WORK/soundlibrary_test" $LF

# Randomising clip order: permutations, pins, strengths, "avoid the same
# order", the re-layout, and one undo step per press in the timeline.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/clipshuffle_test.cpp" "$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/clipshuffle_test" $LF

# Shift+drag: a box that selects every clip it touches, through the real view.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/marquee_test.cpp" "$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/marquee_test" $LF

# Text to speech, the pure half: each service's request, its reply as audio
# and word times, cutting a long text, matching written words to heard ones,
# joining the audio, and the linked place on the timeline.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/tts_test.cpp" "$H/editor/component/Component.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/ComponentRegistry.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/component/ComponentStack.cpp" \
	-o "$WORK/tts_test" $LF

# Linked clips move as one: clicking one selects the group, a drag or a nudge
# moves every member, a pasted copy is a group of its own, and it is saved.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/linkedclips_test.cpp" "$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/linkedclips_test" $LF

# The timeline must not rescale itself while a clip is being dragged: every
# pixel comes from the project's total span, and dragging past the end grows it.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/timelinedragspan_test.cpp" "$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/timelinedragspan_test" $LF

# Keyframes on the timeline: every kind of key is drawn, a pip can be grabbed
# and retimed without moving its clip, and the magnet knows about markers and
# keys as well as clip edges.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/timelinekeys_test.cpp" "$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/timelinekeys_test" $LF

# The ruler marker row uses real pointer gestures and commits each drag once.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/markers_test.cpp" "$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/markers_test" $LF

# Where a clip was split: the drawn shapes leave a gap the eye can see, the hit
# rects still tile so the gap is not a dead strip, and only a REAL split -- one
# whose halves would rejoin seamlessly -- gets the seam mark.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/splitseam_test.cpp" "$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/splitseam_test" $LF

# Grabbing the timeline and sliding it: the moment under the pointer stays put,
# releasing ends it, and the gestures that already worked still do.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/timelinepan_test.cpp" "$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/timelinepan_test" $LF

# Hit-testing must not go quadratic again: every "where is this on screen"
# question used to re-measure the project's whole duration.
g++ -std=c++17 -O2 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/timelineperf_test.cpp" "$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/timelineperf_test" $LF

# One app at a time. Re-launches ITSELF, because two objects in one process is
# not the situation that actually happens.
"$MOC" -I"$H" "$H/ui/SingleInstance.hpp" -o "$WORK/moc_SingleInstance.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/singleinstance_test.cpp" "$H/ui/SingleInstance.cpp" \
	"$WORK/moc_SingleInstance.cpp" -o "$WORK/singleinstance_test" $LF

# Every component property can be keyed -- including the ones that are not
# numbers you can slide between (a Bool holds; a Colour moves channel by channel).
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/propkeys_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Transitions.cpp" \
	-o "$WORK/propkeys_test" $LF

# Splitting a pixel pass across cores must not change a pixel: identical output
# at 1..8 row bands, including counts that divide the height unevenly.
g++ -std=c++17 -O2 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/pixelbands_test.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/pixelbands_test" $LF

# Masking a clip to a shape: the outside is CLEARED, not darkened, and the track
# below shows through the hole.
g++ -std=c++17 -O2 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$ROOT" -I"$QJS" $CF \
	"$HERE/mask_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" $EMOJI_FLAGS "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/script/TransformScript.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/mask_test" "$QJSLIB/libqjs.a" $LF

# A caption wider than the picture wraps at a space and carries on below;
# typed line breaks are kept and a single long word is left whole.
# Outlines keep the same proportion in reduced previews and exports.
for name in textwrap textstylescale textbackground; do
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$ROOT" -I"$QJS" $CF \
	"$HERE/${name}_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" $EMOJI_FLAGS "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/script/TransformScript.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/${name}_test" "$QJSLIB/libqjs.a" $LF
done

# Colour emoji in text clips: which characters are emoji, the pictures, and a
# caption with an emoji in it drawn in colour rather than as a one-colour shape.
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$ROOT" -I"$QJS" $CF \
	"$HERE/emoji_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" $EMOJI_FLAGS \
	"$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/script/TransformScript.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/emoji_test" "$QJSLIB/libqjs.a" $LF

# "Export this clip": the sliced timeline has to RENDER what the project
# rendered, which is a claim about pixels rather than about field arithmetic.
g++ -std=c++17 -O2 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$ROOT" -I"$QJS" $CF \
	"$HERE/timelineslice_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" $EMOJI_FLAGS "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" "$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" \
	"$WORK/moc_TimelineView.cpp" \
	"$H/editor/script/TransformScript.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/timelineslice_test" "$QJSLIB/libqjs.a" $LF

# Playback decodes sequentially, and one source can be on screen from several
# tracks at once at different timestamps. Keyed by source alone, the lower
# track freezes on the upper one's frame. The control here keeps the old keying
# alive so the check is demonstrably able to tell the two apart.
g++ -std=c++17 -O2 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$ROOT" -I"$QJS" $CF \
	"$HERE/playbacktracks_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" $EMOJI_FLAGS "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" "$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" \
	"$WORK/moc_TimelineView.cpp" \
	"$H/editor/script/TransformScript.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/playbacktracks_test" "$QJSLIB/libqjs.a" $LF

# "Zoom here": the clicked point has to land in the MIDDLE, and pushing in near
# an edge must not drag the frame off the canvas.
g++ -std=c++17 -O2 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$ROOT" -I"$QJS" $CF \
	"$HERE/zoomkeyframes_test.cpp" "$H/editor/timeline/ZoomKeyframes.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" $EMOJI_FLAGS "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/script/TransformScript.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/zoomkeyframes_test" "$QJSLIB/libqjs.a" $LF

# Saving a project and opening it must give back what you had. Found the Bezier
# handles being dropped whenever a key's ease was not Bezier, in three places.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/projectroundtrip_test.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	-o "$WORK/projectroundtrip_test" $LF

# Save and open a project with EVERY field of every kind set off its default,
# through the functions Save and Open use and a real file, compared by name.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/projectfull_test.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	-o "$WORK/projectfull_test" $LF

# The recorder window's Developer Panel Copy/Paste JSON: every size, both ways.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/mainlayoutjson_test.cpp" -o "$WORK/mainlayoutjson_test" $LF

# Multi-Cut with mixed resolutions: adding a clip must not re-shape the project,
# and an odd-shaped source previews letterboxed, the way it will be encoded.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/canvasfit_test.cpp" -o "$WORK/canvasfit_test" $LF

# A playback tick repaints only the band the playhead moves across -- in the
# Multi-Cut track editor, the voiceover track and the Simple Trim bar -- not
# the whole widget 30 times a second. Also verify navigation in Simple Trim
# and voiceover never changes clips, selection, trim boundaries or playhead.
for h in editor/EditorWidgets editor/TrackEditor editor/VoiceoverTrack editor/ParamSlider; do
	"$MOC" -I"$H" "$H/$h.hpp" -o "$WORK/moc_ph_$(basename "$h").cpp"
done
for name in playheadrepaint trimpan voiceoverpan; do
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/${name}_test.cpp" "$H/editor/EditorWidgets.cpp" "$H/editor/TrackEditor.cpp" \
	"$H/editor/VoiceoverTrack.cpp" "$H/editor/ParamSlider.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK"/moc_ph_*.cpp \
	-o "$WORK/${name}_test" $LF
done

# Changing the project resolution must not stretch the picture. Renders the real
# PreviewCanvas, so it links the widget and what it draws with.
"$MOC" -I"$H" "$H/editor/EditorWidgets.hpp" -o "$WORK/moc_EditorWidgets.cpp"
"$MOC" -I"$H" "$H/editor/ParamSlider.hpp" -o "$WORK/moc_ParamSlider2.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/previewaspect_test.cpp" "$H/editor/EditorWidgets.cpp" "$H/editor/ParamSlider.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" \
	"$WORK/moc_EditorWidgets.cpp" "$WORK/moc_ParamSlider2.cpp" \
	-o "$WORK/previewaspect_test" $LF

# Area from a window: the window under the pointer (never our overlay), its
# monitor, and its area clipped, monitor-relative and even-sized.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/windowregion_test.cpp" -o "$WORK/windowregion_test" $LF

# The window picker's layer: hover outlines, click picks, never our own
# window, Esc / right-click cancel.
"$MOC" -I"$H" "$H/ui/WindowPicker.hpp" -o "$WORK/moc_WindowPicker.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/windowpicker_test.cpp" "$H/ui/WindowPicker.cpp" "$H/platform/nix/NixWindowList.cpp" \
	"$WORK/moc_WindowPicker.cpp" -o "$WORK/windowpicker_test" $LF

# Custom Region by dragging over it: the rectangle a drag makes (snapped,
# clamped, at 2x), and the layer with real mouse and key events.
"$MOC" -I"$H" "$H/ui/RegionDraw.hpp" -o "$WORK/moc_RegionDraw.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/regiondraw_test.cpp" "$H/ui/RegionDraw.cpp" "$H/ui/NativeWindowOps.cpp" \
	"$WORK/moc_RegionDraw.cpp" -o "$WORK/regiondraw_test" $LF

# Curved motion paths: Bezier segments, automatic and dragged handles, an even
# pace along the curve, and the line the preview draws.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/curvepath_test.cpp" "$H/editor/timeline/Spotlight.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/curvepath_test" $LF

# Deleting recordings: the Delete key wherever the window's keyboard is (never
# while typing), the question, and the recycle bin.
"$MOC" -I"$H" "$H/ui/RecentListWidget.hpp" -o "$WORK/moc_RecentListWidget.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/recordingdelete_test.cpp" "$H/ui/RecordingDelete.cpp" "$WORK/moc_RecentListWidget.cpp" \
	-o "$WORK/recordingdelete_test" $LF

# "Harpia crashed last time": a 300-line stack still fits the screen, scrolls,
# and copies; the buttons close it.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/crashreportdialog_test.cpp" "$H/ui/CrashReportDialog.cpp" -o "$WORK/crashreportdialog_test" $LF

# Deleting a preset: which folders are the same, who else saves there, the
# question, and that only "Delete preset" says yes (Enter keeps it).
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/presetdelete_test.cpp" -o "$WORK/presetdelete_test" $LF

# The OS integration: the graphics-card registry value, the off-Windows stub,
# and the System settings page.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/osintegration_test.cpp" "$H/ui/SystemSettingsWidget.cpp" "$H/platform/OsIntegrationStub.cpp" \
	-o "$WORK/osintegration_test" $LF

# The keyframe list's reorder (poses move, times stay), Reset value and Delete.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/keyorder_test.cpp" "$H/editor/timeline/Spotlight.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/keyorder_test" $LF

# The motion path: position keys only, speed dots, a click back to a time;
# pick, drag, add and right-click on the preview; Delete takes the picked key.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/motionpath_test.cpp" "$H/editor/EditorWidgets.cpp" "$H/editor/ParamSlider.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" \
	"$WORK/moc_EditorWidgets.cpp" "$WORK/moc_ParamSlider2.cpp" \
	-o "$WORK/motionpath_test" $LF

# The pointer over a grip says what a press will do: resize cursors that follow
# a turned box's own axes, a hand over the body, a rotate cursor on the knob.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/gripcursor_test.cpp" "$H/editor/EditorWidgets.cpp" "$H/editor/ParamSlider.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" \
	"$WORK/moc_EditorWidgets.cpp" "$WORK/moc_ParamSlider2.cpp" \
	-o "$WORK/gripcursor_test" $LF

# Social interface margins only shade the widget; clean frames and drags remain unchanged.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/socialsafeareas_test.cpp" "$H/editor/EditorWidgets.cpp" "$H/editor/ParamSlider.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" \
	"$WORK/moc_EditorWidgets.cpp" "$WORK/moc_ParamSlider2.cpp" \
	-o "$WORK/socialsafeareas_test" $LF

# The floating "where am I" overview for zoomed-in timelines: the mapping from
# project time to the red viewport box, and the show/hide rules.
"$MOC" -I"$H" "$H/editor/TimelineOverview.hpp" -o "$WORK/moc_TimelineOverview.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/timelineoverview_test.cpp" "$H/editor/TimelineOverview.cpp" \
	"$WORK/moc_TimelineOverview.cpp" -o "$WORK/timelineoverview_test" $LF

# The filmstrip must never draw the same frame twice, however far you zoom.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/filmstrip_test.cpp" -o "$WORK/filmstrip_test" $LF

# Who the Delete key belongs to, in every mode -- including "the text field you
# are typing in", which is the one answer that must never be got wrong.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/deleterouting_test.cpp" -o "$WORK/deleterouting_test" $LF

# The export-finished window: what its thumbnail hands over when dragged, what
# each Copy button puts on the clipboard, and that no action dismisses it.
PHONE_SOURCES=("$H/ui/PhoneSharingDialog.cpp" "$H/core/LocalSend.cpp" "$H/platform/PhoneSharingNativeStub.cpp")
g++ -std=c++17 -O1 -fPIC -I"$H" $CF \
	"$HERE/localsend_test.cpp" "$H/core/LocalSend.cpp" -o "$WORK/localsend_test" $LF
g++ -std=c++17 -O1 -fPIC -I"$H" $CF \
	"$HERE/phonesharing_test.cpp" "${PHONE_SOURCES[@]}" -o "$WORK/phonesharing_test" $LF
"$MOC" -I"$H" "$H/ui/ExportDoneDialog.hpp" -o "$WORK/moc_ExportDoneDialog.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/exportdone_test.cpp" "$H/ui/ExportDoneDialog.cpp" \
	"${PHONE_SOURCES[@]}" "$WORK/moc_ExportDoneDialog.cpp" -o "$WORK/exportdone_test" $LF

# Export progress: cancellation remains visible until cleanup; final success
# alone reaches 100, and rendering estimates cannot leak into audio/finalizing.
"$MOC" -I"$H" "$H/ui/ExportProgressDialog.hpp" -o "$WORK/moc_ExportProgressDialog.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF $(pkg-config --cflags Qt6Test) \
	"$HERE/exportpanel_test.cpp" "$H/ui/ExportProgressDialog.cpp" "$H/ui/UiText.cpp" \
	"$WORK/moc_ExportProgressDialog.cpp" -o "$WORK/exportpanel_test" $LF $(pkg-config --libs Qt6Test)

# Free disk space: the 2 GB threshold, the wording of the warning, and the line
# the recording screen shows.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/diskspace_test.cpp" -o "$WORK/diskspace_test" $LF

# The cover picture inside an exported MP4/MOV: its size, the JPEG's colours,
# and the picker that takes the middle frame of an export of any length.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/coverart_test.cpp" "$H/editor/CoverArt.cpp" \
	-o "$WORK/coverart_test" $LF $(pkg-config --cflags --libs libavcodec libavutil libswscale)

# Dev panel: "Reset to defaults" must mean the tab you are on. Links the real
# panel and the four track widgets it edits, because the property under test is
# that the OTHER pages are left alone -- which only the real thing can show.
# StillImage/FrameSeeker come along with the panel's widgets, hence libav.
AVCF="$(pkg-config --cflags libavformat libavcodec libavutil libswscale)"
AVLF="$(pkg-config --libs libavformat libavcodec libavutil libswscale)"
for h in editor/DevPanel editor/EditorWidgets editor/TrackEditor editor/VoiceoverTrack \
	 editor/timeline/TimelineView editor/timeline/KeyframeEditor editor/ParamSlider \
	 editor/component/ComponentPanel; do
	"$MOC" -I"$H" "$H/$h.hpp" -o "$WORK/moc_dp_$(basename "$h").cpp"
done
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=0 -I"$H" -I"$ROOT" $CF $AVCF \
	"$HERE/devpanelreset_test.cpp" \
	"$H/editor/DevPanel.cpp" "$H/editor/EditorWidgets.cpp" "$H/editor/TrackEditor.cpp" \
	"$H/editor/VoiceoverTrack.cpp" "$H/editor/timeline/TimelineView.cpp" "$H/editor/timeline/SoundRules.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/timeline/KeyframeEditor.cpp" "$H/editor/ParamSlider.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/shader/ShaderRenderer.cpp" "$H/editor/script/TransformScript.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H"/editor/component/*.cpp "$H/editor/StillImage.cpp" "$H/editor/FrameSeeker.cpp" "$H/editor/HdrToSdr.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK"/moc_dp_*.cpp \
	-o "$WORK/devpanelreset_test" $LF $AVLF

# The keyframe editor's tabs: one per transform channel, plus one for every
# component property that actually has keys -- and none for the ones that do not.
"$MOC" -I"$H" "$H/editor/timeline/KeyframeEditor.hpp" -o "$WORK/moc_kf_KeyframeEditor.cpp"
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=0 -I"$H" -I"$ROOT" $CF \
	"$HERE/keyframetabs_test.cpp" "$H/editor/timeline/KeyframeEditor.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/shader/ShaderRenderer.cpp" "$H/editor/script/TransformScript.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/component/ShaderComponent.cpp" "$H/editor/component/ScriptComponent.cpp" \
	"$H/editor/component/TransformScriptComponent.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_kf_KeyframeEditor.cpp" \
	-o "$WORK/keyframetabs_test" $LF

# Hovering the timeline turns the Inspector into a read-only readout of the
# frame under the pointer -- and hands the controls back when it leaves.
for h in editor/component/ComponentPanel editor/ParamSlider; do
	"$MOC" -I"$H" "$H/$h.hpp" -o "$WORK/moc_hi_$(basename "$h").cpp"
done
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=0 -I"$H" -I"$ROOT" $CF \
	"$HERE/hoverinspect_test.cpp" "$H/editor/component/ComponentPanel.cpp" "$H/ui/SearchPicker.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/component/ShaderComponent.cpp" "$H/editor/component/ScriptComponent.cpp" \
	"$H/editor/component/TransformScriptComponent.cpp" "$H/editor/ParamSlider.cpp" \
	"$H/editor/shader/ShaderRenderer.cpp" "$H/editor/script/TransformScript.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK"/moc_hi_*.cpp \
	-o "$WORK/hoverinspect_test" $LF

# Where a project is saved when the user has not said: the base folder, the name
# it gets, and that a suggestion never lands on a file that already exists.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/projectpaths_test.cpp" -o "$WORK/projectpaths_test" $LF

# Recent projects: persistent history and discovery without a GUI or media.
g++ -std=c++17 -O1 -fPIC -I"$H" $(pkg-config --cflags Qt6Core) \
	"$HERE/recentprojects_test.cpp" -o "$WORK/recentprojects_test" $(pkg-config --libs Qt6Core)

# Numeric recording bitrates: decimal storage estimates, use cases and Auto.
g++ -std=c++17 -O1 -fPIC -I"$H" $(pkg-config --cflags Qt6Core) \
	"$HERE/recordingbitrate_test.cpp" -o "$WORK/recordingbitrate_test" $(pkg-config --libs Qt6Core)

# The typewriter reveal: where the cut lands for characters / words / lines, the
# caret's blink, the component through the stack, and a Choice holding between
# keyframes like a Bool does.
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=0 -I"$H" -I"$ROOT" $CF \
	"$HERE/texttyping_test.cpp" "$H/editor/component/Component.cpp" \
	"$H/editor/component/ComponentRegistry.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ShaderComponent.cpp" \
	"$H/editor/component/ScriptComponent.cpp" \
	"$H/editor/component/TransformScriptComponent.cpp" \
	"$H/editor/shader/ShaderRenderer.cpp" "$H/editor/script/TransformScript.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/texttyping_test" $LF

# Slide to Position: every curve 0 -> 1 (Back/Elastic overshoot, Bounce never
# does); the first frame fully outside the picture from each edge and corner
# using the clip's real size; the clip's own pose after the slide; Slide out
# by the same or a chosen edge; a short clip splits the two halves.
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=0 -I"$H" -I"$ROOT" $CF \
	"$HERE/slide_test.cpp" "$H/editor/component/Component.cpp" \
	"$H/editor/component/ComponentRegistry.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/slide_test" $LF

# Bullets on cue: the "[3.5] " marker, build-up versus one-at-a-time, and -- the
# one that decides whether the component is safe to add -- that a caption which
# does not use the syntax comes back exactly as it was typed.
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=0 -I"$H" -I"$ROOT" $CF \
	"$HERE/bullets_test.cpp" "$H/editor/component/Component.cpp" \
	"$H/editor/component/ComponentRegistry.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ShaderComponent.cpp" \
	"$H/editor/component/ScriptComponent.cpp" \
	"$H/editor/component/TransformScriptComponent.cpp" \
	"$H/editor/shader/ShaderRenderer.cpp" "$H/editor/script/TransformScript.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/bullets_test" $LF

# Which reader a source file needs. Audio used to go to the video reader, which
# cannot open a file with no video stream -- so a project could create an audio
# source and never load it back.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/sourcekind_test.cpp" -o "$WORK/sourcekind_test" $LF

# Does the Display dropdown point at the monitor the recording area lives on?
# Two independent lists (OBS's and Qt's) name the same displays.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/monitormatch_test.cpp" "$H/ui/MonitorMatch.cpp" \
	-o "$WORK/monitormatch_test" $LF

# The startup progress bar: weighted by the last run's real timings, and never
# claiming to be finished while it is not.
"$MOC" -I"$H" "$H/ui/StartupSplash.hpp" -o "$WORK/moc_StartupSplash.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/startupsplash_test.cpp" "$H/ui/StartupSplash.cpp" \
	"$WORK/moc_StartupSplash.cpp" -o "$WORK/startupsplash_test" $LF

# Custom Region keeps its frame on screen even when Harpia is not in front --
# lining the region up against another app is the whole point of it.
"$MOC" -I"$H" "$H/ui/RegionTool.hpp" -o "$WORK/moc_RegionTool.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" -I"$ROOT/libobs" $CF \
	"$HERE/regionoverlay_test.cpp" "$H/ui/RegionTool.cpp" "$H/ui/NativeWindowOps.cpp" \
	"$WORK/moc_RegionTool.cpp" -o "$WORK/regionoverlay_test" $LF

# Add > Paste below the window: what the clipboard is read as (files, sound,
# a picture, a link, words), the best-fit lane, and a pasted link fetched
# from a web server inside the test.
"$MOC" -I"$H" "$H/editor/LinkDownload.hpp" -o "$WORK/moc_LinkDownload.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/clipboardread_test.cpp" "$H/editor/ClipboardPaste.cpp" "$H/editor/LinkDownload.cpp" \
	"$WORK/moc_LinkDownload.cpp" -o "$WORK/clipboardread_test" $LF

# The Custom Region on a second display at another scale, to the left of the
# main one: the crop is what the frame visibly surrounds, a frame Windows put
# in the wrong place is moved back, and the draw layer measures from where it
# really is. Two offscreen displays, 125% beside 100%.
cat > "$WORK/twoscreens.json" <<'JSON'
{ "screens": [
  { "name": "MAIN", "x": 0, "y": 0, "width": 1920, "height": 1080, "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1 },
  { "name": "LEFT", "x": -1920, "y": 0, "width": 1920, "height": 1080, "logicalDpi": 120, "logicalBaseDpi": 96, "dpr": 1 }
] }
JSON
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" -I"$ROOT/libobs" $CF \
	"$HERE/regionnative_test.cpp" "$H/ui/RegionTool.cpp" "$H/ui/RegionDraw.cpp" "$H/ui/NativeWindowOps.cpp" \
	"$WORK/moc_RegionTool.cpp" "$WORK/moc_RegionDraw.cpp" -o "$WORK/regionnative_test" $LF

# The floating desktop controls are on screen while idle now, holding the green
# Record button -- so the red "recording" dot and the two dead buttons that used
# to be unconditional both had to learn about the idle state.
"$MOC" -I"$H" "$H/ui/RecorderControlsOverlay.hpp" -o "$WORK/moc_RecorderControlsOverlay.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/recordercontrols_test.cpp" "$H/ui/RecorderControlsOverlay.cpp" "$H/ui/UiIcons.cpp" \
	"$WORK/moc_RecorderControlsOverlay.cpp" -o "$WORK/recordercontrols_test" $LF

rc=0

# Every source file of the app is in its build. These checks compile the files
# they test by name, so a .cpp left out of CMakeLists.txt passes here and then
# fails to link in the real build -- which is how v0.1.426's PC audio picker
# reached a Windows build as six unresolved externals.
echo "-- every source file is in CMakeLists.txt --"
missing=0
for f in $(cd "$H" && ls core/*.cpp ui/*.cpp editor/*.cpp editor/*/*.cpp library/*.cpp model/*.cpp \
		platform/*.cpp platform/*/*.cpp *.cpp 2>/dev/null); do
	if ! grep -qF "$f" "$H/CMakeLists.txt" "$H"/cmake/*.cmake 2>/dev/null; then
		echo "  FAIL $f is not in CMakeLists.txt"
		missing=1
	fi
done
if [ "$missing" = 0 ]; then
	echo "  PASS all of them"
else
	rc=1
fi
# obs_get_output_source() returns a NEW reference: asked anywhere but the one
# helper that releases it, it leaks the channel's source -- which is how a
# capture scene outlived its capture and crashed obs_shutdown().
if grep -rn "obs_get_output_source(" --include=*.cpp --include=*.hpp "$H/core" "$H/ui" "$H/editor" "$H/library" "$H/platform" "$H/main.cpp" 2>/dev/null | grep -v "core/ObsChannel.hpp"; then
	echo "FAIL obs_get_output_source() called outside core/ObsChannel.hpp (leaks a reference)"
	rc=1
else
	echo "  PASS every output-channel lookup releases its reference (core/ObsChannel.hpp)"
fi
QT_QPA_PLATFORM=offscreen "$WORK/shortcut_dupkey_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/ytdlp_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/windowscreen_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/videofolderscan_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/multicutshuffle_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/trackpan_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/keylist_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/paramslider_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/uitext_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/colorfield_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/regionwatch_test" || rc=1
QT_QPA_PLATFORM="offscreen:configfile=$WORK/twoscreens.json" "$WORK/regionnative_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/clipboardread_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/followmouse_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/multiarea_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/zoommode_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/spotlight_fx_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/audioextract_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/modecapabilities_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/guides_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/systemaudio_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/pcaudioapps_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/preset_equality_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/audiotap_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/shortcutconflict_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/crashreport_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/autopause_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/globalhotkeys_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/sendtofull_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/autosplitdialog_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/recordingparts_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/componentenvelope_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/envelopestack_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/regionoverlay_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/recordercontrols_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/startupsplash_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/monitormatch_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/canvasfit_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/previewaspect_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/motionpath_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/playheadrepaint_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/trimpan_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/voiceoverpan_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/gripcursor_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/timelineoverview_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/filmstrip_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/deleterouting_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/exportdone_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/localsend_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/phonesharing_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/exportpanel_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/diskspace_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/devpanelreset_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/projectroundtrip_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/projectfull_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/mainlayoutjson_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/zoomkeyframes_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/component_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/scriptcomponent_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/console_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/subtitles_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/searchpicker_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/componentrender_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/previewwarning_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/opaqueframe_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/batchplan_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/textvariations_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/clipvariations_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/richtext_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/captionanimation_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/captionperf_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/colorswatch_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/loadingtask_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/loadingdialog_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/silencetrim_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/beatdetector_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/markersmodel_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/markeraudio_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/markers_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/socialsafeareas_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/textstylepresets_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/curvepath_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/keyorder_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/recordingdelete_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/presetdelete_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/crashreportdialog_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/osintegration_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/windowregion_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/windowpicker_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/regiondraw_test" "$WORK" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/stillweight_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/previewcache_test" || rc=1
if command -v xvfb-run >/dev/null 2>&1; then
	QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a -s "-screen 0 640x480x24" "$WORK/gpufx_test" || rc=1
	QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a -s "-screen 0 640x480x24" "$WORK/coloroverlay_test" || rc=1
else
	QT_QPA_PLATFORM=offscreen "$WORK/gpufx_test" || rc=1
	QT_QPA_PLATFORM=offscreen "$WORK/coloroverlay_test" || rc=1
fi
QT_QPA_PLATFORM=offscreen "$WORK/effectcomponent_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/assetcomponent_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/effectarea_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/gutterspill_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/timelinekeys_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/timelinedragspan_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/linkedclips_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/tts_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/clipvolume_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/captionlane_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/tracks_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/slip_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/clipduration_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/soundrules_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/soundlibrary_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/clipshuffle_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/marquee_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/keyframetabs_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/hoverinspect_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/projectpaths_test" || rc=1
"$WORK/recentprojects_test" || rc=1
"$WORK/recordingbitrate_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/sourcekind_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/texttyping_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/slide_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/bullets_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/splitseam_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/timelinepan_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/timelineperf_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/singleinstance_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/propkeys_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/pixelbands_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/mask_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/textwrap_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/textstylescale_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/textbackground_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/coverart_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/emoji_test" "$H/resources/emoji/noto-emoji.pack" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/timelineslice_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/playbacktracks_test" || rc=1
exit $rc
