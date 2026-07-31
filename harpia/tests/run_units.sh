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

# Auto-stop when the pointer leaves the recording region: the coordinate
# conversion and the countdown. No libobs needed -- CaptureManager.hpp is
# obs-free and RegionWatch is deliberately separable from MainWindow.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/regionwatch_test.cpp" "$H/core/RegionWatch.cpp" -o "$WORK/regionwatch_test" $LF

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

# Components through the REAL compositor -- the path the preview and the
# exporter share. Determinism here is what "preview equals export" means.
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $CF \
	"$HERE/componentrender_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/component/ScriptComponent.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/shader/SpotlightGl.cpp" "$H/editor/script/TransformScript.cpp" \
	-o "$WORK/componentrender_test" "$QJSLIB/libqjs.a" $LF

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
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/script/TransformScript.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/effectarea_test" "$QJSLIB/libqjs.a" $LF

# A track's lock/hide/mute toggles stay inside the gutter at every width the Dev
# panel can set, and nothing paints over them.
"$MOC" -I"$H" "$H/editor/timeline/TimelineView.hpp" -o "$WORK/moc_TimelineView.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/gutterspill_test.cpp" "$H/editor/timeline/TimelineView.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/gutterspill_test" $LF

# Where a clip was split: the drawn shapes leave a gap the eye can see, the hit
# rects still tile so the gap is not a dead strip, and only a REAL split -- one
# whose halves would rejoin seamlessly -- gets the seam mark.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/splitseam_test.cpp" "$H/editor/timeline/TimelineView.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/splitseam_test" $LF

# Grabbing the timeline and sliding it: the moment under the pointer stays put,
# releasing ends it, and the gestures that already worked still do.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/timelinepan_test.cpp" "$H/editor/timeline/TimelineView.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Transitions.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" "$H/editor/component/ComponentStack.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TimelineView.cpp" \
	-o "$WORK/timelinepan_test" $LF

# Hit-testing must not go quadratic again: every "where is this on screen"
# question used to re-measure the project's whole duration.
g++ -std=c++17 -O2 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/timelineperf_test.cpp" "$H/editor/timeline/TimelineView.cpp" \
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
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/script/TransformScript.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/mask_test" "$QJSLIB/libqjs.a" $LF

# "Export this clip": the sliced timeline has to RENDER what the project
# rendered, which is a claim about pixels rather than about field arithmetic.
g++ -std=c++17 -O2 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$ROOT" -I"$QJS" $CF \
	"$HERE/timelineslice_test.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" "$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/timeline/TimelineView.cpp" "$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" \
	"$WORK/moc_TimelineView.cpp" \
	"$H/editor/script/TransformScript.cpp" "$H/editor/shader/SpotlightGl.cpp" \
	-o "$WORK/timelineslice_test" "$QJSLIB/libqjs.a" $LF

# "Zoom here": the clicked point has to land in the MIDDLE, and pushing in near
# an edge must not drag the frame off the canvas.
g++ -std=c++17 -O2 -fPIC -DHARPIA_HAVE_QJS=1 -I"$H" -I"$ROOT" -I"$QJS" $CF \
	"$HERE/zoomkeyframes_test.cpp" "$H/editor/timeline/ZoomKeyframes.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/EffectClip.cpp" \
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

# Multi-Cut with mixed resolutions: adding a clip must not re-shape the project,
# and an odd-shaped source previews letterboxed, the way it will be encoded.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/canvasfit_test.cpp" -o "$WORK/canvasfit_test" $LF

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

# The floating "where am I" overview for zoomed-in timelines: the mapping from
# project time to the red viewport box, and the show/hide rules.
"$MOC" -I"$H" "$H/editor/TimelineOverview.hpp" -o "$WORK/moc_TimelineOverview.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/timelineoverview_test.cpp" "$H/editor/TimelineOverview.cpp" \
	"$WORK/moc_TimelineOverview.cpp" -o "$WORK/timelineoverview_test" $LF

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
	"$HERE/regionoverlay_test.cpp" "$H/ui/RegionTool.cpp" \
	"$WORK/moc_RegionTool.cpp" -o "$WORK/regionoverlay_test" $LF

rc=0
QT_QPA_PLATFORM=offscreen "$WORK/shortcut_dupkey_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/keylist_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/paramslider_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/uitext_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/regionwatch_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/regionoverlay_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/startupsplash_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/monitormatch_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/canvasfit_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/previewaspect_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/timelineoverview_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/projectroundtrip_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/zoomkeyframes_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/component_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/scriptcomponent_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/componentrender_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/effectcomponent_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/assetcomponent_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/effectarea_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/gutterspill_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/splitseam_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/timelinepan_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/timelineperf_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/singleinstance_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/propkeys_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/pixelbands_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/mask_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/timelineslice_test" || rc=1
exit $rc
