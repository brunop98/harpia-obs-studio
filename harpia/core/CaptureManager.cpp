#include "CaptureManager.hpp"

#include <obs.h>

#include <atomic>
#include <chrono>
#include <string>

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

const char *CaptureManager::platformWindowCaptureId()
{
#if defined(_WIN32)
	return "window_capture";
#else
	// Window capture isn't wired for macOS/Linux here.
	return nullptr;
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
	// Building source properties is expensive, and this runs on every capture
	// (re)creation and readiness probe. Displays change rarely — cache the
	// list for a few seconds (GUI-thread only, like every caller).
	static std::vector<MonitorOption> cache;
	static std::chrono::steady_clock::time_point cacheAt{};
	const auto now = std::chrono::steady_clock::now();
	if (!cache.empty() && now - cacheAt < std::chrono::seconds(5))
		return cache;

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
	if (!out.empty()) { // don't cache failures — retry those immediately
		cache = out;
		cacheAt = now;
	}
	return out;
}

std::vector<WindowOption> CaptureManager::enumerateWindows()
{
	std::vector<WindowOption> out;
	const char *id = platformWindowCaptureId();
	if (!id)
		return out;

	obs_properties_t *props = obs_get_source_properties(id);
	if (!props)
		return out;

	obs_property_t *p = obs_properties_get(props, "window");
	if (p) {
		const size_t count = obs_property_list_item_count(p);
		for (size_t i = 0; i < count; i++) {
			const char *name = obs_property_list_item_name(p, i);
			const char *val = obs_property_list_item_string(p, i);
			if (!val || !*val)
				continue; // skip the empty placeholder row
			out.push_back({name ? name : val, val});
		}
	}
	obs_properties_destroy(props);
	return out;
}

bool CaptureManager::sourceSize(uint32_t &w, uint32_t &h) const
{
	if (!source_)
		return false;
	w = obs_source_get_width(source_);
	h = obs_source_get_height(source_);
	return w > 0 && h > 0;
}

CaptureManager::~CaptureManager()
{
	stopCapture();
}

bool CaptureManager::startWindowCapture(const std::string &windowValue, bool captureCursor)
{
	stopCapture();

	const char *id = platformWindowCaptureId();
	if (!id) {
		blog(LOG_WARNING, "[harpia] window capture is not available on this platform");
		return false;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "cursor", captureCursor); // window_capture uses "cursor"
	if (!windowValue.empty())
		obs_data_set_string(settings, "window", windowValue.c_str());

	static std::atomic<uint64_t> creationCounter{0};
	const std::string sourceName =
		"harpia_window_capture_" + std::to_string(creationCounter.fetch_add(1));
	source_ = obs_source_create(id, sourceName.c_str(), settings, nullptr);
	obs_data_release(settings);

	if (!source_) {
		blog(LOG_ERROR, "[harpia] failed to create window capture source '%s'", id);
		return false;
	}

	obs_set_output_source(kVideoChannel, source_);
	return true;
}

bool CaptureManager::startCapture(int monitorIndex, bool captureCursor)
{
	stopCapture();

	const char *id = platformCaptureId();

	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "capture_cursor", captureCursor);

	// Apply the chosen display if the source supports selection.
	const std::vector<MonitorOption> monitors = enumerateMonitors();
	if (monitorIndex >= 0 && (size_t)monitorIndex < monitors.size()) {
		const MonitorOption &m = monitors[monitorIndex];
		if (m.isString)
			obs_data_set_string(settings, m.key.c_str(), m.strValue.c_str());
		else
			obs_data_set_int(settings, m.key.c_str(), m.intValue);
	}

	// libobs destroys released sources on a later video tick, so a freshly
	// re-created capture (mode/preset switch) can briefly coexist with the one
	// just released. Reusing a fixed name would then trip "duplicate name" and
	// force libobs to rename it. A unique name per creation avoids that — the
	// name is only a registry/UI label; harpia never looks the source up by it.
	static std::atomic<uint64_t> creationCounter{0};
	const std::string sourceName =
		"harpia_display_capture_" + std::to_string(creationCounter.fetch_add(1));
	source_ = obs_source_create(id, sourceName.c_str(), settings, nullptr);
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
	// Callers fire this per mouse-move during a drag and at 60 Hz while Follow
	// Mouse pans -- and both frequently land on the same integer rectangle two
	// events running. An unchanged region with the filter already in place is
	// a no-op; skip the obs_data allocation and the graphics-thread update.
	if (region == region_ && source_ && (cropFilter_ || !region.enabled))
		return;
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
