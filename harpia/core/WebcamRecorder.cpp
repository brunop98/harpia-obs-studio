#include "WebcamRecorder.hpp"

#include "AudioManager.hpp" // for AudioDevice {id, name}

#include <obs.h>

#include <string>

#ifndef DL_D3D11
#define DL_D3D11 ""
#endif
#ifndef DL_OPENGL
#define DL_OPENGL ""
#endif

namespace harpia {

const char *WebcamRecorder::platformCameraId()
{
#if defined(_WIN32)
	return "dshow_input";
#elif defined(__APPLE__)
	return "macos-avcapture";
#else
	return "v4l2_input";
#endif
}

static const char *deviceKey(obs_properties_t *props)
{
	for (const char *k : {"video_device_id", "device_id", "device"}) {
		if (obs_properties_get(props, k))
			return k;
	}
	return nullptr;
}

std::vector<AudioDevice> WebcamRecorder::cameras()
{
	std::vector<AudioDevice> out;
	obs_properties_t *props = obs_get_source_properties(platformCameraId());
	if (!props)
		return out;
	const char *key = deviceKey(props);
	if (key) {
		obs_property_t *p = obs_properties_get(props, key);
		const size_t count = obs_property_list_item_count(p);
		for (size_t i = 0; i < count; i++) {
			AudioDevice d;
			const char *name = obs_property_list_item_name(p, i);
			const char *id = obs_property_list_item_string(p, i);
			d.name = name ? name : "";
			d.id = id ? id : "";
			if (!d.id.empty())
				out.push_back(std::move(d));
		}
	}
	obs_properties_destroy(props);
	return out;
}

WebcamRecorder::~WebcamRecorder()
{
	teardown();
}

bool WebcamRecorder::start(const std::string &deviceId, int width, int height, int fps,
			   const std::string &filePath, obs_source_t *sharedSource)
{
	teardown();

	if (width < 16 || height < 16)
		return false;
	if (fps < 1)
		fps = 30;

	// 1. Camera source — reuse the shared (preview) one if provided, else open it.
	if (sharedSource) {
		camera_ = obs_source_get_ref(sharedSource);
		if (!camera_) {
			blog(LOG_ERROR, "[harpia] shared webcam source is gone");
			return false;
		}
	} else {
		// Fall back to the first available camera if none specified.
		std::string dev = deviceId;
		if (dev.empty()) {
			const auto cams = cameras();
			if (cams.empty())
				return false; // no camera available
			dev = cams.front().id;
		}

		obs_data_t *cs = obs_data_create();
		{
			obs_properties_t *props = obs_get_source_properties(platformCameraId());
			const char *key = props ? deviceKey(props) : nullptr;
			if (key)
				obs_data_set_string(cs, key, dev.c_str());
			if (props)
				obs_properties_destroy(props);
		}
		// DirectShow custom resolution/fps hints (ignored by other platforms;
		// the mix below enforces the final output size regardless).
		obs_data_set_int(cs, "res_type", 1);
		obs_data_set_string(cs, "resolution",
				    (std::to_string(width) + "x" + std::to_string(height)).c_str());
		obs_data_set_int(cs, "frame_interval", 10000000LL / fps);
		camera_ = obs_source_create(platformCameraId(), "harpia_webcam", cs, nullptr);
		obs_data_release(cs);
		if (!camera_) {
			blog(LOG_ERROR, "[harpia] failed to create webcam source");
			return false;
		}
	}

	// 2. Private view + independent mix at the webcam's own resolution/fps.
	view_ = obs_view_create();
	obs_view_set_source(view_, 0, camera_);

	struct obs_video_info ovi = {};
#if defined(_WIN32)
	ovi.graphics_module = DL_D3D11;
#else
	ovi.graphics_module = DL_OPENGL;
#endif
	ovi.fps_num = (uint32_t)fps;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = (uint32_t)width;
	ovi.base_height = ovi.output_height = (uint32_t)height;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.adapter = 0;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_BICUBIC;

	video_ = obs_view_add2(view_, &ovi);
	if (!video_) {
		blog(LOG_ERROR, "[harpia] failed to create webcam video mix");
		teardown();
		return false;
	}

	// 3. Encoder bound to the webcam mix + its own muxer output.
	obs_data_t *vs = obs_data_create();
	obs_data_set_string(vs, "rate_control", "CBR");
	obs_data_set_int(vs, "bitrate", 6000);
	videoEncoder_ = obs_video_encoder_create("obs_x264", "harpia_webcam_venc", vs, nullptr);
	obs_data_release(vs);
	if (!videoEncoder_) {
		teardown();
		return false;
	}
	obs_encoder_set_video(videoEncoder_, video_);

	output_ = obs_output_create("ffmpeg_muxer", "harpia_webcam_out", nullptr, nullptr);
	if (!output_) {
		teardown();
		return false;
	}
	obs_output_set_video_encoder(output_, videoEncoder_);

	obs_data_t *os = obs_data_create();
	obs_data_set_string(os, "path", filePath.c_str());
	obs_output_update(output_, os);
	obs_data_release(os);

	if (!obs_output_start(output_)) {
		const char *err = obs_output_get_last_error(output_);
		blog(LOG_ERROR, "[harpia] webcam output failed to start: %s", err ? err : "(unknown)");
		teardown();
		return false;
	}
	return true;
}

void WebcamRecorder::stop()
{
	if (output_ && obs_output_active(output_))
		obs_output_stop(output_);
	teardown();
}

bool WebcamRecorder::isRecording() const
{
	return output_ && obs_output_active(output_);
}

void WebcamRecorder::teardown()
{
	if (output_) {
		if (obs_output_active(output_))
			obs_output_stop(output_);
		obs_output_release(output_);
		output_ = nullptr;
	}
	if (videoEncoder_) {
		obs_encoder_release(videoEncoder_);
		videoEncoder_ = nullptr;
	}
	if (view_) {
		obs_view_remove(view_); // unbinds the mix; core tears it down
		obs_view_destroy(view_);
		view_ = nullptr;
		video_ = nullptr;
	}
	if (camera_) {
		obs_source_release(camera_);
		camera_ = nullptr;
	}
}

} // namespace harpia
