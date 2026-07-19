#include "RecordingController.hpp"

#include "EncoderFactory.hpp"

#include <obs.h>

namespace harpia {

namespace {

// A reasonable default recording bitrate (kbps) when a preset selects "Auto"
// (videoBitrateKbps == 0). Scales by resolution, frame rate, and codec so the
// three inputs the user expects all move the number.
int defaultVideoBitrate(const Preset &preset)
{
	if (preset.videoBitrateKbps > 0)
		return preset.videoBitrateKbps;

	long pixels = (long)(preset.width > 0 ? preset.width : 1920) * (preset.height > 0 ? preset.height : 1080);
	if (preset.resolutionMode == ResolutionMode::Native)
		pixels = 1920L * 1080L; // unknown until capture; assume 1080p

	// Resolution baseline (kbps) tuned for ~30 fps H.264.
	double kbps;
	if (pixels >= 3840L * 2160L)
		kbps = 40000.0;
	else if (pixels >= 2560L * 1440L)
		kbps = 20000.0;
	else
		kbps = 12000.0; // 1080p and below

	// Frame rate: higher fps needs proportionally more bits (relative to 30).
	const int fps = preset.fps > 0 ? preset.fps : 30;
	kbps *= 0.6 + 0.4 * (fps / 30.0); // 30->1.0x, 60->1.4x, 120->2.2x

	// Codec efficiency: HEVC/AV1 reach the same quality at a lower bitrate.
	if (preset.codec == VideoCodec::HEVC)
		kbps *= 0.7;
	else if (preset.codec == VideoCodec::AV1)
		kbps *= 0.6;

	// Round to the nearest 500 kbps for tidy values.
	int rounded = (int)((kbps + 250.0) / 500.0) * 500;
	return rounded < 1000 ? 1000 : rounded;
}

} // namespace

RecordingController::~RecordingController()
{
	teardown();
}

void RecordingController::teardown()
{
	if (output_) {
		signal_handler_t *sh = obs_output_get_signal_handler(output_);
		if (sh) {
			signal_handler_disconnect(sh, "start", &RecordingController::onStartSignal, this);
			signal_handler_disconnect(sh, "stop", &RecordingController::onStopSignal, this);
		}
		obs_output_release(output_);
		output_ = nullptr;
	}
	if (videoEncoder_) {
		obs_encoder_release(videoEncoder_);
		videoEncoder_ = nullptr;
	}
	if (audioEncoder_) {
		obs_encoder_release(audioEncoder_);
		audioEncoder_ = nullptr;
	}
}

bool RecordingController::start(const Preset &preset, const std::string &fullFilePath)
{
	if (isRecording())
		return false;

	teardown(); // release any previous session's objects

	activePreset_ = preset;
	currentFilePath_ = fullFilePath;
	usesFfmpegOutput_ = EncoderFactory::usesFfmpegOutput(preset);

	const std::string outputId = EncoderFactory::outputId(preset);
	output_ = obs_output_create(outputId.c_str(), "harpia_file_output", nullptr, nullptr);
	if (!output_) {
		blog(LOG_ERROR, "[harpia] failed to create output '%s'", outputId.c_str());
		return false;
	}

	if (usesFfmpegOutput_) {
		// GIF / exotic formats: ffmpeg_output resolves the muxer from
		// format_name and encodes via libav directly (no obs_encoder objects).
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "url", fullFilePath.c_str());
		obs_data_set_string(settings, "format_name", formatToString(preset.format));
		obs_data_set_string(settings, "video_encoder", "gif");

		// GIFs are for short clips: downscale to keep the file sane. Scale the
		// canvas down so the longest edge is at most 640px, preserving aspect.
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi) && ovi.output_width > 0 && ovi.output_height > 0) {
			const uint32_t maxEdge = 640;
			uint32_t w = ovi.output_width;
			uint32_t h = ovi.output_height;
			if (w > maxEdge || h > maxEdge) {
				if (w >= h) {
					h = (uint32_t)((uint64_t)h * maxEdge / w);
					w = maxEdge;
				} else {
					w = (uint32_t)((uint64_t)w * maxEdge / h);
					h = maxEdge;
				}
			}
			obs_data_set_int(settings, "scale_width", w & ~1u);
			obs_data_set_int(settings, "scale_height", h & ~1u);
		}

		obs_output_update(output_, settings);
		obs_data_release(settings);

		// No audio track for GIF.
		obs_output_set_media(output_, obs_get_video(), nullptr);
	} else {
		// MP4 / MKV / MOV / AVI: encoded muxer recording (supports pause).
		std::string vid = EncoderFactory::videoEncoderId(preset);
		if (vid.empty())
			vid = "obs_x264"; // last-resort baseline
		const std::string aid = EncoderFactory::audioEncoderId(preset);

		obs_data_t *vsettings = obs_data_create();
		obs_data_set_string(vsettings, "rate_control", "CBR");
		obs_data_set_int(vsettings, "bitrate", defaultVideoBitrate(preset));
		videoEncoder_ = obs_video_encoder_create(vid.c_str(), "harpia_video", vsettings, nullptr);
		obs_data_release(vsettings);

		obs_data_t *asettings = obs_data_create();
		obs_data_set_int(asettings, "bitrate", preset.audioBitrateKbps > 0 ? preset.audioBitrateKbps : 160);
		audioEncoder_ = obs_audio_encoder_create(aid.c_str(), "harpia_audio", asettings, 0, nullptr);
		obs_data_release(asettings);

		if (!videoEncoder_ || !audioEncoder_) {
			blog(LOG_ERROR, "[harpia] failed to create encoders (video='%s' audio='%s')", vid.c_str(),
			     aid.c_str());
			teardown();
			return false;
		}

		obs_encoder_set_video(videoEncoder_, obs_get_video());
		obs_encoder_set_audio(audioEncoder_, obs_get_audio());

		obs_output_set_video_encoder(output_, videoEncoder_);
		obs_output_set_audio_encoder(output_, audioEncoder_, 0);

		obs_data_t *osettings = obs_data_create();
		obs_data_set_string(osettings, "path", fullFilePath.c_str());
		obs_output_update(output_, osettings);
		obs_data_release(osettings);
	}

	signal_handler_t *sh = obs_output_get_signal_handler(output_);
	signal_handler_connect(sh, "start", &RecordingController::onStartSignal, this);
	signal_handler_connect(sh, "stop", &RecordingController::onStopSignal, this);

	if (!obs_output_start(output_)) {
		const char *err = obs_output_get_last_error(output_);
		blog(LOG_ERROR, "[harpia] obs_output_start failed: %s", err ? err : "(unknown)");
		teardown();
		return false;
	}

	return true;
}

void RecordingController::stop()
{
	if (output_ && obs_output_active(output_))
		obs_output_stop(output_);
}

bool RecordingController::canPause() const
{
	if (!output_ || usesFfmpegOutput_)
		return false;
	return (obs_output_get_flags(output_) & OBS_OUTPUT_CAN_PAUSE) != 0;
}

bool RecordingController::pause(bool paused)
{
	if (!canPause())
		return false;
	obs_output_pause(output_, paused);
	return obs_output_paused(output_);
}

bool RecordingController::togglePause()
{
	return pause(!isPaused());
}

bool RecordingController::isRecording() const
{
	return output_ && obs_output_active(output_);
}

bool RecordingController::isPaused() const
{
	return output_ && obs_output_paused(output_);
}

void RecordingController::onStartSignal(void *data, calldata_t *)
{
	auto *self = static_cast<RecordingController *>(data);
	if (self->onStarted)
		self->onStarted();
}

void RecordingController::onStopSignal(void *data, calldata_t *)
{
	auto *self = static_cast<RecordingController *>(data);
	if (self->onFinished)
		self->onFinished(self->currentFilePath_);
}

} // namespace harpia
