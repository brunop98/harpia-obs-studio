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

namespace {

// Encoder id candidates per codec, hardware first then software, in preference
// order. The first that is registered wins.
const char *const *hardwareIds(VideoCodec codec, size_t &count)
{
	static const char *h264[] = {"obs_nvenc_h264_tex", "ffmpeg_nvenc", "h264_texture_amf",
				     "obs_qsv11_v2", "ffmpeg_vaapi"};
	static const char *hevc[] = {"obs_nvenc_hevc_tex", "ffmpeg_hevc_nvenc", "h265_texture_amf",
				     "obs_qsv11_hevc", "hevc_ffmpeg_vaapi"};
	static const char *av1[] = {"obs_nvenc_av1_tex", "av1_texture_amf", "obs_qsv11_av1",
				    "av1_ffmpeg_vaapi"};
	switch (codec) {
	case VideoCodec::H264:
		count = sizeof(h264) / sizeof(*h264);
		return h264;
	case VideoCodec::HEVC:
		count = sizeof(hevc) / sizeof(*hevc);
		return hevc;
	case VideoCodec::AV1:
		count = sizeof(av1) / sizeof(*av1);
		return av1;
	}
	count = 0;
	return nullptr;
}

const char *const *softwareIds(VideoCodec codec, size_t &count)
{
	static const char *h264[] = {"obs_x264"};
	static const char *av1[] = {"ffmpeg_svt_av1", "ffmpeg_aom_av1"};
	switch (codec) {
	case VideoCodec::H264:
		count = sizeof(h264) / sizeof(*h264);
		return h264;
	case VideoCodec::HEVC:
		count = 0; // no software HEVC encoder in this build
		return nullptr;
	case VideoCodec::AV1:
		count = sizeof(av1) / sizeof(*av1);
		return av1;
	}
	count = 0;
	return nullptr;
}

} // namespace

std::string EncoderFactory::videoEncoderId(const Preset &preset)
{
	size_t n = 0;

	if (preset.gpuCompression) {
		const char *const *hw = hardwareIds(preset.codec, n);
		for (size_t i = 0; i < n; i++) {
			if (encoderAvailable(hw[i]))
				return hw[i];
		}
	}
	const char *const *sw = softwareIds(preset.codec, n);
	for (size_t i = 0; i < n; i++) {
		if (encoderAvailable(sw[i]))
			return sw[i];
	}
	// If GPU wasn't requested but only a hardware encoder exists (e.g. HEVC),
	// use it rather than failing.
	const char *const *hw = hardwareIds(preset.codec, n);
	for (size_t i = 0; i < n; i++) {
		if (encoderAvailable(hw[i]))
			return hw[i];
	}
	return {};
}

bool EncoderFactory::codecAvailable(VideoCodec codec, bool gpuOnly)
{
	size_t n = 0;
	const char *const *hw = hardwareIds(codec, n);
	for (size_t i = 0; i < n; i++) {
		if (encoderAvailable(hw[i]))
			return true;
	}
	if (gpuOnly)
		return false;
	const char *const *sw = softwareIds(codec, n);
	for (size_t i = 0; i < n; i++) {
		if (encoderAvailable(sw[i]))
			return true;
	}
	return false;
}

std::vector<VideoCodec> EncoderFactory::availableCodecs(bool gpuOnly)
{
	std::vector<VideoCodec> out;
	for (VideoCodec c : {VideoCodec::H264, VideoCodec::HEVC, VideoCodec::AV1}) {
		if (codecAvailable(c, gpuOnly))
			out.push_back(c);
	}
	// Always offer H.264 as a baseline even if the probe came up empty
	// (obs_x264 is always built), so the editor is never left with no codec.
	if (out.empty())
		out.push_back(VideoCodec::H264);
	return out;
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

void EncoderFactory::applyRecordingQuality(obs_data_t *settings, const std::string &encoderId, int bitrateKbps)
{
	// A keyframe every 2 s keeps files seekable without bloating them.
	obs_data_set_int(settings, "keyint_sec", 2);

	if (bitrateKbps > 0) {
		// The user picked an explicit bitrate — honor it as CBR.
		obs_data_set_string(settings, "rate_control", "CBR");
		obs_data_set_int(settings, "bitrate", bitrateKbps);
		return;
	}

	// "Auto" = constant-quality encoding (what OBS Simple mode uses for
	// recordings): better quality per megabyte than CBR, and the file size
	// scales with how much actually changes on screen. Each encoder family
	// names its quality knob differently.
	const bool av1 = encoderId.find("av1") != std::string::npos;
	const int q = av1 ? 30 : 23; // AV1's qp scale runs higher than H.264/HEVC
	if (encoderId == "obs_x264") {
		obs_data_set_string(settings, "rate_control", "CRF");
		obs_data_set_int(settings, "crf", q);
	} else if (encoderId.find("vaapi") != std::string::npos) {
		obs_data_set_string(settings, "rate_control", "CQP");
		obs_data_set_int(settings, "qp", q);
	} else {
		// NVENC / QSV / AMF / ffmpeg AV1 all take CQP via "cqp".
		obs_data_set_string(settings, "rate_control", "CQP");
		obs_data_set_int(settings, "cqp", q);
	}
}

} // namespace harpia
