#include "CaptureManager.hpp"

#include <obs.h>

namespace harpia {

// libobs output channel that the encoders read from. Channel 0 is the video
// program feed; channels 1..6 are reserved for global audio devices.
static constexpr uint32_t kVideoChannel = 0;

const char *CaptureManager::platformCaptureId()
{
#if defined(_WIN32)
	return "monitor_capture";
#elif defined(__APPLE__)
	return "screen_capture";
#else
	// X11 default. A Wayland build would select
	// "pipewire-screen-capture-source" instead.
	return "xshm_input";
#endif
}

// Candidate settings keys used by the various platform capture sources to pick
// a display. We probe these in order and use whichever the source exposes.
static const char *findMonitorKey(obs_properties_t *props)
{
	for (const char *k : {"monitor_id", "monitor", "screen", "display"}) {
		if (obs_properties_get(props, k))
			return k;
	}
	return nullptr;
}

std::vector<MonitorOption> CaptureManager::enumerateMonitors()
{
	std::vector<MonitorOption> out;

	obs_properties_t *props = obs_get_source_properties(platformCaptureId());
	if (!props)
		return out;

	const char *key = findMonitorKey(props);
	if (key) {
		obs_property_t *p = obs_properties_get(props, key);
		const enum obs_combo_format fmt = obs_property_list_format(p);
		const size_t count = obs_property_list_item_count(p);
		for (size_t i = 0; i < count; i++) {
			MonitorOption m;
			const char *name = obs_property_list_item_name(p, i);
			m.name = name ? name : "";
			m.key = key;
			m.isString = (fmt == OBS_COMBO_FORMAT_STRING);
			if (m.isString) {
				const char *v = obs_property_list_item_string(p, i);
				m.strValue = v ? v : "";
			} else {
				m.intValue = obs_property_list_item_int(p, i);
			}
			out.push_back(std::move(m));
		}
	}

	obs_properties_destroy(props);
	return out;
}

CaptureManager::~CaptureManager()
{
	stopCapture();
}

bool CaptureManager::startCapture(int monitorIndex)
{
	stopCapture();

	const char *id = platformCaptureId();

	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "capture_cursor", true);

	// Apply the chosen display if the source supports selection.
	const std::vector<MonitorOption> monitors = enumerateMonitors();
	if (monitorIndex >= 0 && (size_t)monitorIndex < monitors.size()) {
		const MonitorOption &m = monitors[monitorIndex];
		if (m.isString)
			obs_data_set_string(settings, m.key.c_str(), m.strValue.c_str());
		else
			obs_data_set_int(settings, m.key.c_str(), m.intValue);
	}

	source_ = obs_source_create(id, "harpia_display_capture", settings, nullptr);
	obs_data_release(settings);

	if (!source_) {
		blog(LOG_ERROR, "[harpia] failed to create capture source '%s' (plugin missing?)", id);
		return false;
	}

	obs_set_output_source(kVideoChannel, source_);
	return true;
}

void CaptureManager::setRegion(const CaptureRegion &region)
{
	region_ = region;

	if (!source_)
		return;

	if (!region.enabled || region.width <= 0 || region.height <= 0) {
		// Clear any existing crop.
		if (cropFilter_) {
			obs_source_filter_remove(source_, cropFilter_);
			obs_source_release(cropFilter_);
			cropFilter_ = nullptr;
		}
		return;
	}

	// crop_filter in absolute mode records an exact sub-rectangle:
	// left->X, top->Y, cx->width, cy->height.
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "relative", false);
	obs_data_set_int(settings, "left", region.x);
	obs_data_set_int(settings, "top", region.y);
	obs_data_set_int(settings, "cx", region.width);
	obs_data_set_int(settings, "cy", region.height);

	if (!cropFilter_) {
		cropFilter_ = obs_source_create_private("crop_filter", "harpia_region_crop", settings);
		obs_source_filter_add(source_, cropFilter_);
	} else {
		obs_source_update(cropFilter_, settings);
	}
	obs_data_release(settings);
}

void CaptureManager::stopCapture()
{
	if (source_ && obs_get_output_source(kVideoChannel) == source_)
		obs_set_output_source(kVideoChannel, nullptr);

	if (cropFilter_) {
		if (source_)
			obs_source_filter_remove(source_, cropFilter_);
		obs_source_release(cropFilter_);
		cropFilter_ = nullptr;
	}

	if (source_) {
		obs_source_release(source_);
		source_ = nullptr;
	}
}

} // namespace harpia
