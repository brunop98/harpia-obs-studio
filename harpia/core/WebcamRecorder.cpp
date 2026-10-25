#include "WebcamRecorder.hpp"
#include "ObsContext.hpp"
#include "platform/CameraAccess.hpp"
#if defined(__APPLE__)
#include "platform/mac/MacPlatform.hpp"
#endif

#include "AudioManager.hpp" // for AudioDevice {id, name}
#include "EncoderFactory.hpp"
#include "ObsOutputs.hpp"

#include <obs.h>

#include <cstring>
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

bool WebcamRecorder::supported()
{
	// Check the registered input source types rather than trying to create one —
	// creating an unregistered source logs noisy "Source ID not found" errors.
	const char *want = platformCameraId();
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_input_types(i, &id); ++i) {
		if (id && std::strcmp(id, want) == 0)
			return true;
	}
	return false;
}

// Pull camera devices out of a properties object into `out` (deduped by id).
static void readCameras(obs_properties_t *props, std::vector<AudioDevice> &out)
{
	if (!props)
		return;
	const char *key = deviceKey(props);
	if (!key)
		return;
	obs_property_t *p = obs_properties_get(props, key);
	const size_t count = obs_property_list_item_count(p);
	for (size_t i = 0; i < count; i++) {
		const char *name = obs_property_list_item_name(p, i);
		const char *devId = obs_property_list_item_string(p, i);
		if (!devId || !*devId)
			continue; // skip empty/placeholder rows
		bool dup = false;
		for (const AudioDevice &e : out)
			if (e.id == devId) {
				dup = true;
				break;
			}
		if (dup)
			continue;
		AudioDevice d;
		d.name = name ? name : devId;
		d.id = devId;
		out.push_back(std::move(d));
	}
}

std::vector<AudioDevice> WebcamRecorder::cameras()
{
	std::vector<AudioDevice> out;
#if defined(__APPLE__)
	// Enumerating AVFoundation metadata does not open a camera or trigger TCC.
	// The source's property list requires a live capture delegate, so it is
	// inappropriate for startup/readiness probes before the user grants access.
	for (const auto &camera : mac_platform::cameras())
		out.push_back({camera.id.toStdString(), camera.name.toStdString()});
	return out;
#else
	const char *id = platformCameraId();

	if (!supported()) {
		blog(LOG_WARNING,
		     "[harpia] camera source '%s' is not registered — is the capture plugin "
		     "(win-dshow needs ATL) loaded?",
		     id);
		return out;
	}

	// Enumerate from a real (private) source instance — most reliable and opens
	// no device (no video_device_id set) — then also merge the static properties
	// as a fallback in case one path returns an empty list.
	obs_source_t *probe = obs_source_create_private(id, "harpia_cam_probe", nullptr);
	if (probe) {
		obs_properties_t *ip = obs_source_properties(probe);
		readCameras(ip, out);
		obs_properties_destroy(ip);
		obs_source_release(probe);
	}
	if (out.empty()) {
		obs_properties_t *sp = obs_get_source_properties(id);
		readCameras(sp, out);
		obs_properties_destroy(sp);
	}

	// Log on CHANGE, not per call: this runs on the periodic readiness probe,
	// and "still 1 device" hundreds of times an hour buries real log lines.
	static size_t lastLogged = (size_t)-1;
	if (out.size() != lastLogged) {
		lastLogged = out.size();
		blog(LOG_INFO, "[harpia] webcam enumeration found %zu device(s) via '%s'", out.size(), id);
	}
	return out;
#endif
}

void WebcamRecorder::configureCameraSource(obs_source_t *source, int width, int height, int fps)
{
	if (!source || std::strcmp(obs_source_get_id(source), "macos-avcapture") != 0)
		return;
	// Camera video never implicitly enables a second microphone. The preset's
	// selected AudioManager inputs remain responsible for recorded sound.
	obs_source_set_audio_mixers(source, 0);
	obs_source_set_muted(source, true);
	obs_data_t *current = obs_source_get_settings(source);
	obs_data_t *settings = obs_data_create();
	obs_data_apply(settings, current);
	const std::string previousPreset = obs_data_get_string(current, "preset");
	obs_data_release(current);
	obs_data_set_bool(settings, "enable_audio", false);
	obs_properties_t *properties = obs_source_properties(source);
	bool configured = false;
	if (properties) {
		obs_data_set_bool(settings, "use_preset", false);
		obs_data_set_frames_per_second(settings, "frame_rate", {uint32_t(fps > 0 ? fps : 30), 1}, nullptr);
		obs_properties_apply_settings(properties, settings);
		const std::string wanted = std::to_string(width) + "x" + std::to_string(height);
		const std::string prefix = wanted + " ";
		std::vector<std::string> candidates;
		if (obs_property_t *formats = obs_properties_get(properties, "supported_format"))
			for (size_t i = 0; i < obs_property_list_item_count(formats); ++i) {
				const char *label = obs_property_list_item_name(formats, i);
				const char *value = obs_property_list_item_string(formats, i);
				if (label && value && *value && !obs_property_list_item_disabled(formats, i) &&
				    std::strncmp(label, prefix.c_str(), prefix.size()) == 0)
					candidates.emplace_back(value);
			}
		for (const auto &candidate : candidates) {
			obs_data_set_string(settings, "supported_format", candidate.c_str());
			obs_properties_apply_settings(properties, settings);
			obs_property_t *rates = obs_properties_get(properties, "frame_rate");
			for (size_t i = 0; rates && i < obs_property_frame_rate_fps_ranges_count(rates); ++i) {
				const auto minimum = obs_property_frame_rate_fps_range_min(rates, i);
				const auto maximum = obs_property_frame_rate_fps_range_max(rates, i);
				const uint64_t requested = uint32_t(fps > 0 ? fps : 30);
				if (minimum.denominator && maximum.denominator &&
				    requested * minimum.denominator >= minimum.numerator &&
				    requested * maximum.denominator <= maximum.numerator) {
					configured = true;
					break;
				}
			}
			if (configured)
				break;
		}
		if (!configured) {
			obs_data_set_bool(settings, "use_preset", true);
			obs_data_erase(settings, "supported_format");
			obs_data_erase(settings, "frame_rate");
			obs_properties_apply_settings(properties, settings);
			std::string fallback, exact;
			if (obs_property_t *presets = obs_properties_get(properties, "preset"))
				for (size_t i = 0; i < obs_property_list_item_count(presets); ++i) {
					const char *label = obs_property_list_item_name(presets, i);
					const char *value = obs_property_list_item_string(presets, i);
					if (!value || !*value || obs_property_list_item_disabled(presets, i))
						continue;
					if (fallback.empty() || previousPreset == value)
						fallback = value;
					if (label && wanted == label)
						exact = value;
				}
			const std::string selected = exact.empty() ? fallback : exact;
			if (!selected.empty()) {
				obs_data_set_string(settings, "preset", selected.c_str());
				configured = true;
			}
		}
		obs_properties_destroy(properties);
	}
	if (configured)
		obs_source_update(source, settings);
	else {
		obs_data_t *audioOnly = obs_data_create();
		obs_data_set_bool(audioOnly, "enable_audio", false);
		obs_source_update(source, audioOnly);
		obs_data_release(audioOnly);
		blog(LOG_WARNING, "[harpia] webcam has no advertised format for %dx%d @ %d fps; keeping camera defaults",
		     width, height, fps);
	}
	obs_data_release(settings);
}

WebcamRecorder::~WebcamRecorder()
{
	teardown();
}

bool WebcamRecorder::start(const Preset &preset, const std::string &deviceId, int width, int height, int fps,
			   const std::string &filePath, obs_source_t *sharedSource)
{
	teardown();
#if defined(__APPLE__)
	if (cameraAccessStatus() != CameraAccess::Allowed)
		return false; // consent is requested only by the explicit UI action
#endif

	if (width < 16 || height < 16)
		return false;
	if (fps < 1)
		fps = 30;

	// No video engine (graphics failed to start): nothing to record from, and
	// building an output on top of that crashes. See RecordingController::start.
	{
		struct obs_video_info probe;
		if (!obs_get_video_info(&probe)) {
			blog(LOG_ERROR, "[harpia] webcam recording refused: no video engine");
			return false;
		}
	}

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
#if defined(__APPLE__)
		obs_data_set_bool(cs, "enable_audio", false);
#else
		// DirectShow custom resolution/fps hints (ignored by other platforms;
		// the mix below enforces the final output size regardless).
		obs_data_set_int(cs, "res_type", 1);
		obs_data_set_string(cs, "resolution",
				    (std::to_string(width) + "x" + std::to_string(height)).c_str());
		obs_data_set_int(cs, "frame_interval", 10000000LL / fps);
#endif
		camera_ = obs_source_create(platformCameraId(), "harpia_webcam", cs, nullptr);
		obs_data_release(cs);
		if (!camera_) {
			blog(LOG_ERROR, "[harpia] failed to create webcam source");
			return false;
		}
#if defined(__APPLE__)
		configureCameraSource(camera_, width, height, fps);
#endif
	}

#if defined(__APPLE__)
	// OBS keeps source metadata even when a plugin could not create its
	// capture context. Refuse a camera with no usable picture dimensions.
	if (!obs_source_get_width(camera_) || !obs_source_get_height(camera_)) {
		blog(LOG_WARNING, "[harpia] the macOS camera has not delivered a usable video format");
		teardown();
		return false;
	}
#endif

	// 2. Private view + independent mix at the webcam's own resolution/fps.
	view_ = obs_view_create();
	obs_view_set_source(view_, 0, camera_);

	struct obs_video_info ovi = {};
	ovi.graphics_module = ObsContext::renderModule();
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

	// 3. Encoder bound to the webcam mix + its own muxer output. Honor the
	// preset's codec/GPU choice (previously hardcoded x264 @ 6000 CBR) and
	// always use quality mode — the preset's bitrate is sized for the screen,
	// not a small camera frame.
	std::string vid = EncoderFactory::videoEncoderId(preset);
	if (vid.empty())
		vid = "obs_x264";
	obs_data_t *vs = obs_data_create();
	EncoderFactory::applyRecordingQuality(vs, vid, 0);
	videoEncoder_ = obs_video_encoder_create(vid.c_str(), "harpia_webcam_venc", vs, nullptr);
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
	// obs_output_stop is asynchronous — releasing the output before the muxer
	// wrote its trailer truncates the file (an MP4 without its moov atom is
	// unplayable). Request the stop here; reap() tears down once the output
	// actually deactivated. Only when already inactive is teardown immediate.
	if (output_ && obs_output_active(output_)) {
		if (!stopRequested_) {
			obs_output_stop(output_);
			stopRequested_ = true;
		}
		return;
	}
	teardown();
}

void WebcamRecorder::reap()
{
	if (output_ && stopRequested_ && !obs_output_active(output_))
		teardown();
}

bool WebcamRecorder::isRecording() const
{
	return output_ && obs_output_active(output_);
}

void WebcamRecorder::pause(bool paused)
{
	if (!output_ || !obs_output_active(output_))
		return;
	if (obs_output_paused(output_) == paused)
		return;
	obs_output_pause(output_, paused);
}

void WebcamRecorder::teardown()
{
	stopRequested_ = false;
	if (output_) {
		// Finished before released: the view below is the encoder's video
		// source, and destroying it under a running encoder is a crash.
		stopOutputAndWait(output_);
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
