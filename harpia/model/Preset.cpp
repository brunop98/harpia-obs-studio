#include "Preset.hpp"

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

const char *resolutionModeToString(ResolutionMode mode)
{
	switch (mode) {
	case ResolutionMode::Native:
		return "native";
	case ResolutionMode::Scaled:
		return "scaled";
	case ResolutionMode::Custom:
		return "custom";
	}
	return "native";
}

ResolutionMode resolutionModeFromString(const std::string &value, ResolutionMode fallback)
{
	if (value == "native")
		return ResolutionMode::Native;
	if (value == "scaled")
		return ResolutionMode::Scaled;
	if (value == "custom")
		return ResolutionMode::Custom;
	return fallback;
}

std::string Preset::extension() const
{
	return formatExtension(format);
}

Preset Preset::makeDefault(const std::string &outputFolder)
{
	Preset p;
	p.id = "default";
	p.name = "Default";
	p.format = RecordingFormat::MP4;
	p.fps = 30;
	p.resolutionMode = ResolutionMode::Native;
	p.outputFolder = outputFolder;
	p.gpuCompression = false;
	p.audioBitrateKbps = 160;
	p.filenameTemplate = "Recording_{Year}-{Month}-{Day}_{Hour}-{Minute}-{Second}";
	p.idleTimeoutSeconds = 0;
	return p;
}

} // namespace harpia
