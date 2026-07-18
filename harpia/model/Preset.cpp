#include "Preset.hpp"

namespace harpia {

const char *formatToString(RecordingFormat format)
{
	switch (format) {
	case RecordingFormat::MP4:
		return "mp4";
	case RecordingFormat::MKV:
		return "mkv";
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
	if (value == "gif")
		return RecordingFormat::GIF;
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
