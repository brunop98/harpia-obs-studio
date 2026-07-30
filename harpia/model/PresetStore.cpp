#include "PresetStore.hpp"

#include <obs.h>
#include <util/platform.h>

namespace harpia {

namespace {

constexpr const char *kConfigSubdir = "harpia-recorder";
constexpr const char *kPresetsFile = "presets.json";

// Resolve (and create) the per-user config directory for Harpia.
std::string resolveConfigDir()
{
	char *path = os_get_config_path_ptr(kConfigSubdir);
	std::string dir = path ? path : "";
	bfree(path);
	if (!dir.empty())
		os_mkdirs(dir.c_str());
	return dir;
}

// Serialize a single preset into an obs_data object.
obs_data_t *presetToData(const Preset &p)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "id", p.id.c_str());
	obs_data_set_string(d, "name", p.name.c_str());
	obs_data_set_string(d, "format", formatToString(p.format));
	obs_data_set_string(d, "codec", codecToString(p.codec));
	obs_data_set_string(d, "frame_rate_mode", frameRateModeToString(p.frameRateMode));
	obs_data_set_int(d, "fps", p.fps);
	obs_data_set_string(d, "resolution_mode", resolutionModeToString(p.resolutionMode));
	obs_data_set_int(d, "width", p.width);
	obs_data_set_int(d, "height", p.height);
	obs_data_set_string(d, "output_folder", p.outputFolder.c_str());
	obs_data_set_int(d, "monitor_index", p.monitorIndex);
	obs_data_set_bool(d, "gpu_compression", p.gpuCompression);
	obs_data_set_int(d, "video_bitrate_kbps", p.videoBitrateKbps);
	obs_data_set_int(d, "audio_bitrate_kbps", p.audioBitrateKbps);
	obs_data_set_string(d, "filename_template", p.filenameTemplate.c_str());
	obs_data_set_int(d, "idle_timeout_seconds", p.idleTimeoutSeconds);
	// The stored key keeps its original name: it is internal, and renaming it
	// would silently reset the setting to Off for everyone who already has one.
	obs_data_set_int(d, "region_leave_stop_seconds", p.regionLeavePauseSeconds);
	obs_data_set_bool(d, "pause_on_focus_loss", p.pauseOnFocusLoss);
	obs_data_set_int(d, "recording_counter", p.recordingCounter);
	obs_data_set_int(d, "countdown_seconds", p.countdownSeconds);
	obs_data_set_int(d, "min_recording_seconds", p.minRecordingSeconds);
	obs_data_set_string(d, "google_drive_link", p.googleDriveLink.c_str());
	obs_data_set_bool(d, "show_screen_border", p.showScreenBorder);
	obs_data_set_string(d, "screen_border_color", p.screenBorderColor.c_str());
	obs_data_set_int(d, "screen_border_thickness", p.screenBorderThickness);
	obs_data_set_bool(d, "record_desktop_audio", p.recordDesktopAudio);
	obs_data_array_t *mics = obs_data_array_create();
	for (const std::string &id : p.micDeviceIds) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "id", id.c_str());
		obs_data_array_push_back(mics, item);
		obs_data_release(item);
	}
	obs_data_set_array(d, "mic_device_ids", mics);
	obs_data_array_release(mics);
	obs_data_set_double(d, "desktop_volume", p.desktopVolume);
	obs_data_array_t *vols = obs_data_array_create();
	for (const auto &kv : p.micVolumes) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "id", kv.first.c_str());
		obs_data_set_double(item, "volume", kv.second);
		obs_data_array_push_back(vols, item);
		obs_data_release(item);
	}
	obs_data_set_array(d, "mic_volumes", vols);
	obs_data_array_release(vols);
	obs_data_set_bool(d, "show_mouse_cursor", p.showMouseCursor);
	obs_data_set_bool(d, "show_mouse_area", p.showMouseArea);
	obs_data_set_string(d, "mouse_highlight_color", p.mouseHighlightColor.c_str());
	obs_data_set_int(d, "mouse_highlight_size", p.mouseHighlightSize);
	obs_data_set_bool(d, "record_mouse_clicks", p.recordMouseClicks);
	obs_data_set_string(d, "left_click_color", p.leftClickColor.c_str());
	obs_data_set_string(d, "right_click_color", p.rightClickColor.c_str());
	obs_data_set_bool(d, "webcam_enabled", p.webcamEnabled);
	obs_data_set_string(d, "webcam_device_id", p.webcamDeviceId.c_str());
	obs_data_set_int(d, "webcam_width", p.webcamWidth);
	obs_data_set_int(d, "webcam_height", p.webcamHeight);
	obs_data_set_int(d, "webcam_fps", p.webcamFps);
	obs_data_set_bool(d, "webcam_use_custom_folder", p.webcamUseCustomFolder);
	obs_data_set_string(d, "webcam_folder", p.webcamFolder.c_str());
	return d;
}

// Deserialize a single preset from an obs_data object.
Preset presetFromData(obs_data_t *d)
{
	Preset p;
	p.id = obs_data_get_string(d, "id");
	p.name = obs_data_get_string(d, "name");
	p.format = formatFromString(obs_data_get_string(d, "format"));
	p.codec = codecFromString(obs_data_get_string(d, "codec"));
	p.frameRateMode = frameRateModeFromString(obs_data_get_string(d, "frame_rate_mode"));
	p.fps = (int)obs_data_get_int(d, "fps");
	p.resolutionMode = resolutionModeFromString(obs_data_get_string(d, "resolution_mode"));
	p.width = (int)obs_data_get_int(d, "width");
	p.height = (int)obs_data_get_int(d, "height");
	p.outputFolder = obs_data_get_string(d, "output_folder");
	p.monitorIndex = (int)obs_data_get_int(d, "monitor_index");
	p.gpuCompression = obs_data_get_bool(d, "gpu_compression");
	p.videoBitrateKbps = (int)obs_data_get_int(d, "video_bitrate_kbps");
	p.audioBitrateKbps = (int)obs_data_get_int(d, "audio_bitrate_kbps");
	p.filenameTemplate = obs_data_get_string(d, "filename_template");
	p.idleTimeoutSeconds = (int)obs_data_get_int(d, "idle_timeout_seconds");
	// Default -1 (off), not obs_data_get_int's 0 — 0 here means "stop the moment
	// the pointer leaves", so a preset written before this existed would come
	// back with the feature not merely on but at its most aggressive setting.
	obs_data_set_default_int(d, "region_leave_stop_seconds", -1);
	p.regionLeavePauseSeconds = (int)obs_data_get_int(d, "region_leave_stop_seconds");
	p.pauseOnFocusLoss = obs_data_get_bool(d, "pause_on_focus_loss");
	if (obs_data_has_user_value(d, "recording_counter"))
		p.recordingCounter = (int)obs_data_get_int(d, "recording_counter");
	p.countdownSeconds = (int)obs_data_get_int(d, "countdown_seconds");
	p.minRecordingSeconds = (int)obs_data_get_int(d, "min_recording_seconds");
	if (const char *gd = obs_data_get_string(d, "google_drive_link"); gd && *gd)
		p.googleDriveLink = gd;
	p.showScreenBorder = obs_data_get_bool(d, "show_screen_border");
	if (const char *bc = obs_data_get_string(d, "screen_border_color"); bc && *bc)
		p.screenBorderColor = bc;
	if (obs_data_has_user_value(d, "screen_border_thickness"))
		p.screenBorderThickness = (int)obs_data_get_int(d, "screen_border_thickness");
	p.recordDesktopAudio = obs_data_get_bool(d, "record_desktop_audio");
	obs_data_array_t *mics = obs_data_get_array(d, "mic_device_ids");
	const size_t micCount = mics ? obs_data_array_count(mics) : 0;
	for (size_t i = 0; i < micCount; i++) {
		obs_data_t *item = obs_data_array_item(mics, i);
		p.micDeviceIds.emplace_back(obs_data_get_string(item, "id"));
		obs_data_release(item);
	}
	obs_data_array_release(mics);
	obs_data_set_default_double(d, "desktop_volume", 1.0); // pre-volume presets
	p.desktopVolume = obs_data_get_double(d, "desktop_volume");
	obs_data_array_t *vols = obs_data_get_array(d, "mic_volumes");
	const size_t volCount = vols ? obs_data_array_count(vols) : 0;
	for (size_t i = 0; i < volCount; i++) {
		obs_data_t *item = obs_data_array_item(vols, i);
		p.micVolumes[obs_data_get_string(item, "id")] = obs_data_get_double(item, "volume");
		obs_data_release(item);
	}
	obs_data_array_release(vols);

	// Defaults so presets saved before these fields keep sensible values.
	obs_data_set_default_bool(d, "show_mouse_cursor", true);
	obs_data_set_default_int(d, "mouse_highlight_size", 60);
	obs_data_set_default_string(d, "mouse_highlight_color", "#ffd54a");
	obs_data_set_default_string(d, "left_click_color", "#4a90e2");
	obs_data_set_default_string(d, "right_click_color", "#e2534a");
	p.showMouseCursor = obs_data_get_bool(d, "show_mouse_cursor");
	p.showMouseArea = obs_data_get_bool(d, "show_mouse_area");
	p.mouseHighlightColor = obs_data_get_string(d, "mouse_highlight_color");
	p.mouseHighlightSize = (int)obs_data_get_int(d, "mouse_highlight_size");
	p.recordMouseClicks = obs_data_get_bool(d, "record_mouse_clicks");
	p.leftClickColor = obs_data_get_string(d, "left_click_color");
	p.rightClickColor = obs_data_get_string(d, "right_click_color");

	obs_data_set_default_int(d, "webcam_width", 1280);
	obs_data_set_default_int(d, "webcam_height", 720);
	obs_data_set_default_int(d, "webcam_fps", 30);
	p.webcamEnabled = obs_data_get_bool(d, "webcam_enabled");
	p.webcamDeviceId = obs_data_get_string(d, "webcam_device_id");
	p.webcamWidth = (int)obs_data_get_int(d, "webcam_width");
	p.webcamHeight = (int)obs_data_get_int(d, "webcam_height");
	p.webcamFps = (int)obs_data_get_int(d, "webcam_fps");
	p.webcamUseCustomFolder = obs_data_get_bool(d, "webcam_use_custom_folder");
	p.webcamFolder = obs_data_get_string(d, "webcam_folder");
	return p;
}

} // namespace

PresetStore::PresetStore(std::string configDir) : configDir_(std::move(configDir))
{
	if (configDir_.empty())
		configDir_ = resolveConfigDir();
}

std::string PresetStore::filePath() const
{
	if (configDir_.empty())
		return kPresetsFile;
	return configDir_ + "/" + kPresetsFile;
}

void PresetStore::load(const std::string &defaultOutputFolder)
{
	presets_.clear();

	const std::string path = filePath();
	obs_data_t *root = obs_data_create_from_json_file(path.c_str());
	if (root) {
		obs_data_array_t *arr = obs_data_get_array(root, "presets");
		const size_t count = arr ? obs_data_array_count(arr) : 0;
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			presets_.push_back(presetFromData(item));
			obs_data_release(item);
		}
		obs_data_array_release(arr);
		obs_data_release(root);
	}

	if (presets_.empty()) {
		presets_.push_back(Preset::makeDefault(defaultOutputFolder));
		save();
	}
}

bool PresetStore::save() const
{
	obs_data_t *root = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();
	for (const Preset &p : presets_) {
		obs_data_t *item = presetToData(p);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "presets", arr);

	const std::string path = filePath();
	const bool ok = obs_data_save_json_safe(root, path.c_str(), "tmp", "bak");

	obs_data_array_release(arr);
	obs_data_release(root);
	return ok;
}

const Preset *PresetStore::find(const std::string &id) const
{
	for (const Preset &p : presets_) {
		if (p.id == id)
			return &p;
	}
	return nullptr;
}

void PresetStore::upsert(const Preset &preset)
{
	for (Preset &p : presets_) {
		if (p.id == preset.id) {
			p = preset;
			save();
			return;
		}
	}
	presets_.push_back(preset);
	save();
}

void PresetStore::remove(const std::string &id)
{
	if (presets_.size() <= 1)
		return; // keep at least one preset
	for (auto it = presets_.begin(); it != presets_.end(); ++it) {
		if (it->id == id) {
			presets_.erase(it);
			save();
			return;
		}
	}
}

} // namespace harpia
