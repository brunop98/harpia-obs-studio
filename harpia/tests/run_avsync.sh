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
# An iPhone-style HDR clip: 10-bit HEVC, HLG, BT.2020, ten grey bars of HLG
# signal (0 .1 .25 .38 .5 .6 .75 .85 .95 1). Skipped where FFmpeg has no libx265.
if [ ! -f "$WORK/hlg_ramp.mp4" ] && ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx265; then
	ffmpeg -y -loglevel error -f lavfi -i "nullsrc=s=640x64:r=30:d=1" \
		-vf "format=gray16le,geq=lum='if(lt(X,64),0,if(lt(X,128),6554,if(lt(X,192),16384,if(lt(X,256),24903,if(lt(X,320),32768,if(lt(X,384),39321,if(lt(X,448),49151,if(lt(X,512),55705,if(lt(X,576),62258,65535)))))))))',format=yuv420p10le" \
		-c:v libx265 -x265-params "log-level=error:colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc" \
		-color_primaries bt2020 -color_trc arib-std-b67 -colorspace bt2020nc "$WORK/hlg_ramp.mp4" || true
fi
gen green 0x00A000 700
gen blue 0x0000C0 1500
# Ten minutes of sound and nothing else: the long recording a timeline's short
# clips are cut from, for timing the preview mix (timelinemix_test).
[ -f "$WORK/long_tone.m4a" ] || ffmpeg -y -loglevel error \
	-f lavfi -i "sine=frequency=440:sample_rate=48000:duration=600" \
	-f lavfi -i "sine=frequency=660:sample_rate=48000:duration=600" \
	-filter_complex "[0][1]amerge=inputs=2" -c:a aac -b:a 128k "$WORK/long_tone.m4a"

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
		"$H/editor/ClipExporter.cpp" "$H/editor/CoverArt.cpp" "$H/editor/BatchExport.cpp" "$H/editor/TimelineAudio.cpp" \
		"$H/editor/VoiceoverMixer.cpp" "$H/editor/AudioRetimer.cpp" \
		"$H/editor/GifEncoder.cpp" "$H/editor/FrameSeeker.cpp" "$H/editor/HdrToSdr.cpp" \
		"$H/editor/StillImage.cpp" \
		"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
		"$H/editor/component/ComponentStack.cpp" \
		"$H/editor/component/BuiltinComponents.cpp" \
		"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" "$H/editor/timeline/Spotlight.cpp" \
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
# Every frame once: sources that number their frames, read the way an export
# reads them and exported from a two-track 60 fps timeline.
build "$HERE/exportcadence_test.cpp" exportcadence_test
build "$HERE/timelinemix_test.cpp" timelinemix_test
# The export window: what it says is what it asks for -- resolution, frame
# rate, the remembered choices -- and a size estimate that moves the right way.
"$MOC" -I"$H" "$H/editor/ExportOptionsDialog.hpp" -o "$WORK/moc_ExportOptionsDialog.cpp"
g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=0 -I"$H" -I"$ROOT" $CF \
	"$HERE/exportdialog_test.cpp" "$H/editor/ExportOptionsDialog.cpp" "$H/editor/ExportEstimate.cpp" \
	"$H/ui/UiText.cpp" \
	"$H/editor/ClipExporter.cpp" "$H/editor/CoverArt.cpp" "$H/editor/BatchExport.cpp" "$H/editor/TimelineAudio.cpp" \
	"$H/editor/VoiceoverMixer.cpp" "$H/editor/AudioRetimer.cpp" \
	"$H/editor/GifEncoder.cpp" "$H/editor/FrameSeeker.cpp" "$H/editor/HdrToSdr.cpp" \
	"$H/editor/StillImage.cpp" \
	"$H/editor/component/Component.cpp" "$H/editor/component/ComponentRegistry.cpp" \
	"$H/editor/component/ComponentStack.cpp" \
	"$H/editor/component/BuiltinComponents.cpp" \
	"$H/editor/timeline/TimelineCompositor.cpp" "$H/editor/timeline/Emoji.cpp" "$H/editor/timeline/Spotlight.cpp" \
	"$H/editor/timeline/EffectClip.cpp" "$H/editor/timeline/Transitions.cpp" \
	"$H/editor/shader/SpotlightGl.cpp" \
	"$H/editor/script/TransformScript.cpp" "$H/editor/shader/ShaderRenderer.cpp" \
	"$H/editor/component/ShaderComponent.cpp" \
	"$WORK/moc_ClipExporter.cpp" "$WORK/moc_BatchExport.cpp" "$WORK/moc_ExportOptionsDialog.cpp" \
	-o "$WORK/exportdialog_test" $LF
# HDR (HLG/PQ) to SDR: the curve, the colours, and a real 10-bit HLG clip.
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/hdrtosdr_test.cpp" "$H/editor/HdrToSdr.cpp" -o "$WORK/hdrtosdr_test" $LF -lpthread

# The preview decoder. Separate build: it needs its own moc and none of the
# exporter, and its media is a long-GOP 1080p file it generates for itself --
# the short clips above would decode too fast to measure anything.
"$MOC" -I"$H" "$H/editor/PreviewDecoder.hpp" -o "$WORK/moc_PreviewDecoder.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/previewdecoder_test.cpp" "$H/editor/PreviewDecoder.cpp" \
	"$H/editor/FrameSeeker.cpp" "$H/editor/HdrToSdr.cpp" "$WORK/moc_PreviewDecoder.cpp" \
	-o "$WORK/previewdecoder_test" $LF

# Editing proxies. Links ShareExporter (the transcode a proxy build is) and the
# preview decoder it hands the result to.
"$MOC" -I"$H" "$H/editor/ProxyMedia.hpp" -o "$WORK/moc_ProxyMedia.cpp"
"$MOC" -I"$H" "$H/core/ShareExporter.hpp" -o "$WORK/moc_ShareExporter.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/proxymedia_test.cpp" "$H/editor/ProxyMedia.cpp" "$H/editor/PreviewDecoder.cpp" \
	"$H/editor/FrameSeeker.cpp" "$H/editor/HdrToSdr.cpp" "$H/core/ShareExporter.cpp" \
	"$WORK/moc_ProxyMedia.cpp" "$WORK/moc_ShareExporter.cpp" "$WORK/moc_PreviewDecoder.cpp" \
	-o "$WORK/proxymedia_test" $LF

# Multi-Cut output playback: a cut must stop at its end handle. Links the real
# TrackEditor (its segment/output mapping) and a real decoder.
"$MOC" -I"$H" "$H/editor/TrackEditor.hpp" -o "$WORK/moc_TrackEditor.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/multicutplay_test.cpp" "$H/editor/TrackEditor.cpp" "$H/editor/FrameSeeker.cpp" "$H/editor/HdrToSdr.cpp" \
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
  WSRCS=$(ls editor/*.cpp editor/*/*.cpp ui/*.cpp library/*.cpp core/ShareExporter.cpp platform/OsIntegrationStub.cpp tests/support/logger_stub.cpp | grep -v -E "ui/MainWindow.cpp|ui/WebcamPreview.cpp")
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
# Text to speech in the real window, without a network: the narration and its
# captions land linked, are selected together, go with one undo; the menu entry.
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/ttswindow_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/ttswindow_test" $WLF
# Styling a whole caption lane never rewrites what the captions say: the quick
# tags once copied the first caption's words into every one of them.
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/captionbatch_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/captionbatch_test" $WLF
# Closing an editor opened blank, with no video in it: crashed in v0.1.420
# (a null decoder read by the unsaved-changes check).
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/blankeditor_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/blankeditor_test" $WLF
# A Random clip from making one to Export > Variations: the Inspector list,
# + Add clip, the arrows, x, saving it, and one video per clip of its own footage.
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/randomclip_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/randomclip_test" $WLF
# The preview/editing divider moves in Full editing however many lanes there
# are: the lanes scroll under the ruler instead of setting a minimum height.
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/editorsplit_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/editorsplit_test" $WLF
# Slip in its own window: the clip alone in its preview, the strip under a
# fixed window, keys, Play, and OK / Cancel from the clip's real menu.
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/slipwindow_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/slipwindow_test" $WLF
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
# An export finishing in the middle of a progress update (the modal dialog's
# setValue() runs the event loop): the crash in v0.1.406, reproduced exactly.
g++ -std=c++17 -O1 -fPIC -w -DHARPIA_HAVE_QJS=1 -I"$H" -I"$QJS" -I"$ROOT" $WCF \
	"$HERE/exportprogress_test.cpp" \
	-Wl,--start-group "$WORK/win/libeditor.a" "$QJSLIB/libqjs.a" -Wl,--end-group \
	-o "$WORK/exportprogress_test" $WLF
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
QT_QPA_PLATFORM=offscreen "$WORK/exportcadence_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/timelinemix_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/exportdialog_test" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/exportprogress_test" "$WORK" || rc=1
"$WORK/hdrtosdr_test" "$WORK" || rc=1
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
	"$WORK/ttswindow_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/captionbatch_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/blankeditor_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/randomclip_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/slipwindow_test" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/editorsplit_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/editorlayout_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/curvepathwindow_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/keylistwindow_test" "$WORK" || rc=1
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" QT_QPA_PLATFORM=offscreen \
	"$WORK/cliplibrarydelete_test" "$WORK" || rc=1
exit $rc
