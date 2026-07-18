#pragma once

#include "model/Preset.hpp"

#include <string>
#include <vector>

namespace harpia {

// Resolves a Preset's format + GPU-compression choice into concrete libobs
// encoder ids and output configuration. This is the single place that knows the
// plugin-specific encoder id strings, so adding a codec/format touches only
// here (and Preset).
class EncoderFactory {
public:
	// The video encoder id to use for `preset` (codec + gpuCompression). When
	// GPU is on, prefers hardware (NVENC → AMF → QSV → VAAPI) and falls back to
	// the best software encoder for the codec. Empty if the codec has no
	// available encoder at all.
	static std::string videoEncoderId(const Preset &preset);

	// Codecs that currently have at least one registered encoder (optionally
	// restricted to hardware when `gpuOnly`). Drives the editor's codec list so
	// unsupported codecs are hidden.
	static std::vector<VideoCodec> availableCodecs(bool gpuOnly);

	// Whether `codec` has any encoder available (hardware or software).
	static bool codecAvailable(VideoCodec codec, bool gpuOnly);

	// The audio encoder id (AAC for MP4/MKV).
	static std::string audioEncoderId(const Preset &preset);

	// The libobs output plugin id for the container:
	//   MP4/MKV -> "ffmpeg_muxer" (container chosen by file extension)
	//   GIF     -> "ffmpeg_output" (format_name = "gif")
	static std::string outputId(const Preset &preset);

	// True if this format goes through the generic ffmpeg_output path (which
	// uses "url"/"format_name"/media wiring) rather than ffmpeg_muxer.
	static bool usesFfmpegOutput(const Preset &preset);

	// Whether the given encoder id is currently registered/available.
	static bool encoderAvailable(const char *id);
};

} // namespace harpia
