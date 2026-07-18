#pragma once

#include <string>

namespace harpia {

// Container / codec family the recording is written as. Kept as a small,
// explicit enum so new formats slot in by extending this + EncoderFactory and
// the format<->extension helpers below. MP4 and MKV go through OBS's
// `ffmpeg_muxer`; GIF goes through `ffmpeg_output` (see EncoderFactory).
enum class RecordingFormat {
	MP4,
	MKV,
	GIF,
};

// How the output resolution relates to the captured display.
enum class ResolutionMode {
	Native,  // match the captured display exactly
	Scaled,  // downscale to width/height below, preserving capture as base
	Custom,  // use width/height below verbatim
};

// A single, self-contained recording configuration. This is the unit the UI
// creates/edits/deletes and PresetStore persists. Everything the recording
// pipeline needs is derived from a Preset, so adding a knob here is the single
// place a new recording option is introduced.
struct Preset {
	std::string id;    // stable identifier (used as filename/lookup key)
	std::string name;  // human-facing label

	RecordingFormat format = RecordingFormat::MP4;

	int fps = 30;  // 5 / 10 / 30 / 60 or any custom value

	ResolutionMode resolutionMode = ResolutionMode::Native;
	int width = 0;   // used when resolutionMode != Native
	int height = 0;  // used when resolutionMode != Native

	std::string outputFolder;  // where recordings are written

	int monitorIndex = 0;  // which display to capture (0 = primary/first)

	// When true, prefer a hardware ("GPU") encoder (NVENC/AMF/QSV) so
	// compression happens during capture with no separate encode pass.
	// Falls back to x264 if no hardware encoder is available.
	bool gpuCompression = false;

	int videoBitrateKbps = 0;  // 0 => EncoderFactory picks a sane default
	int audioBitrateKbps = 160;

	// Token-based filename template, e.g.
	// "Tutorial_{Year}-{Month}-{Day}_{Hour}-{Minute}-{Second}".
	// Expanded by FileNameTemplate at record time.
	std::string filenameTemplate = "Recording_{Year}-{Month}-{Day}_{Hour}-{Minute}-{Second}";

	// Auto-pause after this many seconds with no keyboard/mouse input.
	// 0 disables the idle auto-pause behavior for this preset.
	int idleTimeoutSeconds = 0;

	// File extension (without the dot) for the current format.
	std::string extension() const;

	// A ready-to-use default preset (MP4, 30fps, native res).
	static Preset makeDefault(const std::string &outputFolder);
};

// Format<->string helpers used by persistence and the UI.
const char *formatToString(RecordingFormat format);
RecordingFormat formatFromString(const std::string &value, RecordingFormat fallback = RecordingFormat::MP4);
const char *formatExtension(RecordingFormat format);

const char *resolutionModeToString(ResolutionMode mode);
ResolutionMode resolutionModeFromString(const std::string &value, ResolutionMode fallback = ResolutionMode::Native);

} // namespace harpia
