#pragma once

#include "AudioOnlyRecorder.hpp"
#include "model/Preset.hpp"

#include <functional>
#include <memory>
#include <string>

struct obs_output;
struct obs_encoder;
typedef struct obs_output obs_output_t;
typedef struct obs_encoder obs_encoder_t;
struct signal_handler;
typedef struct signal_handler signal_handler_t;
struct calldata;
typedef struct calldata calldata_t;

namespace harpia {

// Builds the encode+mux pipeline for a Preset and drives a single recording:
// start / stop / pause / resume. Models the OBS SimpleOutput recording path but
// stripped to just file recording.
//
// Audio Only is the one mode that does not go through an obs output -- it
// cannot, see AudioOnlyRecorder for why -- so it is handled here behind the
// same six methods. Everything above this class asks isRecording()/pause()/
// stop() and never learns which of the two paths is running.
//
// Threading: the finished-callback may fire on a libobs thread. UI code must
// marshal it onto the GUI thread (MainWindow does this).
class RecordingController {
public:
	RecordingController() = default;
	~RecordingController();

	RecordingController(const RecordingController &) = delete;
	RecordingController &operator=(const RecordingController &) = delete;

	// Invoked (on a libobs thread) when a recording fully stops, with the
	// written file path.
	std::function<void(const std::string &path)> onFinished;

	// Invoked (on a libobs thread) when recording successfully starts.
	std::function<void()> onStarted;

	// Start recording `preset` to `fullFilePath`. Requires the video/audio
	// graph and capture source to already be set up. Returns false on failure.
	bool start(const Preset &preset, const std::string &fullFilePath);

	// Request the recording to stop (asynchronous; onFinished fires when done).
	void stop();

	// Last-resort watchdog kill for a stop that hung (stuck muxer/encoder):
	// aborts the output immediately. The file may be incomplete but the app
	// stays alive — no Task Manager needed.
	void forceStop();

	// Pause/resume. Only valid for encoded muxer recordings (our MP4/MKV path).
	// Returns the resulting paused state.
	bool pause(bool paused);
	bool togglePause();

	bool isRecording() const;
	bool isPaused() const;
	bool canPause() const;

	// True while an Audio Only take has stopped capturing and is encoding the
	// file. This is work, not a hang: the stop watchdog has to know the
	// difference, or it force-closes a perfectly healthy encode of a long
	// recording and tells the user the file may be incomplete.
	bool isFinishing() const;

	const std::string &currentFilePath() const { return currentFilePath_; }

	// Why the last recording stopped: 0 = clean stop (OBS_OUTPUT_SUCCESS),
	// anything else is an output error (disk full, write failure…). Valid after
	// the stop signal fired; reset on start().
	int lastStopCode() const { return lastStopCode_; }
	const std::string &lastStopError() const { return lastStopError_; }

private:
	void teardown();
	static void onStartSignal(void *data, calldata_t *cd);
	static void onStopSignal(void *data, calldata_t *cd);

	// Non-null only for an Audio Only take; output_ stays null in that case and
	// vice versa, which is what every branch below tests on.
	std::unique_ptr<AudioOnlyRecorder> tap_;

	obs_output_t *output_ = nullptr;
	obs_encoder_t *videoEncoder_ = nullptr;
	obs_encoder_t *audioEncoder_ = nullptr;
	std::string currentFilePath_;
	Preset activePreset_;
	bool usesFfmpegOutput_ = false;
	int lastStopCode_ = 0;
	std::string lastStopError_;
};

} // namespace harpia
