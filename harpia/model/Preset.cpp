#include "Preset.hpp"

#include "core/ModeCapabilities.hpp"

namespace harpia {

const char *formatToString(RecordingFormat format)
{
	switch (format) {
	case RecordingFormat::MP4:
		return "mp4";
	case RecordingFormat::MKV:
		return "mkv";
	case RecordingFormat::MOV:
		return "mov";
	case RecordingFormat::AVI:
		return "avi";
	case RecordingFormat::GIF:
		return "gif";
	}
	return "mp4";
}

RecordingFormat formatFromString(const std::string &value, RecordingFormat fallback)
{
	if (value == "mp4")
		return RecordingFormat::MP4;
	if (value == "mkv")
		return RecordingFormat::MKV;
	if (value == "mov")
		return RecordingFormat::MOV;
	if (value == "avi")
		return RecordingFormat::AVI;
	if (value == "gif")
		return RecordingFormat::GIF;
	return fallback;
}

const char *codecToString(VideoCodec codec)
{
	switch (codec) {
	case VideoCodec::H264:
		return "h264";
	case VideoCodec::HEVC:
		return "hevc";
	case VideoCodec::AV1:
		return "av1";
	}
	return "h264";
}

VideoCodec codecFromString(const std::string &value, VideoCodec fallback)
{
	if (value == "h264")
		return VideoCodec::H264;
	if (value == "hevc")
		return VideoCodec::HEVC;
	if (value == "av1")
		return VideoCodec::AV1;
	return fallback;
}

const char *frameRateModeToString(FrameRateMode mode)
{
	return mode == FrameRateMode::VFR ? "vfr" : "cfr";
}

FrameRateMode frameRateModeFromString(const std::string &value, FrameRateMode fallback)
{
	if (value == "vfr")
		return FrameRateMode::VFR;
	if (value == "cfr")
		return FrameRateMode::CFR;
	return fallback;
}

const char *formatExtension(RecordingFormat format)
{
	// For the formats we currently support the container name and the file
	// extension coincide.
	return formatToString(format);
}

std::string Preset::extension() const
{
	// Audio Only overrides the video container entirely: there is no picture,
	// so the preset's MP4/MKV/GIF choice describes nothing. Handled here rather
	// than at each call site so the temp file, the final file and the library
	// all agree without three separate checks.
	if (recordModeFromInt(captureMode) == RecordMode::AudioOnly)
		return audioOnlyExtension();
	return formatExtension(format);
}

Preset Preset::makeDefault(const std::string &outputFolder)
{
	Preset p;
	p.id = "default";
	p.name = "Default";
	p.format = RecordingFormat::MP4;
	p.fps = 30;
	p.outputFolder = outputFolder;
	p.gpuCompression = false;
	p.audioBitrateKbps = 160;
	p.filenameTemplate = "Recording_{Year}-{Month}-{Day}_{Hour}-{Minute}-{Second}";
	p.idleTimeoutSeconds = 0;
	p.regionLeavePauseSeconds = -1;
	p.countdownSeconds = 0; // countdown is opt-in — never on by default
	return p;
}

} // namespace harpia
