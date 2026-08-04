#include "AudioOnlyRecorder.hpp"

#include "AudioFileWriter.hpp"

#include <obs.h>

#include <algorithm>
#include <cstdio>

namespace harpia {

namespace {

// How far the writer may fall behind before buffers are dropped. Eight seconds
// of stereo float is about 3 MB -- enough to ride out a stalled disk or a
// virus scanner waking up, and small enough that a genuinely wedged write
// cannot grow without bound through a two-hour take.
constexpr int kQueueSeconds = 8;

// Chosen over MP3 because it needs no external encoder, and over WAV because an
// hour of float PCM is 1.3 GB.
constexpr const char *kEncoder = "aac";

} // namespace

AudioOnlyRecorder::~AudioOnlyRecorder()
{
	requestStop();
	if (writer_.joinable())
		writer_.join();
}

bool AudioOnlyRecorder::start(const std::string &path, int bitrateKbps, std::string *err)
{
	const auto fail = [&](const std::string &why) {
		if (err)
			*err = why;
		return false;
	};

	if (active_.load())
		return fail("A recording is already running.");
	if (writer_.joinable())
		writer_.join(); // a previous take that has already finished

	finalPath_ = path;
	wavPath_ = path + ".part.wav";
	bitrateKbps_ = bitrateKbps > 0 ? bitrateKbps : 160;

	// Follow whatever libobs is actually mixing at; asking for a different
	// rate would put a resampler in the audio thread for no gain, since the
	// encoder can take any rate.
	rate_ = 48000;
	if (audio_t *a = obs_get_audio()) {
		const struct audio_output_info *aoi = audio_output_get_info(a);
		if (aoi && aoi->samples_per_sec > 0)
			rate_ = int(aoi->samples_per_sec);
	}
	// Stereo regardless of the mix's layout: it is what every player and every
	// downstream editor expects of a voice recording, and requesting it here
	// makes the tap's conversion libobs's problem rather than ours.
	channels_ = 2;

	wav_ = std::fopen(wavPath_.c_str(), "wb");
	if (!wav_)
		return fail("Could not create the scratch file next to the recording.");
	const auto header = buildWavHeaderFloat32(rate_, channels_, 0);
	if (std::fwrite(header.data(), 1, header.size(), wav_) != header.size()) {
		std::fclose(wav_);
		wav_ = nullptr;
		std::remove(wavPath_.c_str());
		return fail("Could not write to the recording folder.");
	}

	{
		std::lock_guard<std::mutex> lock(mutex_);
		queue_.clear();
		tap_ = AudioTapState{};
		tap_.queueCapFrames = uint64_t(rate_) * kQueueSeconds;
		stopRequested_ = false;
	}

	active_.store(true);
	capturing_.store(true);
	writer_ = std::thread(&AudioOnlyRecorder::writerLoop, this);

	struct audio_convert_info conv = {};
	conv.samples_per_sec = uint32_t(rate_);
	conv.format = AUDIO_FORMAT_FLOAT; // interleaved, which is what the WAV is
	conv.speakers = SPEAKERS_STEREO;
	obs_add_raw_audio_callback(0, &conv, &AudioOnlyRecorder::tapCallback, this);

	blog(LOG_INFO, "[harpia] audio-only tap started: %d Hz, %d ch -> %s", rate_, channels_,
	     finalPath_.c_str());
	return true;
}

void AudioOnlyRecorder::requestStop()
{
	if (!capturing_.exchange(false))
		return; // already stopping or never started

	// Disconnect FIRST, so no buffer can arrive after the drain decided the
	// queue was empty. Not called with mutex_ held: audio_output_disconnect
	// waits on the audio thread's input lock, which the tap callback holds
	// while it is taking mutex_.
	obs_remove_raw_audio_callback(0, &AudioOnlyRecorder::tapCallback, this);

	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopRequested_ = true;
	}
	cv_.notify_all();
}

void AudioOnlyRecorder::setPaused(bool paused)
{
	std::lock_guard<std::mutex> lock(mutex_);
	tap_.paused = paused;
}

bool AudioOnlyRecorder::paused() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return tap_.paused;
}

int64_t AudioOnlyRecorder::durationMs() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return tap_.durationMs(rate_);
}

uint64_t AudioOnlyRecorder::droppedFrames() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return tap_.dropped;
}

void AudioOnlyRecorder::tapCallback(void *param, size_t, struct audio_data *data)
{
	auto *self = static_cast<AudioOnlyRecorder *>(param);
	if (!self || !data || !data->data[0] || data->frames == 0)
		return;

	const float *src = reinterpret_cast<const float *>(data->data[0]);
	const uint64_t frames = data->frames;

	{
		std::lock_guard<std::mutex> lock(self->mutex_);
		const uint64_t queued = uint64_t(self->queue_.size()) / uint64_t(self->channels_);
		if (self->tap_.offer(frames, queued) == 0)
			return;
		self->queue_.insert(self->queue_.end(), src,
				    src + frames * uint64_t(self->channels_));
	}
	self->cv_.notify_one();
}

void AudioOnlyRecorder::writerLoop()
{
	std::vector<float> chunk;
	for (;;) {
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [this]() { return !queue_.empty() || stopRequested_; });
			chunk.clear();
			chunk.swap(queue_);
			if (chunk.empty() && stopRequested_)
				break;
		}
		if (!chunk.empty() && wav_)
			std::fwrite(chunk.data(), sizeof(float), chunk.size(), wav_);
	}

	patchWavHeader();
	if (wav_) {
		std::fclose(wav_);
		wav_ = nullptr;
	}

	const uint64_t dropped = droppedFrames();
	if (dropped > 0)
		blog(LOG_WARNING, "[harpia] audio-only recording dropped %llu frames (disk too slow)",
		     (unsigned long long)dropped);

	std::string err;
	const bool ok = encodeToFinal(&err);
	if (ok) {
		std::remove(wavPath_.c_str());
	} else {
		// The WAV is a complete, playable recording. Leaving it is strictly
		// better than deleting the only copy of a take that cannot be redone.
		blog(LOG_ERROR, "[harpia] audio-only encode failed (%s); keeping %s", err.c_str(),
		     wavPath_.c_str());
	}

	// Cleared BEFORE the callback: the owner's handler asks isRecording() on
	// its way to finalising, and a recorder that has just written its last
	// byte must not answer yes.
	active_.store(false);
	if (onFinished)
		onFinished(ok ? finalPath_ : wavPath_, ok, err);
}

void AudioOnlyRecorder::patchWavHeader()
{
	if (!wav_)
		return;
	std::fflush(wav_);
	uint64_t frames = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		frames = tap_.frames;
	}
	const uint64_t dataBytes = frames * uint64_t(channels_) * sizeof(float);
	const auto header = buildWavHeaderFloat32(rate_, channels_, dataBytes);
	if (std::fseek(wav_, 0, SEEK_SET) == 0)
		std::fwrite(header.data(), 1, header.size(), wav_);
	std::fflush(wav_);
}

bool AudioOnlyRecorder::encodeToFinal(std::string *err)
{
	uint64_t frames = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		frames = tap_.frames;
	}
	if (frames == 0) {
		if (err)
			*err = "The recording captured no audio.";
		return false;
	}

	std::FILE *in = std::fopen(wavPath_.c_str(), "rb");
	if (!in) {
		if (err)
			*err = "The captured audio could not be read back.";
		return false;
	}
	if (std::fseek(in, long(kWavHeaderBytes), SEEK_SET) != 0) {
		std::fclose(in);
		if (err)
			*err = "The captured audio could not be read back.";
		return false;
	}

	// Streamed back a chunk at a time: an hour of stereo float is 1.3 GB and
	// must never be resident just to change container.
	const auto pull = [&](float *dst, int maxFrames) -> int {
		const size_t want = size_t(maxFrames) * size_t(channels_);
		const size_t got = std::fread(dst, sizeof(float), want, in);
		return int(got / size_t(channels_));
	};

	const bool ok = encodeAudioStream(finalPath_, kEncoder, pull, rate_, channels_, bitrateKbps_,
					  err);
	std::fclose(in);
	return ok;
}

} // namespace harpia
