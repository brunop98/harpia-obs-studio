#include <AvailabilityMacros.h>
#include <Cocoa/Cocoa.h>

#include <stdlib.h>
#include <obs-module.h>
#include <util/threading.h>
#include <pthread.h>
#include <dispatch/dispatch.h>
#include <dispatch/dispatch.h>

#include <IOSurface/IOSurface.h>
#include <ScreenCaptureKit/ScreenCaptureKit.h>
#include <CoreMedia/CMSampleBuffer.h>
#include <CoreVideo/CVPixelBuffer.h>

#define MACCAP_LOG(level, msg, ...) blog(level, "[ mac-screencapture ]: " msg, ##__VA_ARGS__)
#define MACCAP_ERR(msg, ...)        MACCAP_LOG(LOG_ERROR, msg, ##__VA_ARGS__)

typedef enum {
    ScreenCaptureDisplayStream = 0,
    ScreenCaptureWindowStream = 1,
    ScreenCaptureApplicationStream = 2,
} ScreenCaptureStreamType;

typedef enum {
    ScreenCaptureAudioDesktopStream = 0,
    ScreenCaptureAudioApplicationStream = 1,
} ScreenCaptureAudioStreamType;

API_AVAILABLE(macos(12.5)) typedef SCDisplay *SCDisplayRef;

API_AVAILABLE(macos(12.5))
@interface ScreenCaptureDelegate : NSObject <SCStreamOutput, SCStreamDelegate>

@property struct screen_capture *sc;

@end

struct API_AVAILABLE(macos(12.5)) screen_capture {
    obs_source_t *source;

    gs_effect_t *effect;
    gs_texture_t *tex;

    NSRect frame;
    bool hide_cursor;
    bool hide_obs;
    bool show_hidden_windows;
    bool show_empty_names;
    bool audio_only;
    bool capture_audio;

    // Harpia excludes recorder controls by native window ID, rather than
    // hiding its entire application (which would also hide mouse effects).
    NSArray<NSNumber *> *excluded_window_ids;
    uint64_t exclusion_revision;

    // Read by the application without rebuilding source properties. The
    // ScreenCaptureKit delegate writes these from a separate callback queue.
    volatile bool harpia_stream_running;
    volatile bool harpia_frame_received;
    volatile bool harpia_stream_failed;
    volatile bool harpia_filter_pending;
    dispatch_queue_t harpia_video_queue;
    bool audio_exclude_application;
    volatile bool audio_routing_pending;
    volatile bool audio_routing_shutdown;
    volatile long audio_routing_revision;
    NSArray *audio_app_observers;
    dispatch_queue_t audio_routing_queue;

    SCStream *disp;
    SCStreamConfiguration *stream_properties;
    SCShareableContent *shareable_content;
    ScreenCaptureDelegate *capture_delegate;

    os_event_t *stream_start_completed;
    os_sem_t *shareable_content_available;
    IOSurfaceRef current, prev;
    bool capture_failed;

    pthread_mutex_t mutex;

    ScreenCaptureStreamType capture_type;
    ScreenCaptureAudioStreamType audio_capture_type;
    CGDirectDisplayID display;
    CGWindowID window;
    NSString *application_id;
};

bool is_screen_capture_available(void);

API_AVAILABLE(macos(12.5)) void screen_capture_build_content_list(struct screen_capture *sc, bool display_capture);

API_AVAILABLE(macos(12.5)) bool build_display_list(struct screen_capture *sc, obs_properties_t *props);

API_AVAILABLE(macos(12.5)) bool build_window_list(struct screen_capture *sc, obs_properties_t *props);

API_AVAILABLE(macos(12.5)) bool build_application_list(struct screen_capture *sc, obs_properties_t *props);

static const char *screen_capture_getname(void *unused __unused);

API_AVAILABLE(macos(12.5)) void screen_stream_video_update(struct screen_capture *sc, CMSampleBufferRef sample_buffer);

API_AVAILABLE(macos(12.5)) void screen_stream_audio_update(struct screen_capture *sc, CMSampleBufferRef sample_buffer);

API_AVAILABLE(macos(12.5)) void harpia_capture_status(void *data, calldata_t *parameters);
