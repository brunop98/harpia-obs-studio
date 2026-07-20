#include "RegionStore.hpp"

#include <obs.h>
#include <util/platform.h>

#include <algorithm>

namespace harpia {

namespace {
constexpr const char *kConfigSubdir = "harpia-recorder";
constexpr const char *kRegionsFile = "regions.json";

std::string resolveConfigDir()
{
	char *path = os_get_config_path_ptr(kConfigSubdir);
	std::string dir = path ? path : "";
	bfree(path);
	if (!dir.empty())
		os_mkdirs(dir.c_str());
	return dir;
}
} // namespace

RegionStore::RegionStore(std::string configDir) : configDir_(std::move(configDir))
{
	if (configDir_.empty())
		configDir_ = resolveConfigDir();
}

std::string RegionStore::filePath() const
{
	if (configDir_.empty())
		return {};
	return configDir_ + "/" + kRegionsFile;
}

void RegionStore::load()
{
	regions_.clear();
	const std::string path = filePath();
	if (path.empty())
		return;

	obs_data_t *root = obs_data_create_from_json_file(path.c_str());
	if (!root)
		return;

	obs_data_array_t *arr = obs_data_get_array(root, "regions");
	if (arr) {
		const size_t count = obs_data_array_count(arr);
		for (size_t i = 0; i < count; ++i) {
			obs_data_t *d = obs_data_array_item(arr, i);
			SavedRegion r;
			r.id = obs_data_get_string(d, "id");
			r.name = obs_data_get_string(d, "name");
			r.x = (int)obs_data_get_int(d, "x");
			r.y = (int)obs_data_get_int(d, "y");
			r.width = (int)obs_data_get_int(d, "width");
			r.height = (int)obs_data_get_int(d, "height");
			r.monitorIndex = (int)obs_data_get_int(d, "monitor_index"); // 0 for old files
			if (!r.id.empty() && r.width > 0 && r.height > 0)
				regions_.push_back(std::move(r));
			obs_data_release(d);
		}
		obs_data_array_release(arr);
	}
	obs_data_release(root);
}

bool RegionStore::save() const
{
	const std::string path = filePath();
	if (path.empty())
		return false;

	obs_data_t *root = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();
	for (const SavedRegion &r : regions_) {
		obs_data_t *d = obs_data_create();
		obs_data_set_string(d, "id", r.id.c_str());
		obs_data_set_string(d, "name", r.name.c_str());
		obs_data_set_int(d, "x", r.x);
		obs_data_set_int(d, "y", r.y);
		obs_data_set_int(d, "width", r.width);
		obs_data_set_int(d, "height", r.height);
		obs_data_set_int(d, "monitor_index", r.monitorIndex);
		obs_data_array_push_back(arr, d);
		obs_data_release(d);
	}
	obs_data_set_array(root, "regions", arr);
	obs_data_array_release(arr);

	const bool ok = obs_data_save_json_safe(root, path.c_str(), "tmp", "bak");
	obs_data_release(root);
	return ok;
}

void RegionStore::upsert(const SavedRegion &r)
{
	auto it = std::find_if(regions_.begin(), regions_.end(),
			       [&](const SavedRegion &e) { return e.id == r.id; });
	if (it != regions_.end())
		*it = r;
	else
		regions_.push_back(r);
	save();
}

void RegionStore::remove(const std::string &id)
{
	regions_.erase(std::remove_if(regions_.begin(), regions_.end(),
				      [&](const SavedRegion &e) { return e.id == id; }),
		       regions_.end());
	save();
}

const SavedRegion *RegionStore::find(const std::string &id) const
{
	auto it = std::find_if(regions_.begin(), regions_.end(),
			       [&](const SavedRegion &e) { return e.id == id; });
	return it != regions_.end() ? &*it : nullptr;
}

} // namespace harpia
