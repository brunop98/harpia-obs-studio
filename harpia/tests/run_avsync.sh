#!/bin/bash
# Build and run the checks that need real media: A/V sync, all four export
# paths, and the preview decoder.
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

build() { # source-file output-name
	g++ -std=c++17 -O1 -fPIC -DHARPIA_HAVE_QJS=0 -I"$H" -I"$ROOT" $CF \
		"$1" \
		"$H/editor/ClipExporter.cpp" "$H/editor/TimelineAudio.cpp" \
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
		"$WORK/moc_ClipExporter.cpp" \
		-o "$WORK/$2" $LF
}
build "$HERE/avsync_test.cpp" avsync_test
build "$HERE/exportpaths_test.cpp" exportpaths_test

# The preview decoder. Separate build: it needs its own moc and none of the
# exporter, and its media is a long-GOP 1080p file it generates for itself --
# the short clips above would decode too fast to measure anything.
"$MOC" -I"$H" "$H/editor/PreviewDecoder.hpp" -o "$WORK/moc_PreviewDecoder.cpp"
g++ -std=c++17 -O1 -fPIC -I"$H" -I"$ROOT" $CF \
	"$HERE/previewdecoder_test.cpp" "$H/editor/PreviewDecoder.cpp" \
	"$H/editor/FrameSeeker.cpp" "$WORK/moc_PreviewDecoder.cpp" \
	-o "$WORK/previewdecoder_test" $LF

rc=0
QT_QPA_PLATFORM=offscreen "$WORK/avsync_test" "$WORK" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/exportpaths_test" "$WORK" || rc=1
QT_QPA_PLATFORM=offscreen "$WORK/previewdecoder_test" "$WORK" || rc=1
exit $rc
