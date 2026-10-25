#include "mac-sck-common.h"

const char *sck_audio_capture_getname(void *unused __unused)
{
    return obs_module_text("SCK.Audio.Name");
}

API_AVAILABLE(macos(13.0)) static void destroy_audio_screen_stream(struct screen_capture *sc)
{
    SCStream *old_stream;
    bool failed;
    @synchronized(sc->capture_delegate) {
        old_stream = sc->disp;
        sc->disp = NULL; // late callbacks from this stream are now stale
        failed = sc->capture_failed;
        os_atomic_store_bool(&sc->harpia_stream_running, false);
    }
    if (old_stream && !failed) {
        [old_stream stopCaptureWithCompletionHandler:^(NSError *_Nullable error) {
            if (error && error.code != SCStreamErrorAttemptToStopStreamState) {
                MACCAP_ERR("destroy_audio_screen_stream: Failed to stop stream with error %s\n",
                           [[error localizedFailureReason] cStringUsingEncoding:NSUTF8StringEncoding]);
            }
        }];
    }
    [old_stream release];

    if (sc->stream_properties) {
        [sc->stream_properties release];
        sc->stream_properties = NULL;
    }

    os_event_destroy(sc->stream_start_completed);
    sc->stream_start_completed = NULL;
}

static char audio_routing_queue_key;

// obs_shutdown forcibly destroys any surviving source even when references
// remain. The frontend calls this proc while it still owns its source, before
// releasing that ownership or shutting OBS down. No job can enqueue after the
// drain because notification dispatch and this guard use the same monitor.
API_AVAILABLE(macos(13.0)) static void prepare_audio_shutdown(void *data, calldata_t *parameters)
{
    (void)parameters;
    struct screen_capture *sc = data;
    if (!sc)
        return;
    NSArray *observers = nil;
    dispatch_queue_t queue = NULL;
    @synchronized(sc->capture_delegate) {
        os_atomic_store_bool(&sc->audio_routing_shutdown, true);
        os_atomic_store_bool(&sc->audio_routing_pending, true);
        sc->capture_delegate.sc = NULL;
        observers = sc->audio_app_observers;
        sc->audio_app_observers = nil;
        queue = sc->audio_routing_queue;
    }
    NSNotificationCenter *center = NSWorkspace.sharedWorkspace.notificationCenter;
    for (id observer in observers)
        [center removeObserver:observer];
    [observers release];
    if (queue && dispatch_get_specific(&audio_routing_queue_key) != sc)
        dispatch_sync(queue, ^{});
}

API_AVAILABLE(macos(13.0)) static void sck_audio_capture_destroy(void *data)
{
    struct screen_capture *sc = data;

    if (!sc)
        return;

    prepare_audio_shutdown(sc, NULL);
    if (sc->audio_routing_queue) {
        dispatch_release(sc->audio_routing_queue);
        sc->audio_routing_queue = NULL;
    }
    destroy_audio_screen_stream(sc);

    if (sc->shareable_content_available) {
        // Wait even if content is currently nil: a refresh callback may still
        // own this semaphore and must finish before its source is released.
        os_sem_wait(sc->shareable_content_available);
        [sc->shareable_content release];
        sc->shareable_content = nil;
        os_sem_destroy(sc->shareable_content_available);
        sc->shareable_content_available = NULL;
    }

    if (sc->capture_delegate) {
        [sc->capture_delegate release];
    }
    [sc->application_id release];

    pthread_mutex_destroy(&sc->mutex);
    bfree(sc);
}

API_AVAILABLE(macos(13.0)) static bool init_audio_screen_stream(struct screen_capture *sc)
{
    SCContentFilter *content_filter;
    const long routing_revision = os_atomic_load_long(&sc->audio_routing_revision);
    os_atomic_store_bool(&sc->harpia_stream_running, false);
    os_atomic_store_bool(&sc->harpia_stream_failed, false);
    os_atomic_store_bool(&sc->audio_routing_pending, true);
    if (sc->capture_failed) {
        sc->capture_failed = false;
        obs_source_update_properties(sc->source);
    }

    sc->stream_properties = [[SCStreamConfiguration alloc] init];
    os_sem_wait(sc->shareable_content_available);

    SCDisplayRef (^get_target_display)(void) = ^SCDisplayRef {
        for (SCDisplay *display in sc->shareable_content.displays) {
            if (display.displayID == sc->display) {
                return display;
            }
        }
        return nil;
    };

    switch (sc->audio_capture_type) {
        case ScreenCaptureAudioDesktopStream: {
            SCDisplay *target_display = get_target_display();
            if (!target_display) {
                MACCAP_ERR("init_audio_screen_stream: No display available for system audio capture\n");
                os_sem_post(sc->shareable_content_available);
                return false;
            }

            NSArray *empty = [[NSArray alloc] init];
            content_filter = [[SCContentFilter alloc] initWithDisplay:target_display excludingWindows:empty];
            [empty release];
        } break;
        case ScreenCaptureAudioApplicationStream: {
            SCDisplay *target_display = get_target_display();
            if (!target_display) {
                MACCAP_ERR("init_audio_screen_stream: No display available for application audio capture\n");
                os_sem_post(sc->shareable_content_available);
                return false;
            }
            NSMutableArray *target_application_array = [[NSMutableArray alloc] init];
            for (SCRunningApplication *application in sc->shareable_content.applications) {
                if (application.bundleIdentifier.length &&
                    [application.bundleIdentifier caseInsensitiveCompare:sc->application_id] == NSOrderedSame)
                    [target_application_array addObject:application];
            }
            bool application_running = false;
            for (NSRunningApplication *running in NSWorkspace.sharedWorkspace.runningApplications) {
                if (running.bundleIdentifier.length &&
                    [running.bundleIdentifier caseInsensitiveCompare:sc->application_id] == NSOrderedSame) {
                    application_running = true;
                    break;
                }
            }
            if (target_application_array.count == 0 && (!sc->audio_exclude_application || application_running)) {
                MACCAP_ERR("init_audio_screen_stream: Application '%s' is not running\n",
                           sc->application_id.UTF8String);
                [target_application_array release];
                os_sem_post(sc->shareable_content_available);
                return false;
            }
            NSArray *empty = [[NSArray alloc] init];
            if (sc->audio_exclude_application)
                content_filter = [[SCContentFilter alloc] initWithDisplay:target_display
                                                   excludingApplications:target_application_array
                                                        exceptingWindows:empty];
            else
                content_filter = [[SCContentFilter alloc] initWithDisplay:target_display
                                                   includingApplications:target_application_array
                                                        exceptingWindows:empty];
            [target_application_array release];
            [empty release];
        } break;
    }
    os_sem_post(sc->shareable_content_available);
    [sc->stream_properties setQueueDepth:3];
    // SCK requires a screen output alongside audio; those frames are dropped.
    // Keep that dummy stream tiny and infrequent rather than rendering a full
    // desktop at 60 fps just to record its sound.
    [sc->stream_properties setWidth:2];
    [sc->stream_properties setHeight:2];
    [sc->stream_properties setMinimumFrameInterval:CMTimeMake(1, 1)];

    [sc->stream_properties setCapturesAudio:TRUE];
    [sc->stream_properties setExcludesCurrentProcessAudio:TRUE];
    [sc->stream_properties setShowsCursor:FALSE];

    struct obs_audio_info audio_info;
    BOOL did_get_audio_info = obs_get_audio_info(&audio_info);
    if (!did_get_audio_info) {
        MACCAP_ERR("init_audio_screen_stream: No audio configured, returning %d\n", did_get_audio_info);
        [content_filter release];
        return did_get_audio_info;
    }
    [sc->stream_properties setSampleRate:audio_info.samples_per_sec];
    int channel_count = get_audio_channels(audio_info.speakers);
    if (channel_count > 1) {
        [sc->stream_properties setChannelCount:2];
    } else {
        [sc->stream_properties setChannelCount:channel_count];
    }

    sc->disp = [[SCStream alloc] initWithFilter:content_filter configuration:sc->stream_properties
                                       delegate:sc->capture_delegate];
    [content_filter release];

    //add a dummy video stream output to silence errors from SCK. frames are dropped by the delegate
    NSError *error = nil;
    BOOL did_add_output = [sc->disp addStreamOutput:sc->capture_delegate type:SCStreamOutputTypeScreen
                                 sampleHandlerQueue:nil
                                              error:&error];
    if (!did_add_output) {
        MACCAP_ERR("init_audio_screen_stream: Failed to add video stream output with error %s\n",
                   [[error localizedFailureReason] cStringUsingEncoding:NSUTF8StringEncoding]);
        [sc->disp release];
        sc->disp = NULL;
        return false;
    }

    did_add_output = [sc->disp addStreamOutput:sc->capture_delegate type:SCStreamOutputTypeAudio sampleHandlerQueue:nil
                                         error:&error];
    if (!did_add_output) {
        MACCAP_ERR("init_audio_screen_stream: Failed to add audio stream output with error %s\n",
                   [[error localizedFailureReason] cStringUsingEncoding:NSUTF8StringEncoding]);
        [sc->disp release];
        sc->disp = NULL;
        return false;
    }
    os_event_init(&sc->stream_start_completed, OS_EVENT_TYPE_MANUAL);

    __block BOOL did_stream_start = false;
    [sc->disp startCaptureWithCompletionHandler:^(NSError *_Nullable error2) {
        did_stream_start = (BOOL) (error2 == nil && !os_atomic_load_bool(&sc->harpia_stream_failed));
        os_atomic_store_bool(&sc->harpia_stream_running, did_stream_start);
        os_atomic_store_bool(&sc->harpia_stream_failed, !did_stream_start);
        // A newer app launch/exit may have arrived during stream startup.
        // Its queued refresh must complete before this older filter can emit.
        os_atomic_store_bool(&sc->audio_routing_pending,
                             !did_stream_start || routing_revision != os_atomic_load_long(&sc->audio_routing_revision));
        if (!did_stream_start) {
            MACCAP_ERR("init_audio_screen_stream: Failed to start capture with error %s\n",
                       [[error localizedFailureReason] cStringUsingEncoding:NSUTF8StringEncoding]);
            // Clean up disp so it isn't stopped
            [sc->disp release];
            sc->disp = NULL;
        }
        os_event_signal(sc->stream_start_completed);
    }];
    os_event_wait(sc->stream_start_completed);

    return did_stream_start;
}

static void sck_audio_capture_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "application", NULL);
    obs_data_set_default_int(settings, "type", ScreenCaptureAudioDesktopStream);
    obs_data_set_default_bool(settings, "exclude_application", false);
}

// App filters contain native running-application identities. Rebuild them on
// launches and exits so a newly launched copy cannot escape an exclusion that
// was configured by persistent bundle ID. Stop publishing the old mix as soon
// as the notification arrives, before the asynchronous filter refresh.
API_AVAILABLE(macos(13.0)) static void watch_application_changes(struct screen_capture *sc)
{
    if (sc->audio_capture_type != ScreenCaptureAudioApplicationStream)
        return;
    sc->audio_routing_queue = dispatch_queue_create("com.harpia.obs.audio-routing", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(sc->audio_routing_queue, &audio_routing_queue_key, sc, NULL);
    ScreenCaptureDelegate *delegate = sc->capture_delegate;
    void (^changed)(NSNotification *) = ^(NSNotification *notification) {
        NSRunningApplication *application = notification.userInfo[NSWorkspaceApplicationKey];
        @synchronized(delegate) {
            struct screen_capture *capture = delegate.sc;
            if (!capture || os_atomic_load_bool(&capture->audio_routing_shutdown) ||
                capture->audio_capture_type != ScreenCaptureAudioApplicationStream ||
                !application.bundleIdentifier.length ||
                [application.bundleIdentifier caseInsensitiveCompare:capture->application_id] != NSOrderedSame)
                return;
            obs_source_t *source = obs_source_get_ref(capture->source);
            if (!source)
                return;
            os_atomic_inc_long(&capture->audio_routing_revision);
            os_atomic_store_bool(&capture->audio_routing_pending, true);
            // Enqueue while the delegate monitor is held. prepare_shutdown
            // cannot drain and then be overtaken by an already-notified job.
            dispatch_async(capture->audio_routing_queue, ^{
                if (!os_atomic_load_bool(&capture->audio_routing_shutdown)) {
                    obs_data_t *settings = obs_source_get_settings(source);
                    obs_source_update(source, settings);
                    obs_data_release(settings);
                }
                obs_source_release(source);
            });
        }
    };
    NSNotificationCenter *center = NSWorkspace.sharedWorkspace.notificationCenter;
    id launched = [center addObserverForName:NSWorkspaceDidLaunchApplicationNotification object:nil queue:nil usingBlock:changed];
    id terminated = [center addObserverForName:NSWorkspaceDidTerminateApplicationNotification object:nil queue:nil usingBlock:changed];
    sc->audio_app_observers = [@[launched, terminated] retain];
}

// Called by Harpia after obs_source_create returns and publishes its native
// context. Observers cannot schedule a skipped update during create(), and a
// serial refresh closes the gap between the first content snapshot and observer
// registration before the frontend connects the source to the audio graph.
API_AVAILABLE(macos(13.0)) static void begin_audio_tracking(void *data, calldata_t *parameters)
{
    (void)parameters;
    struct screen_capture *sc = data;
    if (!sc || os_atomic_load_bool(&sc->audio_routing_shutdown) || sc->audio_routing_queue)
        return;
    watch_application_changes(sc);
    if (!sc->audio_routing_queue)
        return;
    void (^refresh)(void) = ^{
        if (!os_atomic_load_bool(&sc->audio_routing_shutdown)) {
            obs_data_t *settings = obs_source_get_settings(sc->source);
            obs_source_update(sc->source, settings);
            obs_data_release(settings);
        }
    };
    if (dispatch_get_specific(&audio_routing_queue_key) == sc)
        refresh();
    else
        dispatch_sync(sc->audio_routing_queue, refresh);
}

API_AVAILABLE(macos(13.0)) static void *sck_audio_capture_create(obs_data_t *settings, obs_source_t *source)
{
    struct screen_capture *sc = bzalloc(sizeof(struct screen_capture));

    sc->source = source;
    sc->audio_only = true;
    sc->audio_capture_type = (unsigned int) obs_data_get_int(settings, "type");
    if (sc->audio_capture_type != ScreenCaptureAudioDesktopStream &&
        sc->audio_capture_type != ScreenCaptureAudioApplicationStream) {
        bfree(sc);
        return NULL;
    }
    sc->audio_exclude_application = obs_data_get_bool(settings, "exclude_application");

    os_sem_init(&sc->shareable_content_available, 1);
    screen_capture_build_content_list(sc, sc->audio_capture_type == ScreenCaptureAudioDesktopStream);

    sc->capture_delegate = [[ScreenCaptureDelegate alloc] init];
    sc->capture_delegate.sc = sc;

    sc->display = CGMainDisplayID();

    sc->application_id = [[NSString alloc] initWithUTF8String:obs_data_get_string(settings, "application")];
    pthread_mutex_init(&sc->mutex, NULL);

    if (!init_audio_screen_stream(sc))
        goto fail;
    proc_handler_add(obs_source_get_proc_handler(source),
                     "void harpia_capture_status(out bool ready, out bool failed)", harpia_capture_status, sc);
    proc_handler_add(obs_source_get_proc_handler(source),
                     "void harpia_prepare_shutdown()", prepare_audio_shutdown, sc);
    proc_handler_add(obs_source_get_proc_handler(source),
                     "void harpia_begin_app_tracking()", begin_audio_tracking, sc);
    return sc;

fail:
    sck_audio_capture_destroy(sc);
    return NULL;
}

#pragma mark - obs_properties

API_AVAILABLE(macos(13.0))
static bool audio_capture_method_changed(void *data, obs_properties_t *props, obs_property_t *list __unused,
                                         obs_data_t *settings)
{
    struct screen_capture *sc = data;

    if (!sc)
        return false;
    unsigned int capture_type_id = (unsigned int) obs_data_get_int(settings, "type");
    obs_property_t *app_list = obs_properties_get(props, "application");

    switch (capture_type_id) {
        case ScreenCaptureAudioDesktopStream: {
            obs_property_set_visible(app_list, false);
            break;
        }
        case ScreenCaptureAudioApplicationStream: {
            obs_property_set_visible(app_list, true);
            screen_capture_build_content_list(sc, capture_type_id == ScreenCaptureAudioDesktopStream);
            build_application_list(sc, props);
            break;
        }
    }

    return true;
}

API_AVAILABLE(macos(13.0))
static bool reactivate_capture(obs_properties_t *props __unused, obs_property_t *property, void *data)
{
    struct screen_capture *sc = data;
    if (!sc || !sc->capture_failed) {
        MACCAP_LOG(LOG_WARNING, "Tried to reactivate capture that hadn't failed.");
        return false;
    }

    destroy_audio_screen_stream(sc);
    sc->capture_failed = false;
    bool started = init_audio_screen_stream(sc);
    if (!started) {
        sc->capture_failed = true;
        os_atomic_store_bool(&sc->harpia_stream_failed, true);
    }
    obs_property_set_enabled(property, !started);
    return true;
}

API_AVAILABLE(macos(13.0)) static obs_properties_t *sck_audio_capture_properties(void *data)
{
    struct screen_capture *sc = data;

    obs_properties_t *props = obs_properties_create();

    obs_property_t *capture_type = obs_properties_add_list(props, "type", obs_module_text("SCK.Method"),
                                                           OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

    obs_property_list_add_int(capture_type, obs_module_text("DesktopAudioCapture"), 0);
    obs_property_list_add_int(capture_type, obs_module_text("ApplicationAudioCapture"), 1);

    if (sc)
        obs_property_set_modified_callback2(capture_type, audio_capture_method_changed, data);

    obs_property_t *app_list = obs_properties_add_list(props, "application", obs_module_text("Application"),
                                                       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_t *reactivate =
        obs_properties_add_button2(props, "reactivate_capture", obs_module_text("SCK.Restart"), reactivate_capture, sc);
    obs_property_set_enabled(reactivate, sc && sc->capture_failed);

    if (sc) {
        switch (sc->audio_capture_type) {
            case 0: {
                obs_property_set_visible(app_list, false);
                break;
            }
            case 1: {
                obs_property_set_visible(app_list, true);
                break;
            }
        }
    }

    return props;
}

API_AVAILABLE(macos(13.0)) static void sck_audio_capture_update(void *data, obs_data_t *settings)
{
    struct screen_capture *sc = data;

    if (!sc || os_atomic_load_bool(&sc->audio_routing_shutdown))
        return;
    ScreenCaptureAudioStreamType capture_type = (ScreenCaptureAudioStreamType) obs_data_get_int(settings, "type");
    if (capture_type != ScreenCaptureAudioDesktopStream && capture_type != ScreenCaptureAudioApplicationStream)
        return;
    NSString *application_id = [[NSString alloc] initWithUTF8String:obs_data_get_string(settings, "application")];

    destroy_audio_screen_stream(sc);
    @synchronized(sc->capture_delegate) {
        sc->audio_capture_type = capture_type;
        sc->audio_exclude_application = obs_data_get_bool(settings, "exclude_application");
        [sc->application_id release];
        sc->application_id = application_id;
    }
    screen_capture_build_content_list(sc, sc->audio_capture_type == ScreenCaptureAudioDesktopStream);
    if (!init_audio_screen_stream(sc)) {
        sc->capture_failed = true;
        os_atomic_store_bool(&sc->harpia_stream_failed, true);
    }
}

#pragma mark - obs_source_info

API_AVAILABLE(macos(13.0))
struct obs_source_info sck_audio_capture_info = {
    .id = "sck_audio_capture",
    .type = OBS_SOURCE_TYPE_INPUT,
    .get_name = sck_audio_capture_getname,

    .create = sck_audio_capture_create,
    .destroy = sck_audio_capture_destroy,

    .output_flags = OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_AUDIO,

    .get_defaults = sck_audio_capture_defaults,
    .get_properties = sck_audio_capture_properties,
    .update = sck_audio_capture_update,
    .icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT,
};
