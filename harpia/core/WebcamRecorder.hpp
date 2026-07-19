#pragma once

#include <string>
#include <vector>

struct obs_source;
typedef struct obs_source obs_source_t;
struct obs_view;
typedef struct obs_view obs_view_t;
struct obs_encoder;
typedef struct obs_encoder obs_encoder_t;
struct obs_output;
typedef struct obs_output obs_output_t;
struct video_output;
typedef struct video_output video_t;

namespace harpia {

struct AudioDevice; // reuse the {id,name} shape from AudioManager

// Records a webcam as an INDEPENDENT, synchronized video file — never composited
// into the screen recording. It builds a private obs_view holding only the camera
// source and a separate video mix (obs_view_add2) at the camera's own
// resolution/fps, then drives its own ffmpeg_muxer output. Started/stopped
// alongside the main screen recording; both share the libobs clock so the two
// files stay in sync.
//
// This is the first of what can become several independent "tracks" (mic-only,
// second camera, game capture…) recorded to their own files.
class WebcamRecorder {
public:
	WebcamRecorder() = default;
	~WebcamRecorder();

	WebcamRecorder(const WebcamRecorder &) = delete;
	WebcamRecorder &operator=(const WebcamRecorder &) = delete;

	// Available camera devices (id + human name). Requires modules loaded.
	static std::vector<AudioDevice> cameras();

	// Whether the platform camera source is registered at all. False means the
	// capture plugin (e.g. win-dshow) wasn't built/loaded — distinct from "a
	// camera plugin is present but no device is plugged in".
	static bool supported();

	// The platform camera source id (dshow_input / macos-avcapture / v4l2_input).
	static const char *platformCameraId();

	// Start recording the given camera to `filePath`. deviceId "" uses the first
	// device. Returns false if no camera is available or setup fails.
	//
	// If `sharedSource` is non-null, that already-open camera source is reused
	// (e.g. the live toolbar preview's source) instead of opening the device a
	// second time — DirectShow cameras are typically exclusive. The recorder
	// takes its own reference and never destroys the shared source.
	bool start(const std::string &deviceId, int width, int height, int fps, const std::string &filePath,
		   obs_source_t *sharedSource = nullptr);

	// Request the recording to stop. Asynchronous: the output keeps finalizing
	// in the background; call reap() periodically to release it once done.
	// (Only tears down immediately when the output is already inactive.)
	void stop();

	// Release the output once an async stop() has fully finished — safe to call
	// every UI tick; does nothing while the muxer is still writing.
	void reap();

	bool isRecording() const;

	// Pause/resume in lockstep with the screen recording, so the two files stay
	// the same length and in sync (a paused screen file must not keep growing a
	// webcam companion).
	void pause(bool paused);

private:
	void teardown();

	obs_source_t *camera_ = nullptr;
	obs_view_t *view_ = nullptr;
	video_t *video_ = nullptr; // the independent mix
	obs_encoder_t *videoEncoder_ = nullptr;
	obs_output_t *output_ = nullptr;
	bool stopRequested_ = false; // async stop() issued; reap() finishes it
};

} // namespace harpia
