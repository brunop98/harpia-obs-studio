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

CaptureManager::~CaptureManager()
{
	stopCapture();
}

bool CaptureManager::startCapture(int monitorIndex)
{
	stopCapture();

	const char *id = platformCaptureId();

	// Default settings capture the primary display. Selecting a specific
	// monitor by index is a follow-up: enumerate the source's "monitor_id"
	// property list and set the chosen entry. `monitorIndex` is accepted now
	// so callers don't change when that lands.
	(void)monitorIndex;

	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "capture_cursor", true);

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
