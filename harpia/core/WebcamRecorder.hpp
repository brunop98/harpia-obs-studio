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

	// The platform camera source id (dshow_input / macos-avcapture / v4l2_input).
	static const char *platformCameraId();

	// Start recording the given camera to `filePath`. deviceId "" uses the first
	// device. Returns false if no camera is available or setup fails.
	bool start(const std::string &deviceId, int width, int height, int fps, const std::string &filePath);

	void stop();
	bool isRecording() const;

private:
	void teardown();

	obs_source_t *camera_ = nullptr;
	obs_view_t *view_ = nullptr;
	video_t *video_ = nullptr; // the independent mix
	obs_encoder_t *videoEncoder_ = nullptr;
	obs_output_t *output_ = nullptr;
};

} // namespace harpia
