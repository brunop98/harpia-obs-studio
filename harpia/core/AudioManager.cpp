#include "AudioManager.hpp"

#include "ObsChannel.hpp"

#include <obs.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace harpia {

// libobs global audio channels: 1 = desktop, 3..6 = mic/aux (2 reserved for a
// second desktop device, which we don't use).
static constexpr uint32_t kDesktopChannel = 1;
static constexpr uint32_t kMicChannelFirst = 3;
static constexpr uint32_t kMicChannelLast = 6;
// One per PC capture: the whole mix takes the first; "Only these apps" one
// each, up to kMaxPcAudioApps. Channel 2 is libobs' second desktop device,
// which nothing else here uses; 7.. are past the microphones.
static constexpr uint32_t kPcChannels[] = {kDesktopChannel, 2, 7, 8, 9, 10, 11, 12};
static_assert(sizeof(kPcChannels) / sizeof(kPcChannels[0]) == size_t(kMaxPcAudioApps),
	      "a channel for every app");
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
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "device_id", deviceId);
	auto m = makeMeter(sourceId, name, settings, channel);
	obs_data_release(settings);
	if (!m)
		blog(LOG_WARNING, "[harpia] failed to create audio source '%s' (device '%s')", sourceId,
		     deviceId);
	return m;
}

std::unique_ptr<AudioManager::Meter> AudioManager::makeMeter(const char *sourceId, const char *name,
							    obs_data_t *settings, uint32_t channel)
{
	auto m = std::make_unique<Meter>();
	m->source = obs_source_create(sourceId, name, settings, nullptr);
	if (!m->source)
		return nullptr;

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
		if (channelShows(m->channel, m->source))
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

bool AudioManager::appCaptureAvailable()
{
	// Registered by core/AppAudioSource where Windows can single apps out.
	return obs_source_get_display_name(kAppAudioSourceId) != nullptr;
}

std::vector<PcAudioCapture> AudioManager::pcCaptures() const
{
	return pcOn_ ? pcPlan_ : std::vector<PcAudioCapture>();
}

void AudioManager::rebuildPc()
{
	const std::vector<PcAudioCapture> plan =
		pcOn_ ? pcAudioCaptures(pcChoice_, appCaptureAvailable()) : std::vector<PcAudioCapture>();
	if (plan == pcPlan_ && pc_.size() == plan.size())
		return; // already exactly this
	for (auto &m : pc_)
		destroyMeter(m);
	pc_.clear();
	pcPlan_.clear();
	for (size_t i = 0; i < plan.size() && i < std::size(kPcChannels); ++i) {
		const PcAudioCapture &c = plan[i];
		const std::string name = "harpia_pc_audio" + (i ? "_" + std::to_string(i + 1) : std::string());
		std::unique_ptr<Meter> m;
		if (c.wholeMix()) {
			m = makeMeter(outputCaptureId(), name.c_str(), "default", kPcChannels[i]);
		} else {
			obs_data_t *settings = obs_data_create();
			obs_data_set_string(settings, "exe", c.exe.c_str());
			obs_data_set_bool(settings, "exclude", c.exclude);
			m = makeMeter(kAppAudioSourceId, name.c_str(), settings, kPcChannels[i]);
			obs_data_release(settings);
			if (!m)
				blog(LOG_WARNING, "[harpia] failed to create the app audio capture for '%s'",
				     c.exe.c_str());
		}
		if (!m)
			continue;
		if (m->source)
			obs_source_set_volume(m->source, desktopVol_);
		pc_.push_back(std::move(m));
		pcPlan_.push_back(c);
	}
	if (pcOn_) {
		std::string what;
		for (const PcAudioCapture &c : pcPlan_)
			what += (what.empty() ? "" : ", ") +
				(c.wholeMix() ? std::string("everything") : (c.exclude ? "all but " : "only ") + c.exe);
		blog(LOG_INFO, "[harpia] PC audio from: %s", what.empty() ? "nothing" : what.c_str());
	}
}

void AudioManager::setDesktopEnabled(bool on)
{
	if (on == pcOn_)
		return;
	pcOn_ = on;
	rebuildPc();
}

void AudioManager::setPcAudioChoice(const PcAudioChoice &choice)
{
	if (choice == pcChoice_)
		return;
	pcChoice_ = choice;
	if (hold_) {
		pcPending_ = true;
		return;
	}
	rebuildPc();
}

void AudioManager::setDesktopVolume(float v)
{
	desktopVol_ = std::clamp(v, 0.f, 1.f);
	for (const auto &m : pc_)
		if (m && m->source)
			obs_source_set_volume(m->source, desktopVol_);
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
	// Several apps: the loudest of them, which is what the one bar can show.
	float db = kFloorDb;
	for (const auto &m : pc_)
		if (m)
			db = std::max(db, m->peakDb.load(std::memory_order_relaxed));
	return db;
}

void AudioManager::beginHold(const std::vector<std::string> &micDeviceIds)
{
	hold_ = true;
	for (const std::string &id : micDeviceIds) {
		if (mics_.count(id))
			continue; // already live; leave it exactly as it is
		const uint32_t ch = allocMicChannel();
		if (ch == 0) {
			// Only four channels exist. The ones that got them still work;
			// the rest simply cannot be switched on mid-recording.
			blog(LOG_INFO, "[harpia] mic '%s' not held: all audio channels in use",
			     id.c_str());
			break;
		}
		const std::string name = "harpia_mic_" + std::to_string(ch);
		auto meter = makeMeter(inputCaptureId(), name.c_str(), id.c_str(), ch);
		if (!meter)
			continue;
		// Created silent: it exists so libobs has it in the mix from the first
		// frame, but nothing of it reaches the file until it is switched on.
		obs_source_set_muted(meter->source, true);
		meter->muted = true;
		auto vol = micVols_.find(id);
		if (vol != micVols_.end() && meter->source)
			obs_source_set_volume(meter->source, vol->second);
		mics_.emplace(id, std::move(meter));
	}
}

void AudioManager::endHold()
{
	hold_ = false;
	if (pcPending_) {
		pcPending_ = false;
		rebuildPc();
	}
	// Whatever is still muted was only being held for the recording; drop it so
	// an idle app is not holding microphones open.
	for (auto it = mics_.begin(); it != mics_.end();) {
		if (it->second && it->second->muted) {
			destroyMeter(it->second);
			it = mics_.erase(it);
		} else {
			++it;
		}
	}
}

void AudioManager::setMicEnabled(const std::string &deviceId, bool on)
{
	auto it = mics_.find(deviceId);
	if (on) {
		if (it != mics_.end()) {
			// Already there -- held for the recording, or genuinely on
			// already. Unmuting is what makes a mid-recording switch work.
			if (it->second && it->second->muted) {
				obs_source_set_muted(it->second->source, false);
				it->second->muted = false;
			}
			return;
		}
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
		if (hold_ && it->second && it->second->source) {
			// Mid-recording: mute rather than destroy. Destroying frees the
			// channel, and re-creating on the same channel later is the very
			// thing libobs will not accept once the output is running.
			obs_source_set_muted(it->second->source, true);
			it->second->muted = true;
			return;
		}
		destroyMeter(it->second);
		mics_.erase(it);
	}
}

// Enabled means audible, not merely present: a held-but-muted source exists and
// is not being recorded.
bool AudioManager::micEnabled(const std::string &deviceId) const
{
	const auto it = mics_.find(deviceId);
	return it != mics_.end() && it->second && !it->second->muted;
}

float AudioManager::micPeakDb(const std::string &deviceId) const
{
	auto it = mics_.find(deviceId);
	return (it != mics_.end() && it->second) ? it->second->peakDb.load(std::memory_order_relaxed)
						 : kFloorDb;
}

void AudioManager::clear()
{
	for (auto &m : pc_)
		destroyMeter(m);
	pc_.clear();
	pcPlan_.clear();
	pcOn_ = false;
	for (auto &kv : mics_)
		destroyMeter(kv.second);
	mics_.clear();
}

AudioManager::~AudioManager()
{
	clear();
}

} // namespace harpia
