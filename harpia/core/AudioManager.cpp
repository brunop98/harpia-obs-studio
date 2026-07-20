#include "AudioManager.hpp"

#include <obs.h>

#include <algorithm>
#include <cmath>

namespace harpia {

// libobs global audio channels: 1 = desktop, 3..6 = mic/aux (2 reserved for a
// second desktop device, which we don't use).
static constexpr uint32_t kDesktopChannel = 1;
static constexpr uint32_t kMicChannelFirst = 3;
static constexpr uint32_t kMicChannelLast = 6;
static constexpr float kFloorDb = -60.f;

const char *AudioManager::outputCaptureId()
{
#if defined(_WIN32)
	return "wasapi_output_capture";
#elif defined(__APPLE__)
	return "coreaudio_output_capture";
#else
	return "pulse_output_capture";
#endif
}

const char *AudioManager::inputCaptureId()
{
#if defined(_WIN32)
	return "wasapi_input_capture";
#elif defined(__APPLE__)
	return "coreaudio_input_capture";
#else
	return "pulse_input_capture";
#endif
}

std::vector<AudioDevice> AudioManager::inputDevices()
{
	std::vector<AudioDevice> out;
	obs_properties_t *props = obs_get_source_properties(inputCaptureId());
	if (!props)
		return out;

	obs_property_t *p = obs_properties_get(props, "device_id");
	if (p) {
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

void AudioManager::volmeterCallback(void *param, const float *, const float *peak, const float *)
{
	auto *meter = static_cast<Meter *>(param);
	float mx = -200.f;
	for (int i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		const float v = peak[i];
		if (std::isfinite(v) && v > mx)
			mx = v;
	}
	if (!std::isfinite(mx) || mx < kFloorDb)
		mx = kFloorDb;
	else if (mx > 0.f)
		mx = 0.f;
	meter->peakDb.store(mx, std::memory_order_relaxed);
}

std::unique_ptr<AudioManager::Meter> AudioManager::makeMeter(const char *sourceId, const char *name,
							    const char *deviceId, uint32_t channel)
{
	auto m = std::make_unique<Meter>();

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "device_id", deviceId);
	m->source = obs_source_create(sourceId, name, settings, nullptr);
	obs_data_release(settings);
	if (!m->source) {
		blog(LOG_WARNING, "[harpia] failed to create audio source '%s' (device '%s')", sourceId,
		     deviceId);
		return nullptr;
	}

	m->channel = channel;
	obs_set_output_source(channel, m->source);

	m->volmeter = obs_volmeter_create(OBS_FADER_LOG);
	if (m->volmeter) {
		obs_volmeter_attach_source(m->volmeter, m->source);
		obs_volmeter_add_callback(m->volmeter, &AudioManager::volmeterCallback, m.get());
	} else {
		// The source still records fine — only the level meter is unavailable.
		blog(LOG_WARNING, "[harpia] failed to create volmeter for '%s'", name);
	}
	return m;
}

void AudioManager::destroyMeter(std::unique_ptr<Meter> &m)
{
	if (!m)
		return;
	if (m->volmeter) {
		obs_volmeter_remove_callback(m->volmeter, &AudioManager::volmeterCallback, m.get());
		obs_volmeter_destroy(m->volmeter);
		m->volmeter = nullptr;
	}
	if (m->source) {
		if (obs_get_output_source(m->channel) == m->source)
			obs_set_output_source(m->channel, nullptr);
		obs_source_release(m->source);
		m->source = nullptr;
	}
	m.reset();
}

uint32_t AudioManager::allocMicChannel() const
{
	for (uint32_t ch = kMicChannelFirst; ch <= kMicChannelLast; ch++) {
		bool used = false;
		for (const auto &kv : mics_) {
			if (kv.second && kv.second->channel == ch) {
				used = true;
				break;
			}
		}
		if (!used)
			return ch;
	}
	return 0; // no free channel
}

void AudioManager::setDesktopEnabled(bool on)
{
	if (on && !desktop_) {
		desktop_ = makeMeter(outputCaptureId(), "harpia_pc_audio", "default", kDesktopChannel);
		if (desktop_ && desktop_->source)
			obs_source_set_volume(desktop_->source, desktopVol_);
	} else if (!on && desktop_) {
		destroyMeter(desktop_);
	}
}

void AudioManager::setDesktopVolume(float v)
{
	desktopVol_ = std::clamp(v, 0.f, 1.f);
	if (desktop_ && desktop_->source)
		obs_source_set_volume(desktop_->source, desktopVol_);
}

void AudioManager::setMicVolume(const std::string &deviceId, float v)
{
	const float vol = std::clamp(v, 0.f, 1.f);
	micVols_[deviceId] = vol;
	auto it = mics_.find(deviceId);
	if (it != mics_.end() && it->second && it->second->source)
		obs_source_set_volume(it->second->source, vol);
}

float AudioManager::desktopPeakDb() const
{
	return desktop_ ? desktop_->peakDb.load(std::memory_order_relaxed) : kFloorDb;
}

void AudioManager::setMicEnabled(const std::string &deviceId, bool on)
{
	auto it = mics_.find(deviceId);
	if (on) {
		if (it != mics_.end())
			return; // already enabled
		const uint32_t ch = allocMicChannel();
		if (ch == 0) {
			blog(LOG_WARNING, "[harpia] no free audio channel for mic '%s'", deviceId.c_str());
			return;
		}
		const std::string name = "harpia_mic_" + std::to_string(ch);
		auto meter = makeMeter(inputCaptureId(), name.c_str(), deviceId.c_str(), ch);
		if (meter) {
			// Re-apply the remembered volume for this device.
			auto vol = micVols_.find(deviceId);
			if (vol != micVols_.end() && meter->source)
				obs_source_set_volume(meter->source, vol->second);
			mics_.emplace(deviceId, std::move(meter));
		}
	} else if (it != mics_.end()) {
		destroyMeter(it->second);
		mics_.erase(it);
	}
}

bool AudioManager::micEnabled(const std::string &deviceId) const
{
	return mics_.find(deviceId) != mics_.end();
}

float AudioManager::micPeakDb(const std::string &deviceId) const
{
	auto it = mics_.find(deviceId);
	return (it != mics_.end() && it->second) ? it->second->peakDb.load(std::memory_order_relaxed)
						 : kFloorDb;
}

void AudioManager::clear()
{
	destroyMeter(desktop_);
	for (auto &kv : mics_)
		destroyMeter(kv.second);
	mics_.clear();
}

AudioManager::~AudioManager()
{
	clear();
}

} // namespace harpia
