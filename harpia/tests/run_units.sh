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
CF="$(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core Qt6Test)"
LF="$(pkg-config --libs Qt6Widgets Qt6Gui Qt6Core Qt6Test)"

# Two commands must never end up sharing a key: Qt fires neither.
"$MOC" -I"$H" "$H/editor/ShortcutRegistry.hpp" -o "$WORK/moc_ShortcutRegistry.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/shortcut_dupkey_test.cpp" "$H/editor/ShortcutRegistry.cpp" \
	"$WORK/moc_ShortcutRegistry.cpp" -o "$WORK/shortcut_dupkey_test" $LF

# The keyframe list, and the add/remove rules under it.
"$MOC" -I"$H" "$H/editor/KeyList.hpp" -o "$WORK/moc_KeyList.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/keylist_test.cpp" "$H/editor/KeyList.cpp" "$H/editor/timeline/EffectClip.cpp" \
	"$H/editor/timeline/Spotlight.cpp" "$H/ui/UiIcons.cpp" \
	"$WORK/moc_KeyList.cpp" -o "$WORK/keylist_test" $LF

rc=0
QT_QPA_PLATFORM=offscreen "$WORK/shortcut_dupkey_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/keylist_test" || rc=1
exit $rc
