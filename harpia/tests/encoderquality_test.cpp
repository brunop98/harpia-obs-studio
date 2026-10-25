#include "core/EncoderFactory.hpp"
#include <obs.h>
#include <QGuiApplication>
#include <cstdio>
#include <cstring>

namespace {
int failed = 0;
void check(bool value, const char *text) { std::printf("  %s %s\n", value ? "PASS" : "FAIL", text); failed += !value; }
const char *name(void *) { return "Encoder selection test"; }
void *create(obs_data_t *, obs_encoder_t *) { return nullptr; }
void destroy(void *) {}
bool encode(void *, encoder_frame *, encoder_packet *, bool *) { return false; }
void registerEncoder(const char *id, const char *codec)
{
	obs_encoder_info info{};
	info.id = id; info.type = OBS_ENCODER_VIDEO; info.codec = codec;
	info.get_name = name; info.create = create; info.destroy = destroy; info.encode = encode;
	obs_register_encoder(&info);
}
}
int main(int argc, char **argv)
{
	QGuiApplication app(argc, argv);
	if (!obs_startup("en-US", nullptr, nullptr)) return 2;
	using harpia::EncoderFactory;
	harpia::Preset preset;
	preset.codec = harpia::VideoCodec::H264;
	preset.gpuCompression = true;
	registerEncoder("obs_x264", "h264");
	check(EncoderFactory::videoEncoderId(preset) == "obs_x264", "hardware preference retains software fallback when Apple hardware is unavailable");
	registerEncoder("ffmpeg_h264_videotoolbox", "h264");
	registerEncoder("ffmpeg_hevc_videotoolbox", "hevc");
	check(EncoderFactory::videoEncoderId(preset) == "ffmpeg_h264_videotoolbox", "registered Apple H.264 hardware is selected for GPU recording");
	preset.gpuCompression = false;
	check(EncoderFactory::videoEncoderId(preset) == "obs_x264", "CPU recording still selects x264");
	preset.codec = harpia::VideoCodec::HEVC;
	check(EncoderFactory::videoEncoderId(preset) == "ffmpeg_hevc_videotoolbox", "HEVC can use available Apple hardware without a software HEVC encoder");
	obs_data_t *settings = obs_data_create();
	EncoderFactory::applyRecordingQuality(settings, "ffmpeg_h264_videotoolbox", 0);
	check(std::strcmp(obs_data_get_string(settings, "rate_control"), "QUALITY") == 0 && obs_data_get_int(settings, "quality") == 65,
		"Apple Auto requests its own quality scale rather than NVENC CQP");
	EncoderFactory::applyRecordingQuality(settings, "ffmpeg_hevc_videotoolbox", 5000);
	check(std::strcmp(obs_data_get_string(settings, "rate_control"), "ABR") == 0 && obs_data_get_int(settings, "bitrate") == 5000,
		"explicit Apple bitrate preserves the requested target");
	EncoderFactory::applyRecordingQuality(settings, "obs_x264", 0);
	check(std::strcmp(obs_data_get_string(settings, "rate_control"), "CRF") == 0 && obs_data_get_int(settings, "crf") == 23,
		"existing x264 automatic quality stays unchanged");
	EncoderFactory::applyRecordingQuality(settings, "obs_nvenc_h264_tex", 20000);
	check(std::strcmp(obs_data_get_string(settings, "rate_control"), "CBR") == 0 && obs_data_get_int(settings, "bitrate") == 20000,
		"existing explicit NVENC bitrate stays unchanged");
	obs_data_release(settings);
	obs_shutdown();
	return failed ? 1 : 0;
}
