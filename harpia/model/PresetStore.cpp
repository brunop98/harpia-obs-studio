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
	return d;
}

// Deserialize a single preset from an obs_data object.
Preset presetFromData(obs_data_t *d)
{
	Preset p;
	p.id = obs_data_get_string(d, "id");
	p.name = obs_data_get_string(d, "name");
	p.format = formatFromString(obs_data_get_string(d, "format"));
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
