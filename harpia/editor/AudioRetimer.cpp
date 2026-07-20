#include "AudioRetimer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

namespace harpia {

namespace {
bool fail(QString *err, const char *msg)
{
	if (err)
		*err = QString::fromUtf8(msg);
	return false;
}
} // namespace

AudioRetimer::~AudioRetimer()
{
	destroyGraph();
	if (fifo_)
		av_audio_fifo_free(fifo_);
	if (decFrame_)
		av_frame_free(&decFrame_);
	if (filtFrame_)
		av_frame_free(&filtFrame_);
	if (encFrame_)
		av_frame_free(&encFrame_);
	if (encPkt_)
		av_packet_free(&encPkt_);
	if (dec_)
		avcodec_free_context(&dec_);
	if (enc_)
		avcodec_free_context(&enc_);
}

int AudioRetimer::sampleRate() const
{
	return enc_ ? enc_->sample_rate : 0;
}

bool AudioRetimer::init(const AVCodecParameters *srcPar, int tbNum, int tbDen, bool globalHeader,
			QString *err)
{
	tbNum_ = tbNum > 0 ? tbNum : 1;
	tbDen_ = tbDen > 0 ? tbDen : 1000;

	// ---- Decoder ----
	const AVCodec *dc = avcodec_find_decoder(srcPar->codec_id);
	if (!dc)
		return fail(err, "No decoder for the source audio.");
	dec_ = avcodec_alloc_context3(dc);
	if (!dec_ || avcodec_parameters_to_context(dec_, srcPar) < 0)
		return fail(err, "Could not set up the audio decoder.");
	dec_->pkt_timebase = AVRational{tbNum_, tbDen_};
	if (avcodec_open2(dec_, dc, nullptr) < 0)
		return fail(err, "Could not open the audio decoder.");
	if (dec_->sample_rate <= 0 || dec_->ch_layout.nb_channels <= 0)
		return fail(err, "The source audio format is unknown.");

	// ---- AAC encoder ----
	const AVCodec *ec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (!ec)
		return fail(err, "The AAC encoder is not available in this build.");
	enc_ = avcodec_alloc_context3(ec);
	if (!enc_)
		return fail(err, "Could not allocate the audio encoder.");
	enc_->sample_rate = dec_->sample_rate;
	// The native AAC encoder consumes planar float; hardcoded on purpose (the
	// sample_fmts capability list is deprecated API on newer FFmpeg).
	enc_->sample_fmt = AV_SAMPLE_FMT_FLTP;
	// Mono stays mono; anything else becomes stereo (aformat converts).
	av_channel_layout_default(&enc_->ch_layout, dec_->ch_layout.nb_channels == 1 ? 1 : 2);
	enc_->time_base = AVRational{1, enc_->sample_rate};
	enc_->bit_rate = 192000;
	if (globalHeader)
		enc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	if (avcodec_open2(enc_, ec, nullptr) < 0)
		return fail(err, "Could not open the audio encoder.");

	// ---- Scratch buffers ----
	fifo_ = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, enc_->ch_layout.nb_channels,
				    std::max(1, enc_->frame_size) * 4);
	decFrame_ = av_frame_alloc();
	filtFrame_ = av_frame_alloc();
	encPkt_ = av_packet_alloc();
	encFrame_ = av_frame_alloc();
	if (!fifo_ || !decFrame_ || !filtFrame_ || !encPkt_ || !encFrame_)
		return fail(err, "Out of memory.");
	encFrame_->format = AV_SAMPLE_FMT_FLTP;
	encFrame_->nb_samples = enc_->frame_size;
	if (av_channel_layout_copy(&encFrame_->ch_layout, &enc_->ch_layout) < 0 ||
	    av_frame_get_buffer(encFrame_, 0) < 0)
		return fail(err, "Out of memory.");
	return true;
}

bool AudioRetimer::buildGraph(double speed, QString *err)
{
	destroyGraph();
	graph_ = avfilter_graph_alloc();
	if (!graph_)
		return fail(err, "Out of memory.");

	char srcLayout[64] = {0};
	char encLayout[64] = {0};
	av_channel_layout_describe(&dec_->ch_layout, srcLayout, sizeof(srcLayout));
	av_channel_layout_describe(&enc_->ch_layout, encLayout, sizeof(encLayout));

	char srcArgs[256];
	snprintf(srcArgs, sizeof(srcArgs),
		 "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s", tbNum_, tbDen_,
		 dec_->sample_rate, av_get_sample_fmt_name(dec_->sample_fmt), srcLayout);

	if (avfilter_graph_create_filter(&bufSrc_, avfilter_get_by_name("abuffer"), "in", srcArgs,
					 nullptr, graph_) < 0)
		return fail(err, "Could not create the audio buffer source.");
	if (avfilter_graph_create_filter(&bufSink_, avfilter_get_by_name("abuffersink"), "out", nullptr,
					 nullptr, graph_) < 0)
		return fail(err, "Could not create the audio buffer sink.");

	// atempo accepts [0.5, 100]; chain halvings for slow-motion below 0.5×.
	QByteArray spec;
	double t = std::clamp(speed, 0.05, 50.0);
	while (t < 0.5 - 1e-9) {
		spec += "atempo=0.5,";
		t /= 0.5;
	}
	spec += QByteArray("atempo=") + QByteArray::number(t, 'f', 6);
	char tail[192];
	snprintf(tail, sizeof(tail), ",aformat=sample_fmts=fltp:sample_rates=%d:channel_layouts=%s",
		 enc_->sample_rate, encLayout);
	spec += tail;

	AVFilterInOut *outputs = avfilter_inout_alloc(); // feeds the graph (source side)
	AVFilterInOut *inputs = avfilter_inout_alloc();  // reads the graph (sink side)
	if (!outputs || !inputs) {
		avfilter_inout_free(&outputs);
		avfilter_inout_free(&inputs);
		return fail(err, "Out of memory.");
	}
	outputs->name = av_strdup("in");
	outputs->filter_ctx = bufSrc_;
	outputs->pad_idx = 0;
	outputs->next = nullptr;
	inputs->name = av_strdup("out");
	inputs->filter_ctx = bufSink_;
	inputs->pad_idx = 0;
	inputs->next = nullptr;

	int r = avfilter_graph_parse_ptr(graph_, spec.constData(), &inputs, &outputs, nullptr);
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	if (r < 0)
		return fail(err, "Could not build the audio speed filter.");
	if (avfilter_graph_config(graph_, nullptr) < 0)
		return fail(err, "Could not configure the audio speed filter.");
	return true;
}

void AudioRetimer::destroyGraph()
{
	if (graph_)
		avfilter_graph_free(&graph_); // frees the filter contexts too
	bufSrc_ = nullptr;
	bufSink_ = nullptr;
}

bool AudioRetimer::beginSegment(int64_t startMs, int64_t endMs, double speed, QString *err)
{
	if (!dec_ || !enc_)
		return fail(err, "Audio pipeline not initialized.");
	if (!buildGraph(speed, err))
		return false;
	avcodec_flush_buffers(dec_);
	segStartTb_ = av_rescale_q(startMs, AVRational{1, 1000}, AVRational{tbNum_, tbDen_});
	segEndTb_ = av_rescale_q(endMs, AVRational{1, 1000}, AVRational{tbNum_, tbDen_});
	inSegment_ = true;
	return true;
}

bool AudioRetimer::push(const AVPacket *pkt, const WritePacket &write, QString *err)
{
	if (!inSegment_)
		return true;
	if (avcodec_send_packet(dec_, pkt) < 0)
		return true; // tolerate isolated bad packets rather than failing the export
	while (true) {
		const int r = avcodec_receive_frame(dec_, decFrame_);
		if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
			break;
		if (r < 0)
			return fail(err, "Audio decoding failed.");
		const int64_t pts = decFrame_->best_effort_timestamp;
		const bool inRange =
			pts == AV_NOPTS_VALUE || (pts >= segStartTb_ && pts < segEndTb_);
		if (inRange) {
			if (av_buffersrc_add_frame_flags(bufSrc_, decFrame_,
							 AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
				av_frame_unref(decFrame_);
				return fail(err, "Audio filtering failed.");
			}
			if (!drainSink(write, err)) {
				av_frame_unref(decFrame_);
				return false;
			}
		}
		av_frame_unref(decFrame_);
	}
	return true;
}

bool AudioRetimer::endSegment(const WritePacket &write, QString *err)
{
	if (!inSegment_)
		return true;
	inSegment_ = false;
	// Flush the filter graph so atempo emits its buffered tail.
	if (av_buffersrc_add_frame(bufSrc_, nullptr) < 0)
		return fail(err, "Audio filtering failed.");
	if (!drainSink(write, err))
		return false;
	destroyGraph();
	return true;
}

bool AudioRetimer::finish(const WritePacket &write, QString *err)
{
	if (inSegment_ && !endSegment(write, err))
		return false;
	if (!enc_)
		return true;
	// Emit the remaining (short) frame, then flush the encoder.
	const int leftover = fifo_ ? av_audio_fifo_size(fifo_) : 0;
	if (leftover > 0) {
		if (av_frame_make_writable(encFrame_) < 0)
			return fail(err, "Out of memory.");
		if (av_audio_fifo_read(fifo_, (void **)encFrame_->extended_data, leftover) < leftover)
			return fail(err, "Audio buffering failed.");
		encFrame_->nb_samples = leftover;
		encFrame_->pts = nextPts_;
		nextPts_ += leftover;
		if (!sendToEncoder(encFrame_, write, err))
			return false;
	}
	return sendToEncoder(nullptr, write, err);
}

bool AudioRetimer::drainSink(const WritePacket &write, QString *err)
{
	while (true) {
		const int r = av_buffersink_get_frame(bufSink_, filtFrame_);
		if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
			break;
		if (r < 0)
			return fail(err, "Audio filtering failed.");
		const int wrote = av_audio_fifo_write(fifo_, (void **)filtFrame_->extended_data,
						      filtFrame_->nb_samples);
		const int expected = filtFrame_->nb_samples;
		av_frame_unref(filtFrame_);
		if (wrote < expected)
			return fail(err, "Audio buffering failed.");
	}
	return encodeReady(write, err);
}

bool AudioRetimer::encodeReady(const WritePacket &write, QString *err)
{
	const int frameSize = enc_->frame_size > 0 ? enc_->frame_size : 1024;
	while (av_audio_fifo_size(fifo_) >= frameSize) {
		if (av_frame_make_writable(encFrame_) < 0)
			return fail(err, "Out of memory.");
		if (av_audio_fifo_read(fifo_, (void **)encFrame_->extended_data, frameSize) < frameSize)
			return fail(err, "Audio buffering failed.");
		encFrame_->nb_samples = frameSize;
		encFrame_->pts = nextPts_;
		nextPts_ += frameSize;
		if (!sendToEncoder(encFrame_, write, err))
			return false;
	}
	return true;
}

bool AudioRetimer::sendToEncoder(AVFrame *frame, const WritePacket &write, QString *err)
{
	if (avcodec_send_frame(enc_, frame) < 0)
		return fail(err, "Audio encoding failed.");
	while (true) {
		const int r = avcodec_receive_packet(enc_, encPkt_);
		if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
			break;
		if (r < 0)
			return fail(err, "Audio encoding failed.");
		const bool ok = write(encPkt_);
		av_packet_unref(encPkt_);
		if (!ok)
			return fail(err, "Writing audio to the output failed.");
	}
	return true;
}

} // namespace harpia
