#include "EncoderFactory.hpp"

#include <obs.h>

#include <cstring>

namespace harpia {

bool EncoderFactory::encoderAvailable(const char *id)
{
	const char *encId = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &encId); i++) {
		if (encId && std::strcmp(encId, id) == 0)
			return true;
	}
	return false;
}

std::string EncoderFactory::videoEncoderId(const Preset &preset)
{
	if (preset.gpuCompression) {
		// Prefer hardware H.264 texture encoders, in the same priority the
		// OBS simple-output path uses. Each does GPU-side encoding during
		// capture, so there is no separate encode pass afterwards.
		if (encoderAvailable("obs_nvenc_h264_tex"))
			return "obs_nvenc_h264_tex";
		if (encoderAvailable("ffmpeg_nvenc"))
			return "ffmpeg_nvenc";
		if (encoderAvailable("h264_texture_amf"))
			return "h264_texture_amf";
		if (encoderAvailable("obs_qsv11_v2"))
			return "obs_qsv11_v2";
		if (encoderAvailable("ffmpeg_vaapi"))
			return "ffmpeg_vaapi";
		// No hardware encoder present — fall through to software.
	}
	return "obs_x264";
}

std::string EncoderFactory::audioEncoderId(const Preset &preset)
{
	(void)preset;
	return "ffmpeg_aac";
}

bool EncoderFactory::usesFfmpegOutput(const Preset &preset)
{
	// GIF (and other exotic containers) need the generic ffmpeg_output, which
	// resolves the muxer from format_name. MP4/MKV use the robust ffmpeg_muxer.
	return preset.format == RecordingFormat::GIF;
}

std::string EncoderFactory::outputId(const Preset &preset)
{
	return usesFfmpegOutput(preset) ? "ffmpeg_output" : "ffmpeg_muxer";
}

} // namespace harpia
