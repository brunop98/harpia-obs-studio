#!/bin/bash
# Build and run the checks that need real media: A/V sync, all four export
# paths (and the colour, placement and detail of their pixels), the preview
# decoder, the editing proxies and Multi-Cut playback.
#
#   harpia/tests/run_avsync.sh [workdir]
#
# Generates its own test media, links the real ClipExporter / TimelineAudio /
# TimelineCompositor, exports a timeline and inspects the result with ffmpeg.
# Needs ffmpeg on PATH and the same Qt6 + libav packages the app builds against;
# nothing from libobs, so it runs anywhere the editor code compiles.
#
# Deliberately NOT wired into CMake: the project has no test target and adding
# one is a decision about how this project wants to run tests, not a side effect
# of writing a test.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
H="$ROOT/harpia"
WORK="${1:-$(mktemp -d)}"
mkdir -p "$WORK"
echo "work dir: $WORK"

# --- the sources ------------------------------------------------------------
# Each is a solid colour AND a distinct sine tone. The colour identifies a clip
# in the video stream, the frequency identifies it in the audio stream, and the
# two disagreeing at the same timestamp is exactly what "out of sync" means.
gen() { # name colour hz
	[ -f "$WORK/av_$1.mp4" ] && return 0
	ffmpeg -y -loglevel error \
		-f lavfi -i "color=c=$2:s=320x180:r=30:d=4" \
		-f lavfi -i "sine=frequency=$3:sample_rate=48000:duration=4" \
		-c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$WORK/av_$1.mp4"
}
gen red 0xC00000 300
gen green 0x00A000 700
gen blue 0x0000C0 1500

# --- build ------------------------------------------------------------------
MOC="$(command -v moc || ls /usr/lib/qt6/libexec/moc /usr/lib/x86_64-linux-gnu/qt6/libexec/moc 2>/dev/null | head -1)"
PKGS="Qt6Widgets Qt6Gui Qt6Core Qt6OpenGL libavcodec libavformat libavutil libswscale libswresample libavfilter"
CF="$(pkg-config --cflags $PKGS)"
LF="$(pkg-config --libs $PKGS)"

"$MOC" -I"$H" "$H/editor/ClipExporter.hpp" -o "$WORK/moc_ClipExporter.cpp"
"$MOC" -I"$H" "$H/editor/BatchExport.hpp" -o "$WORK/moc_BatchExport.cpp"

build() { # source-file output-name
	g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=0 -I"$H" -I"$ROOT" $CF \
		"$1" \
		"$H/editor/ClipExporter.cpp" "$H/editor/BatchExport.cpp" "$H/editor/TimelineAudio.cpp" \
		"$H/editor/VoiceoverMixer.cpp" "$H/editor/AudioRetimer.cpp" \
		"$H/editor/GifEncoder.cpp" "$H/editor/FrameSeeker.cpp" \
		"$H/editor/StillImage.cpp" \
		"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
		"$H/editor/component/ComponentStack.cpp" \
		"$H/editor/component/BuiltinComponents.cpp" \
		"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Spotlight.cpp" \
		"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Transitions.cpp" \
		"$H/editor/shader/SpotlightGl.cpp" \
		"$H/editor/script/TransformScript.cpp" "$H/editor/shader/ShaderRenderer.cpp" \
		"$H/editor/component/ShaderComponent.cpp" \
		"$WORK/moc_ClipExporter.cpp" "$WORK/moc_BatchExport.cpp" \
		-o "$WORK/$2" $LF
}
build "$HERE/avsync_test.cpp" avsync_test
build "$HERE/exportpaths_test.cpp" exportpaths_test
build "$HERE/exportcolor_test.cpp" exportcolor_test
build "$HERE/batchexport_test.cpp" batchexport_test

# The preview decoder. Separate build: it needs its own moc and none of the
# exporter, and its media is a long-GOP 1080p file it generates for itself --
# the short clips above would decode too fast to measure anything.
"$MOC" -I"$H" "$H/editor/PreviewDecoder.hpp" -o "$WORK/moc_PreviewDecoder.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/previewdecoder_test.cpp" "$H/editor/PreviewDecoder.cpp" \
	"$H/editor/FrameSeeker.cpp" "$WORK/moc_PreviewDecoder.cpp" \
	-o "$WORK/previewdecoder_test" $LF

# Editing proxies. Links ShareExporter (the transcode a proxy build is) and the
# preview decoder it hands the result to.
"$MOC" -I"$H" "$H/editor/ProxyMedia.hpp" -o "$WORK/moc_ProxyMedia.cpp"
"$MOC" -I"$H" "$H/core/ShareExporter.hpp" -o "$WORK/moc_ShareExporter.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/proxymedia_test.cpp" "$H/editor/ProxyMedia.cpp" "$H/editor/PreviewDecoder.cpp" \
	"$H/editor/FrameSeeker.cpp" "$H/core/ShareExporter.cpp" \
	"$WORK/moc_ProxyMedia.cpp" "$WORK/moc_ShareExporter.cpp" "$WORK/moc_PreviewDecoder.cpp" \
	-o "$WORK/proxymedia_test" $LF

# Multi-Cut output playback: a cut must stop at its end handle. Links the real
# TrackEditor (its segment/output mapping) and a real decoder.
"$MOC" -I"$H" "$H/editor/TrackEditor.hpp" -o "$WORK/moc_TrackEditor.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/multicutplay_test.cpp" "$H/editor/TrackEditor.cpp" "$H/editor/FrameSeeker.cpp" \
	"$H/ui/UiIcons.cpp" "$H/ui/UiText.cpp" "$WORK/moc_TrackEditor.cpp" \
	-o "$WORK/multicutplay_test" $LF

# Save Project / Open Project through the REAL editor window: the whole editor
# (everything but the recorder and libobs) is built into an archive once, and
# a project saved from one editor is opened in a fresh one and compared.
[ -f "$WORK/still.png" ] || ffmpeg -y -loglevel error -f lavfi -i "testsrc=s=400x300:d=1" -frames:v 1 "$WORK/still.png"
[ -f "$WORK/tone.wav" ] || ffmpeg -y -loglevel error -f lavfi -i "sine=frequency=800:sample_rate=48000:duration=2" "$WORK/tone.wav"
WPKGS="Qt6Widgets Qt6Gui Qt6Core Qt6OpenGL Qt6Network Qt6Multimedia Qt6Test libavcodec libavformat libavutil libswscale libswresample libavfilter"
WCF="$(pkg-config --cflags $WPKGS)"
WLF="$(pkg-config --libs $WPKGS)"
QJS="$H/third_party/quickjs"
QJSLIB="${TMPDIR:-/tmp}/harpia-qjs-units"
mkdir -p "$QJSLIB"
if [ ! -f "$QJSLIB/libqjs.a" ]; then # the same one-time build run_units.sh does
	for f in dtoa libregexp libunicode quickjs; do
		gcc -std=c11 -O1 -fPIC -D_GNU_SOURCE -I"$QJS" -c "$QJS/$f.c" -o "$QJSLIB/$f.o" &
	done
	wait
	ar rcs "$QJSLIB/libqjs.a" "$QJSLIB"/*.o
fi
mkdir -p "$WORK/win/obj" "$WORK/win/moc"
( cd "$H"
  WSRCS=$(ls editor/*.cpp editor/*/*.cpp ui/*.cpp library/*.cpp core/ShareExporter.cpp tests/support/logger_stub.cpp | grep -v -E "ui/MainWindow.cpp|ui/WebcamPreview.cpp")
  for h in $(grep -l Q_OBJECT editor/*.hpp editor/*/*.hpp ui/*.hpp library/*.hpp core/ShareExporter.hpp | grep -v -E "ui/MainWindow.hpp|ui/WebcamPreview.hpp"); do
	"$MOC" -I"$H" "$h" -o "$WORK/win/moc/moc_$(echo "$h" | tr '/' '_' | sed 's/.hpp$//').cpp"
  done
  printf "%s\n" $WSRCS "$WORK"/win/moc/*.cpp | xargs -P "$(nproc)" -I{} sh -c \
	'g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$0" -I"$0/third_party/quickjs" -I"$0/.." $1 -c "{}" -o "$2/win/obj/$(echo "{}" | tr "/" "_").o"' \
	"$H" "$WCF" "$WORK" )
rm -f "$WORK/win/libeditor.a"
ar rcs "$WORK/win/libeditor.a" "$WORK"/win/obj/*.o
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/projectwindow_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/projectwindow_test" $WLF
# Save -> Open -> Save gives the same file: sources, trim/speed/crop, cuts,
# voiceover takes and mix, editing mode and active source, compared key by key.
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/projectstate_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/projectstate_test" $WLF
# Undo through the real Ctrl+Z / Ctrl+Y: selection steps, a selection box as
# one step, a group drag, Force ripple, and an edit through clipsChanged.
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/undohistory_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/undohistory_test" $WLF
# Saved variation lists: the file, and picking one in the real window (undoable).
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/variationpresets_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/variationpresets_test" $WLF
# The two kinds of text: Add > Random text, its boxes (+, multi-line, x),
# quick tags into the box being typed in, and a plain Text showing none of it.
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/randomtext_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/randomtext_test" $WLF
# Exporting a Random text's versions the way the user does: Export... ->
# Variations tab -> Export, through the real dialog, batch runner and message.
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/variationexport_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/variationexport_test" $WLF
# The editor's layout: the Inspector fits its narrowest width, the Clip tab's
# order, folding sections, the one wrapping toolbar, the divider per mode.
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/editorlayout_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/editorlayout_test" $WLF
# Curved motion paths in the real window: Curve/Straight path, the curve and
# handles on the preview, a real handle drag (mirrored, Alt to break), undo.
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/curvepathwindow_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/curvepathwindow_test" $WLF
# The keyframe list: drag a row to reorder the framings (times stay), the
# right-click menu (Move to first/last, Reset value, Delete), and undo.
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/keylistwindow_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/keylistwindow_test" $WLF
# The Clip Library's Delete key from anywhere in the window, to the recycle bin.
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/cliplibrarydelete_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/cliplibrarydelete_test" $WLF

rc=0
QT_QPA_PLATFORM=offscreen "$WORK/avsync_test" "$WORK" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/exportpaths_test" "$WORK" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/exportcolor_test" "$WORK" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/batchexport_test" "$WORK" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/previewdecoder_test" "$WORK" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/proxymedia_test" "$WORK" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/multicutplay_test" "$WORK" || rc=1
# Settings go to a scratch home, so the test never touches real ones.
mkdir -p "$WORK/home"
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/projectwindow_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/undohistory_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/projectstate_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/variationpresets_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/randomtext_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/variationexport_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/editorlayout_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/curvepathwindow_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/keylistwindow_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/cliplibrarydelete_test" "$WORK" || rc=1
exit $rc
