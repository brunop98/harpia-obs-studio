/* Apple hardware encoding through FFmpeg's VideoToolbox implementation.
 * Raw NV12 input keeps this independent of the libobs graphics backend. */
#include "obs-ffmpeg-video-encoders.h"

static bool h264_quality_available;
static bool hevc_quality_available;

struct vt_encoder {
	struct ffmpeg_video_encoder video;
	DARRAY(uint8_t) header;
};

static void vt_video_info(void *unused, struct video_scale_info *info)
{
	UNUSED_PARAMETER(unused);
	info->format = VIDEO_FORMAT_NV12;
}

static void vt_first_packet(void *data, AVPacket *packet, struct darray *out)
{
	struct vt_encoder *enc = data;
	AVCodecContext *context = enc->video.context;
	if (context->extradata_size > 0)
		da_copy_array(enc->header, context->extradata, context->extradata_size);
	for (int i = 0; i < packet->side_data_elems; ++i) {
		const AVPacketSideData *side = &packet->side_data[i];
		if (side->type == AV_PKT_DATA_NEW_EXTRADATA)
			da_copy_array(enc->header, side->data, side->size);
	}
	darray_copy_array(1, out, packet->data, packet->size);
}

static void vt_destroy(void *data)
{
	struct vt_encoder *enc = data;
	if (!enc)
		return;
	ffmpeg_video_encoder_free(&enc->video);
	da_free(enc->header);
	bfree(enc);
}

static void *vt_create(obs_data_t *settings, obs_encoder_t *encoder, const char *codec, const char *name)
{
	struct vt_encoder *enc = bzalloc(sizeof(*enc));
	if (!ffmpeg_video_encoder_init(&enc->video, enc, encoder, codec, NULL, name, NULL, vt_first_packet))
		goto fail;
	const struct video_output_info *voi = video_output_get_info(obs_encoder_video(encoder));
	if (voi->colorspace == VIDEO_CS_2100_HLG || voi->colorspace == VIDEO_CS_2100_PQ) {
		obs_encoder_set_last_error(encoder, "This Apple recording encoder requires SDR video. Choose SDR/Rec.709.");
		goto fail;
	}
	struct video_scale_info info = {.format = VIDEO_FORMAT_NV12, .colorspace = voi->colorspace,
				      .range = voi->range};
	const bool requested_quality = strcmp(obs_data_get_string(settings, "rate_control"), "QUALITY") == 0;
	const bool supported_quality = strcmp(codec, "h264_videotoolbox") == 0
		? h264_quality_available : hevc_quality_available;
	const bool quality = requested_quality && supported_quality;
	int bitrate = quality ? 0 : (int)obs_data_get_int(settings, "bitrate");
	if (requested_quality && !supported_quality) {
		const uint64_t pixels = (uint64_t)obs_encoder_get_width(encoder) * obs_encoder_get_height(encoder);
		uint64_t target = pixels * voi->fps_num * 9 / ((uint64_t)voi->fps_den * 100000);
		if (target < 2000) target = 2000;
		if (target > 50000) target = 50000;
		bitrate = (int)target;
		blog(LOG_INFO, "[harpia] %s constant-quality mode is unavailable; Auto uses a %d kbps target.", name, bitrate);
	}
	ffmpeg_video_encoder_update(&enc->video, bitrate, (int)obs_data_get_int(settings, "keyint_sec"),
				    voi, &info, "");
	AVCodecContext *context = enc->video.context;
	context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	context->max_b_frames = 0; // recording latency and exact pause/resume boundaries
	if (quality) {
		int value = (int)obs_data_get_int(settings, "quality");
		if (value < 1) value = 1;
		if (value > 100) value = 100;
		context->flags |= AV_CODEC_FLAG_QSCALE;
		context->global_quality = value * FF_QP2LAMBDA;
	}
	// Never present an expensive software fallback as Apple hardware encoding.
	if (av_opt_set_int(context->priv_data, "allow_sw", 0, 0) < 0 ||
	    av_opt_set_int(context->priv_data, "realtime", 1, 0) < 0) {
		obs_encoder_set_last_error(encoder, "FFmpeg's Apple hardware encoder is missing required options.");
		goto fail;
	}
	if (!ffmpeg_video_encoder_init_codec(&enc->video))
		goto fail;
	if (context->extradata_size > 0)
		da_copy_array(enc->header, context->extradata, context->extradata_size);
	return enc;
fail:
	vt_destroy(enc);
	return NULL;
}

static void *h264_vt_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	return vt_create(settings, encoder, "h264_videotoolbox", "Apple VideoToolbox H.264");
}

static void *hevc_vt_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	return vt_create(settings, encoder, "hevc_videotoolbox", "Apple VideoToolbox HEVC");
}

static bool vt_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet, bool *received)
{
	struct vt_encoder *enc = data;
	return ffmpeg_video_encode(&enc->video, frame, packet, received);
}

static bool vt_extra(void *data, uint8_t **bytes, size_t *size)
{
	struct vt_encoder *enc = data;
	*bytes = enc->header.array;
	*size = enc->header.num;
	return *size > 0;
}

static void vt_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "rate_control", "QUALITY");
	obs_data_set_default_int(settings, "quality", 65);
	obs_data_set_default_int(settings, "bitrate", 8000);
	obs_data_set_default_int(settings, "keyint_sec", 2);
}

static obs_properties_t *vt_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *properties = obs_properties_create();
	obs_property_t *mode = obs_properties_add_list(properties, "rate_control", "Rate control",
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, "Constant quality", "QUALITY");
	obs_property_list_add_string(mode, "Target bitrate", "ABR");
	obs_properties_add_int(properties, "quality", "Quality", 1, 100, 1);
	obs_properties_add_int(properties, "bitrate", "Bitrate (kbps)", 100, 300000, 100);
	obs_properties_add_int(properties, "keyint_sec", "Keyframe interval (seconds)", 1, 10, 1);
	return properties;
}

static const char *h264_vt_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Apple VideoToolbox H.264";
}
static const char *hevc_vt_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Apple VideoToolbox HEVC";
}

struct obs_encoder_info h264_videotoolbox_encoder_info = {
	.id = "ffmpeg_h264_videotoolbox", .type = OBS_ENCODER_VIDEO, .codec = "h264",
	.get_name = h264_vt_name, .create = h264_vt_create, .destroy = vt_destroy,
	.encode = vt_encode, .get_defaults = vt_defaults, .get_properties = vt_properties,
	.get_extra_data = vt_extra, .get_video_info = vt_video_info,
};
struct obs_encoder_info hevc_videotoolbox_encoder_info = {
	.id = "ffmpeg_hevc_videotoolbox", .type = OBS_ENCODER_VIDEO, .codec = "hevc",
	.get_name = hevc_vt_name, .create = hevc_vt_create, .destroy = vt_destroy,
	.encode = vt_encode, .get_defaults = vt_defaults, .get_properties = vt_properties,
	.get_extra_data = vt_extra, .get_video_info = vt_video_info,
};

// Probe a real hardware session, not just the existence of an FFmpeg wrapper.
// A machine without hardware HEVC must not advertise it in the preset picker.
static bool vt_probe(const char *name, bool quality)
{
	const AVCodec *codec = avcodec_find_encoder_by_name(name);
	if (!codec)
		return false;
	AVCodecContext *context = avcodec_alloc_context3(codec);
	if (!context)
		return false;
	context->width = 320;
	context->height = 180;
	context->pix_fmt = AV_PIX_FMT_NV12;
	context->time_base = (AVRational){1, 30};
	context->framerate = (AVRational){30, 1};
	context->bit_rate = quality ? 0 : 2000000;
	if (quality) {
		context->flags |= AV_CODEC_FLAG_QSCALE;
		context->global_quality = 65 * FF_QP2LAMBDA;
	}
	context->max_b_frames = 0;
	context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	const bool configured = av_opt_set_int(context->priv_data, "allow_sw", 0, 0) >= 0 &&
				av_opt_set_int(context->priv_data, "realtime", 1, 0) >= 0;
	const bool available = configured && avcodec_open2(context, codec, NULL) >= 0;
	avcodec_free_context(&context);
	return available;
}


bool harpia_videotoolbox_available(const char *name)
{
	if (!vt_probe(name, false))
		return false;
	const bool quality = vt_probe(name, true);
	if (strcmp(name, "h264_videotoolbox") == 0)
		h264_quality_available = quality;
	else if (strcmp(name, "hevc_videotoolbox") == 0)
		hevc_quality_available = quality;
	return true;
}
