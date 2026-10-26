#include "RecordingController.hpp"

#include "EncoderFactory.hpp"
#include "ObsOutputs.hpp"
#include "ModeCapabilities.hpp"
#include "VideoStatus.hpp"

#include <obs.h>

#include <algorithm>

namespace harpia {

RecordingController::~RecordingController()
{
	teardown();
}

void RecordingController::teardown()
{
	if (tap_) {
		// Blocks in the destructor until the writer thread has closed the
		// file, for the same reason the output is stopped rather than
		// released below: a half-written recording is worse than a wait.
		tap_.reset();
	}
	if (output_) {
		// Never release an output that is still writing: stop it AND wait for
		// the muxer to finish the file (matters when the app quits
		// mid-recording). The stop signal still reaches onStopSignal, so the
		// recording is finalized as usual.
		stopOutputAndWait(output_);
		signal_handler_t *sh = obs_output_get_signal_handler(output_);
		if (sh) {
			signal_handler_disconnect(sh, "start", &RecordingController::onStartSignal, this);
			signal_handler_disconnect(sh, "stop", &RecordingController::onStopSignal, this);
			if (splitEnabled_)
				signal_handler_disconnect(sh, "file_changed",
						  &RecordingController::onFileChangedSignal, this);
		}
		obs_output_release(output_);
		output_ = nullptr;
	}
	if (videoEncoder_) {
		obs_encoder_release(videoEncoder_);
		videoEncoder_ = nullptr;
	}
	if (audioEncoder_) {
		obs_encoder_release(audioEncoder_);
		audioEncoder_ = nullptr;
	}
	splitEnabled_ = false;
}

bool RecordingController::start(const Preset &preset, const std::string &fullFilePath,
				const AutoSplitSettings &requestedSplit)
{
	if (isRecording())
		return false;
	const AutoSplitSettings split = auto_split_settings::validated(requestedSplit);

	teardown(); // release any previous session's objects

	lastStopCode_ = 0;
	lastStopError_.clear();
	{
		std::lock_guard<std::mutex> lock(recordedFilesMutex_);
		recordedFiles_.clear();
	}
	activePreset_ = preset;
	currentFilePath_ = fullFilePath;
	usesFfmpegOutput_ = EncoderFactory::usesFfmpegOutput(preset);
	if (split.enabled && (usesFfmpegOutput_ ||
			      !modeUsesVideoEncoder(recordModeFromInt(preset.captureMode)))) {
		lastStopError_ = "Auto Split supports MP4, MKV, MOV and AVI screen recordings. "
				 "Turn off Auto Split for Audio Only or GIF recording.";
		return false;
	}

	// Audio Only: no output, no encoders, no video graph -- a tap straight on
	// the audio mix. libobs will not start an FFmpeg output without a video
	// encoder whatever is attached to it (the flag is registered by the
	// plugin, not derived from the wiring), so this mode has to go around it.
	if (!modeUsesVideoEncoder(recordModeFromInt(preset.captureMode))) {
		tap_ = std::make_unique<AudioOnlyRecorder>();
		tap_->onFinished = [this](const std::string &path, bool ok, const std::string &why) {
			lastStopCode_ = ok ? 0 : -1;
			lastStopError_ = ok ? std::string() : why;
			// The path can differ from the one asked for: a failed encode
			// leaves the captured WAV behind rather than nothing, and the
			// user should be shown the file that exists.
			currentFilePath_ = path;
			{
				std::lock_guard<std::mutex> lock(recordedFilesMutex_);
				recordedFiles_ = {path};
			}
			if (onFinished)
				onFinished(path);
		};

		std::string err;
		if (!tap_->start(fullFilePath, preset.audioBitrateKbps > 0 ? preset.audioBitrateKbps : 160,
				 &err)) {
			blog(LOG_ERROR, "[harpia] audio-only recording failed to start: %s", err.c_str());
			lastStopError_ = err;
			tap_.reset();
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(recordedFilesMutex_);
			recordedFiles_ = {fullFilePath};
		}
		if (onStarted)
			onStarted();
		return true;
	}

	// No video engine, no video recording. When libobs could not start its
	// graphics (the log says "obs_reset_video failed"), the capture source has
	// no textures and the encoders no frames, and starting an output on top of
	// that crashed the app. Refuse, and say why.
	{
		struct obs_video_info probe;
		if (!obs_get_video_info(&probe)) {
			// The reason obs_reset_video gave, when we have it: this used to
			// blame the graphics DLL every time, which sent people rebuilding
			// for a canvas the GPU had simply refused.
			const std::string why = lastVideoResetError().empty()
							? describeVideoResetError(-5, 0, 0)
							: lastVideoResetError();
			lastStopError_ = "The video engine is not running, so nothing can be recorded.\n\n" + why;
			blog(LOG_ERROR, "[harpia] recording refused: no video engine (%s)", why.c_str());
			return false;
		}
	}

	const std::string outputId = EncoderFactory::outputId(preset);
	output_ = obs_output_create(outputId.c_str(), "harpia_file_output", nullptr, nullptr);
	if (!output_) {
		blog(LOG_ERROR, "[harpia] failed to create output '%s'", outputId.c_str());
		return false;
	}

	if (usesFfmpegOutput_) {
		// GIF / exotic formats: ffmpeg_output resolves the muxer from
		// format_name and encodes via libav directly (no obs_encoder objects).
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "url", fullFilePath.c_str());
		obs_data_set_string(settings, "format_name", formatToString(preset.format));
		obs_data_set_string(settings, "video_encoder", "gif");

		// GIFs are for short clips: downscale to keep the file sane. Scale the
		// canvas down so the longest edge is at most 640px, preserving aspect.
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi) && ovi.output_width > 0 && ovi.output_height > 0) {
			const uint32_t maxEdge = 640;
			uint32_t w = ovi.output_width;
			uint32_t h = ovi.output_height;
			if (w > maxEdge || h > maxEdge) {
				if (w >= h) {
					h = (uint32_t)((uint64_t)h * maxEdge / w);
					w = maxEdge;
				} else {
					w = (uint32_t)((uint64_t)w * maxEdge / h);
					h = maxEdge;
				}
			}
			obs_data_set_int(settings, "scale_width", w & ~1u);
			obs_data_set_int(settings, "scale_height", h & ~1u);
		}

		obs_output_update(output_, settings);
		obs_data_release(settings);

		// No audio track for GIF.
		obs_output_set_media(output_, obs_get_video(), nullptr);
	} else {
		// MP4 / MKV / MOV / AVI: encoded muxer recording (supports pause).
		std::string vid = EncoderFactory::videoEncoderId(preset);
		if (vid.empty())
			vid = "obs_x264"; // last-resort baseline
		const std::string aid = EncoderFactory::audioEncoderId(preset);

		// Audio Only: no video encoder is created and none is attached, so the
		// muxer writes a single AAC track. This is why the two encoders are set
		// on separate calls below rather than in one helper -- an audio-only
		// output is a real libobs configuration, not a video output with the
		// picture blanked, and it never touches the GPU encoder at all.
		const bool audioOnly = !modeUsesVideoEncoder(recordModeFromInt(preset.captureMode));

		if (!audioOnly) {
			obs_data_t *vsettings = obs_data_create();
			// Constant quality on "Auto" bitrate, CBR when the user set a number.
			EncoderFactory::applyRecordingQuality(vsettings, vid, preset.videoBitrateKbps);
			videoEncoder_ =
				obs_video_encoder_create(vid.c_str(), "harpia_video", vsettings, nullptr);
			obs_data_release(vsettings);
		}

		obs_data_t *asettings = obs_data_create();
		obs_data_set_int(asettings, "bitrate", preset.audioBitrateKbps > 0 ? preset.audioBitrateKbps : 160);
		audioEncoder_ = obs_audio_encoder_create(aid.c_str(), "harpia_audio", asettings, 0, nullptr);
		obs_data_release(asettings);

		if ((!audioOnly && !videoEncoder_) || !audioEncoder_) {
			blog(LOG_ERROR, "[harpia] failed to create encoders (video='%s' audio='%s')", vid.c_str(),
			     aid.c_str());
			teardown();
			return false;
		}

		if (videoEncoder_)
			obs_encoder_set_video(videoEncoder_, obs_get_video());
		obs_encoder_set_audio(audioEncoder_, obs_get_audio());

		if (videoEncoder_)
			obs_output_set_video_encoder(output_, videoEncoder_);
		obs_output_set_audio_encoder(output_, audioEncoder_, 0);

		obs_data_t *osettings = obs_data_create();
		obs_data_set_string(osettings, "path", fullFilePath.c_str());
		if (split.enabled) {
			// The caller gives each take its own hidden directory. A short,
			// literal internal stem avoids the OBS formatter interpreting '%'
			// in user names or truncating a long generated filename. The GUI
			// assigns numbered, user-facing names after the take stops.
			const size_t slash = fullFilePath.find_last_of("/\\");
			const size_t dot = fullFilePath.find_last_of('.');
			if (slash == std::string::npos || dot == std::string::npos || dot <= slash) {
				lastStopError_ = "Auto Split requires a recording path with a folder and extension.";
				obs_data_release(osettings);
				teardown();
				return false;
			}
			obs_data_set_bool(osettings, "split_file", true);
			obs_data_set_int(osettings, "max_time_sec",
					 split.mode == AutoSplitSettings::Mode::Time
						 ? int64_t(std::max(1, split.minutes)) * 60
						 : 0);
			obs_data_set_int(osettings, "max_size_mb",
					 split.mode == AutoSplitSettings::Mode::Size
						 ? std::max(1, split.sizeMB)
						 : 0);
			obs_data_set_string(osettings, "directory", fullFilePath.substr(0, slash).c_str());
			obs_data_set_string(osettings, "format", "part");
			obs_data_set_string(osettings, "extension", fullFilePath.substr(dot + 1).c_str());
			obs_data_set_bool(osettings, "allow_spaces", true);
			obs_data_set_bool(osettings, "allow_overwrite", false);
			splitEnabled_ = true;
		}
		obs_output_update(output_, osettings);
		obs_data_release(osettings);
	}

	signal_handler_t *sh = obs_output_get_signal_handler(output_);
	signal_handler_connect(sh, "start", &RecordingController::onStartSignal, this);
	signal_handler_connect(sh, "stop", &RecordingController::onStopSignal, this);
	if (splitEnabled_)
		signal_handler_connect(sh, "file_changed", &RecordingController::onFileChangedSignal, this);
	{
		std::lock_guard<std::mutex> lock(recordedFilesMutex_);
		recordedFiles_ = {fullFilePath};
	}

	if (!obs_output_start(output_)) {
		const char *err = obs_output_get_last_error(output_);
		blog(LOG_ERROR, "[harpia] obs_output_start failed: %s", err ? err : "(unknown)");
		lastStopError_ = err ? err : "The recording output could not start.";
		teardown();
		{
			std::lock_guard<std::mutex> lock(recordedFilesMutex_);
			recordedFiles_.clear();
		}
		return false;
	}

	return true;
}

std::vector<std::string> RecordingController::recordedFiles() const
{
	std::lock_guard<std::mutex> lock(recordedFilesMutex_);
	return recordedFiles_;
}

void RecordingController::onFileChangedSignal(void *data, calldata_t *cd)
{
	auto *self = static_cast<RecordingController *>(data);
	const char *next = calldata_string(cd, "next_file");
	if (!next || !*next)
		return;
	const std::string path(next);
	{
		std::lock_guard<std::mutex> lock(self->recordedFilesMutex_);
		if (self->recordedFiles_.empty() || self->recordedFiles_.back() != path)
			self->recordedFiles_.push_back(path);
	}
	if (self->onFileChanged)
		self->onFileChanged(path);
}

void RecordingController::stop()
{
	if (tap_) {
		tap_->requestStop(); // asynchronous; onFinished fires once encoded
		return;
	}
	if (output_ && obs_output_active(output_))
		obs_output_stop(output_);
}

void RecordingController::forceStop()
{
	if (tap_) {
		// There is nothing to abort: the tap's stop path is a drain and an
		// encode, both bounded. Asking it to stop is the fastest exit there is.
		tap_->requestStop();
		return;
	}
	if (output_ && obs_output_active(output_)) {
		blog(LOG_WARNING, "[harpia] force-stopping the recording output");
		obs_output_force_stop(output_);
	}
}

bool RecordingController::canPause() const
{
	if (tap_)
		return true; // pausing a tap is just dropping what arrives
	if (!output_ || usesFfmpegOutput_)
		return false;
	return (obs_output_get_flags(output_) & OBS_OUTPUT_CAN_PAUSE) != 0;
}

bool RecordingController::pause(bool paused)
{
	if (tap_) {
		tap_->setPaused(paused);
		return tap_->paused();
	}
	if (!canPause())
		return false;
	obs_output_pause(output_, paused);
	return obs_output_paused(output_);
}

bool RecordingController::togglePause()
{
	return pause(!isPaused());
}

bool RecordingController::isRecording() const
{
	if (tap_)
		return tap_->active();
	return output_ && obs_output_active(output_);
}

bool RecordingController::isFinishing() const
{
	return tap_ && tap_->active() && !tap_->capturing();
}

bool RecordingController::isPaused() const
{
	if (tap_)
		return tap_->paused();
	return output_ && obs_output_paused(output_);
}

void RecordingController::onStartSignal(void *data, calldata_t *)
{
	auto *self = static_cast<RecordingController *>(data);
	if (self->onStarted)
		self->onStarted();
}

void RecordingController::onStopSignal(void *data, calldata_t *cd)
{
	auto *self = static_cast<RecordingController *>(data);

	// Capture WHY the output stopped. 0 == OBS_OUTPUT_SUCCESS; anything else
	// (disk full, write error, encoder failure) must not be reported to the
	// user as a clean save.
	const int code = (int)calldata_int(cd, "code");
	self->lastStopCode_ = code;
	self->lastStopError_.clear();
	if (code != 0) {
		const char *err = self->output_ ? obs_output_get_last_error(self->output_) : nullptr;
		self->lastStopError_ = err ? err : "";
		blog(LOG_ERROR, "[harpia] recording stopped with error code %d: %s", code,
		     err ? err : "(no detail)");
	}

	if (self->onFinished)
		self->onFinished(self->currentFilePath_);
}

} // namespace harpia
