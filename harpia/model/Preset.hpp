#pragma once

#include <map>
#include <string>
#include <vector>

namespace harpia {

// Container the recording is written as. MP4/MKV/MOV/AVI go through OBS's
// `ffmpeg_muxer` (container chosen by file extension); GIF goes through
// `ffmpeg_output` (see EncoderFactory).
enum class RecordingFormat {
	MP4,
	MKV,
	MOV,
	AVI,
	GIF,
};

// Video codec family. The concrete encoder id is resolved by EncoderFactory
// from (codec, gpuCompression), preferring hardware when available.
enum class VideoCodec {
	H264,
	HEVC,
	AV1,
};

// Frame-rate handling. OBS records constant frame rate natively; VFR is a hint
// applied where the backend supports it.
enum class FrameRateMode {
	CFR,
	VFR,
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
	VideoCodec codec = VideoCodec::H264;
	FrameRateMode frameRateMode = FrameRateMode::CFR;

	int fps = 30;  // 24 / 30 / 60 / 120 or any custom value

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

	// Audio capture: system/desktop ("PC") audio, and the set of microphone/
	// input device ids to record. Empty micDeviceIds means no mic.
	bool recordDesktopAudio = false;
	std::vector<std::string> micDeviceIds;
	// Per-source volumes (linear 0..1). Mic volumes are keyed by device id and
	// kept even for currently-disabled devices so re-enabling restores them.
	double desktopVolume = 1.0;
	std::map<std::string, double> micVolumes;

	// Mouse recording (drawn onto the desktop so the screen capture records it).
	bool showMouseCursor = true;                    // record the OS cursor
	bool showMouseArea = false;                      // highlight around the cursor
	std::string mouseHighlightColor = "#ffd54a";    // highlight color (hex)
	int mouseHighlightSize = 60;                     // highlight diameter (px)
	bool recordMouseClicks = false;                  // click ripple animations
	std::string leftClickColor = "#4a90e2";
	std::string rightClickColor = "#e2534a";

	// Webcam: recorded as a SEPARATE synchronized video file (never composited),
	// at its own resolution/fps.
	bool webcamEnabled = false;
	std::string webcamDeviceId;                      // camera device id ("" = default/first)
	int webcamWidth = 1280;
	int webcamHeight = 720;
	int webcamFps = 30;
	bool webcamUseCustomFolder = false;              // else same folder as the screen recording
	std::string webcamFolder;

	// Token-based filename template, e.g.
	// "Tutorial_{Year}-{Month}-{Day}_{Hour}-{Minute}-{Second}".
	// Expanded by FileNameTemplate at record time.
	std::string filenameTemplate = "Recording_{Year}-{Month}-{Day}_{Hour}-{Minute}-{Second}";

	// Auto-pause after this many seconds with no keyboard/mouse input.
	// 0 disables the idle auto-pause behavior for this preset.
	int idleTimeoutSeconds = 0;

	// Region recording only: auto-PAUSE once the pointer has been outside the
	// recording region this many seconds, resuming when it comes back. -1 is
	// off; 0 means the moment it leaves, so unlike idleTimeoutSeconds above, 0
	// cannot double as the disabled value.
	int regionLeavePauseSeconds = -1;

	// Auto-pause recording whenever the target application (the app that was in
	// the foreground when recording started) loses focus, and resume when it
	// regains focus. Child windows/dialogs of that app count as still focused.
	bool pauseOnFocusLoss = false;

	// Next value for the {Counter} filename token; incremented after each
	// recording that uses it so files number 0001, 0002, … across sessions.
	int recordingCounter = 1;

	// Seconds to count down (on-screen) before recording actually starts.
	// 0 = disabled (start immediately). 1..10 supported.
	int countdownSeconds = 0;

	// Minimum recorded length (seconds, content excluding paused spans). If a
	// finished recording is shorter than this, the user is asked whether to
	// discard it. 0 = disabled (always keep).
	int minRecordingSeconds = 0;

	// Optional Google Drive (or any) share URL associated with this preset. When
	// set, the main window shows a clickable "Google Drive" shortcut in the
	// status bar that opens this link — a quick way to jump to the folder where
	// recordings from this preset are meant to be shared. Empty = hidden.
	std::string googleDriveLink;

	// Show a colored border around the recorded monitor while recording (Full
	// Screen capture only). The border is excluded from the recording itself.
	bool showScreenBorder = false;
	std::string screenBorderColor = "#e5484d"; // default red
	int screenBorderThickness = 4;             // px, 1..10

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

const char *codecToString(VideoCodec codec);
VideoCodec codecFromString(const std::string &value, VideoCodec fallback = VideoCodec::H264);

const char *frameRateModeToString(FrameRateMode mode);
FrameRateMode frameRateModeFromString(const std::string &value, FrameRateMode fallback = FrameRateMode::CFR);

} // namespace harpia
