#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct obs_source;
typedef struct obs_source obs_source_t;
struct obs_volmeter;
typedef struct obs_volmeter obs_volmeter_t;

namespace harpia {

// An audio input/output device as exposed by the platform capture source's
// "device_id" property list. `id` "default" is the system default.
struct AudioDevice {
	std::string id;
	std::string name;
};

// Manages the recorder's audio capture: desktop/system ("PC") audio on output
// channel 1 and one microphone/input source per enabled device on channels
// 3..6. Enabled sources are wired into libobs global audio channels, so they are
// both **live** (metered immediately, before recording) and automatically mixed
// into the recording. Each active source carries an obs_volmeter so the UI can
// show a live level bar.
//
// Qt-free (core): the UI polls peak levels via the *PeakDb() getters.
class AudioManager {
public:
	AudioManager() = default;
	~AudioManager();

	AudioManager(const AudioManager &) = delete;
	AudioManager &operator=(const AudioManager &) = delete;

	// Microphone/input devices available for capture (includes a "Default"
	// entry). Requires modules loaded.
	static std::vector<AudioDevice> inputDevices();

	// Desktop/system audio.
	void setDesktopEnabled(bool on);
	bool desktopEnabled() const { return desktop_ != nullptr; }
	float desktopPeakDb() const; // latest peak in dB (-60..0); -60 if inactive

	// Microphones by device id.
	void setMicEnabled(const std::string &deviceId, bool on);
	bool micEnabled(const std::string &deviceId) const;
	float micPeakDb(const std::string &deviceId) const;

	// Per-source volume (linear 0..1). Values are remembered while a source is
	// disabled and re-applied when it is (re)created.
	void setDesktopVolume(float v);
	void setMicVolume(const std::string &deviceId, float v);

	// Release every audio source/meter.
	void clear();

	static const char *outputCaptureId(); // desktop/system audio
	static const char *inputCaptureId();   // microphone/line-in

private:
	struct Meter {
		obs_source_t *source = nullptr;
		obs_volmeter_t *volmeter = nullptr;
		std::atomic<float> peakDb{-60.f};
		uint32_t channel = 0;
	};

	static void volmeterCallback(void *param, const float *magnitude, const float *peak,
				     const float *inputPeak);

	std::unique_ptr<Meter> makeMeter(const char *sourceId, const char *name, const char *deviceId,
					 uint32_t channel);
	void destroyMeter(std::unique_ptr<Meter> &m);
	uint32_t allocMicChannel() const; // first free channel in 3..6, or 0

	std::unique_ptr<Meter> desktop_;
	std::map<std::string, std::unique_ptr<Meter>> mics_;
	float desktopVol_ = 1.f;
	std::map<std::string, float> micVols_;
};

} // namespace harpia
