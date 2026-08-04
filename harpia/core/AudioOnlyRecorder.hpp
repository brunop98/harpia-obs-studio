#pragma once

// Audio Only recording: a tap on the mixed audio, straight to a file, with no
// obs output involved at all.
//
// The reason it cannot use one is in obs-output.c. can_begin_data_capture gates
// on flag_video(), which reads the output's REGISTERED flags -- a compile-time
// constant of the plugin, not a description of what was attached -- and every
// FFmpeg output in this build is declared OBS_OUTPUT_AV. So libobs demands a
// video encoder however the output is wired, and refuses without setting an
// error string. There is no audio-only output in the build to hand it to.
//
// obs_add_raw_audio_callback is the way past that. It connects an input to mix
// 0 of the audio thread, which runs continuously from obs_reset_audio and mixes
// every active source whether or not anything is recording (audio-io.c:160
// derives its active mixes from the connected inputs). So the tap alone brings
// the mix to life, and nothing about outputs applies to it.
//
// The shape of a take:
//
//   start()   connect the tap, open a float WAV, spin up the writer thread
//   ...       the libobs audio thread hands over buffers; the writer drains them
//   stop()    disconnect the tap, drain, patch the WAV header, encode to M4A,
//             delete the WAV, then call onFinished
//
// The WAV in the middle is not a detour. It keeps libav off the audio thread,
// it bounds memory to one chunk however long the take is, and if the encode
// fails at the end the user is left holding a playable file rather than
// nothing.

#include "AudioTap.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct audio_data;

namespace harpia {

class AudioOnlyRecorder {
public:
	AudioOnlyRecorder() = default;
	~AudioOnlyRecorder();

	AudioOnlyRecorder(const AudioOnlyRecorder &) = delete;
	AudioOnlyRecorder &operator=(const AudioOnlyRecorder &) = delete;

	// Called on the writer thread once the file is complete: (path, ok, why).
	// `path` is the final file when ok, and the leftover WAV when the encode
	// failed but something was captured -- so the caller can still hand the
	// user a recording.
	std::function<void(const std::string &path, bool ok, const std::string &why)> onFinished;

	// Begin capturing to `path` (a .m4a). False with *err if the tap or the
	// scratch file could not be opened; nothing is left behind in that case.
	bool start(const std::string &path, int bitrateKbps, std::string *err);

	// Disconnect and finish, asynchronously: the encode runs on the writer
	// thread and onFinished fires when the file is closed. Safe to call twice.
	void requestStop();

	void setPaused(bool paused);
	bool paused() const;

	// True from start() until onFinished has fired -- capturing OR encoding.
	bool active() const { return active_.load(); }
	// True only while the tap is connected. Distinguished from active() so the
	// UI can say "finishing" rather than showing a stopped recording as live.
	bool capturing() const { return capturing_.load(); }

	// Length of the recording so far, from the frames actually written, so a
	// paused stretch does not inflate it.
	int64_t durationMs() const;

	// Frames dropped because the writer could not keep up. Nonzero means the
	// file has a gap in it and the log says so.
	uint64_t droppedFrames() const;

	const std::string &path() const { return finalPath_; }

private:
	static void tapCallback(void *param, size_t mixIdx, struct audio_data *data);
	void writerLoop();
	bool encodeToFinal(std::string *err);
	void patchWavHeader();

	std::string finalPath_;
	std::string wavPath_;
	std::FILE *wav_ = nullptr;
	int rate_ = 48000;
	int channels_ = 2;
	int bitrateKbps_ = 160;

	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::vector<float> queue_; // interleaved, protected by mutex_
	AudioTapState tap_;        // protected by mutex_
	bool stopRequested_ = false;

	std::thread writer_;
	std::atomic<bool> active_{false};
	std::atomic<bool> capturing_{false};
};

} // namespace harpia
