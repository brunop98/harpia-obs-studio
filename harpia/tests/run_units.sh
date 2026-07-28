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
CF="$(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core Qt6Test Qt6OpenGL)"
LF="$(pkg-config --libs Qt6Widgets Qt6Gui Qt6Core Qt6Test Qt6OpenGL)"

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

rc=0
QT_QPA_PLATFORM=offscreen "$WORK/shortcut_dupkey_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/keylist_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/paramslider_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/uitext_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/regionwatch_test" || rc=1
exit $rc
