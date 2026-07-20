#include "GifEncoder.hpp"

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace harpia {

namespace {

int evenDown(int v)
{
	return v & ~1;
}

// All libav state, freed in the destructor.
struct GifState {
	AVFormatContext *ifmt = nullptr;
	AVCodecContext *vdec = nullptr;
	SwsContext *sws = nullptr;
	AVFilterGraph *graph = nullptr;
	AVFilterContext *srcCtx = nullptr;
	AVFilterContext *sinkCtx = nullptr;
	AVCodecContext *enc = nullptr;
	AVFormatContext *ofmt = nullptr;
	AVFrame *rgb = nullptr; // scaled rgb24 frame fed to the filter graph
	bool headerWritten = false;

	~GifState()
	{
		if (rgb)
			av_frame_free(&rgb);
		if (sws)
			sws_freeContext(sws);
		if (graph)
			avfilter_graph_free(&graph);
		if (enc)
			avcodec_free_context(&enc);
		if (vdec)
			avcodec_free_context(&vdec);
		if (ofmt) {
			if (ofmt->pb && !(ofmt->oformat->flags & AVFMT_NOFILE))
				avio_closep(&ofmt->pb);
			avformat_free_context(ofmt);
		}
		if (ifmt)
			avformat_close_input(&ifmt);
	}
};

} // namespace

bool GifEncoder::encode(const QString &inPath, const QString &outPath, const Params &p,
			const std::function<bool()> &canceled,
			const std::function<void(int, qint64, qint64)> &progress, QString *error)
{
	auto fail = [&](const char *m) {
		if (error)
			*error = QString::fromUtf8(m);
		return false;
	};

	GifState s;
	const QByteArray in = inPath.toUtf8();
	const QByteArray out = outPath.toUtf8();

	if (avformat_open_input(&s.ifmt, in.constData(), nullptr, nullptr) < 0)
		return fail("Could not open the source file.");
	if (avformat_find_stream_info(s.ifmt, nullptr) < 0)
		return fail("Could not read the source file.");

	const int vIdx = av_find_best_stream(s.ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (vIdx < 0)
		return fail("The file has no video track.");
	AVStream *vin = s.ifmt->streams[vIdx];

	const AVCodec *vdc = avcodec_find_decoder(vin->codecpar->codec_id);
	if (!vdc)
		return fail("Unsupported source video codec.");
	s.vdec = avcodec_alloc_context3(vdc);
	if (!s.vdec || avcodec_parameters_to_context(s.vdec, vin->codecpar) < 0)
		return fail("Could not set up the video decoder.");
	s.vdec->pkt_timebase = vin->time_base;
	if (avcodec_open2(s.vdec, vdc, nullptr) < 0)
		return fail("Could not open the video decoder.");

	// Resolve crop (default full frame), even-aligned + clamped.
	int cx = 0, cy = 0, cw = s.vdec->width, ch = s.vdec->height;
	if (p.crop && p.cropW > 0 && p.cropH > 0) {
		cx = std::clamp(p.cropX, 0, s.vdec->width - 2);
		cy = std::clamp(p.cropY, 0, s.vdec->height - 2);
		cw = std::min(p.cropW, s.vdec->width - cx);
		ch = std::min(p.cropH, s.vdec->height - cy);
	}
	cx = evenDown(cx);
	cy = evenDown(cy);
	cw = std::max(2, evenDown(cw));
	ch = std::max(2, evenDown(ch));

	// Output GIF size: cap width, preserve aspect, keep dimensions even.
	int outW = (p.width > 0) ? std::min(p.width, cw) : cw;
	int outH = int((int64_t)ch * outW / cw);
	outW = std::max(2, evenDown(outW));
	outH = std::max(2, evenDown(outH));
	const int fps = std::clamp(p.fps, 1, 50);

	// Scaler: crop + scale straight to rgb24 at the GIF size (crop by pointer
	// offset on a yuv420p copy — but the decoder gives us YUV, so scale the
	// cropped rectangle directly).
	s.sws = sws_getContext(cw, ch, s.vdec->pix_fmt, outW, outH, AV_PIX_FMT_RGB24, SWS_LANCZOS, nullptr,
			       nullptr, nullptr);
	if (!s.sws)
		return fail("Could not set up GIF scaling.");
	s.rgb = av_frame_alloc();
	s.rgb->format = AV_PIX_FMT_RGB24;
	s.rgb->width = outW;
	s.rgb->height = outH;
	if (av_frame_get_buffer(s.rgb, 0) < 0)
		return fail("Out of memory.");

	// Filter graph: palettegen (shared palette) + paletteuse (dithered) -> pal8.
	s.graph = avfilter_graph_alloc();
	if (!s.graph)
		return fail("Could not create the GIF filter graph.");
	char args[256];
	snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=1/%d:pixel_aspect=1/1", outW, outH,
		 AV_PIX_FMT_RGB24, fps);
	if (avfilter_graph_create_filter(&s.srcCtx, avfilter_get_by_name("buffer"), "in", args, nullptr,
					 s.graph) < 0)
		return fail("Could not create the GIF input filter.");
	if (avfilter_graph_create_filter(&s.sinkCtx, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr,
					 s.graph) < 0)
		return fail("Could not create the GIF output filter.");
	// No explicit sink format constraint: paletteuse always outputs PAL8, which is
	// exactly what the GIF encoder wants. (Avoids the deprecated
	// av_opt_set_int_list, which MSVC rejects under warnings-as-errors.)

	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs = avfilter_inout_alloc();
	outputs->name = av_strdup("in");
	outputs->filter_ctx = s.srcCtx;
	outputs->pad_idx = 0;
	outputs->next = nullptr;
	inputs->name = av_strdup("out");
	inputs->filter_ctx = s.sinkCtx;
	inputs->pad_idx = 0;
	inputs->next = nullptr;
	const char *graphDesc =
		"split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle";
	int gp = avfilter_graph_parse_ptr(s.graph, graphDesc, &inputs, &outputs, nullptr);
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	if (gp < 0 || avfilter_graph_config(s.graph, nullptr) < 0)
		return fail("Could not build the GIF palette filter.");

	// GIF encoder + output.
	const AVCodec *gifc = avcodec_find_encoder(AV_CODEC_ID_GIF);
	if (!gifc)
		return fail("The GIF encoder is not available in this build.");
	s.enc = avcodec_alloc_context3(gifc);
	s.enc->width = outW;
	s.enc->height = outH;
	s.enc->pix_fmt = AV_PIX_FMT_PAL8;
	s.enc->time_base = {1, fps};
	if (avformat_alloc_output_context2(&s.ofmt, nullptr, nullptr, out.constData()) < 0 || !s.ofmt)
		return fail("Could not create the GIF file.");
	if (s.ofmt->oformat->flags & AVFMT_GLOBALHEADER)
		s.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	if (avcodec_open2(s.enc, gifc, nullptr) < 0)
		return fail("Could not open the GIF encoder.");
	AVStream *st = avformat_new_stream(s.ofmt, nullptr);
	if (!st || avcodec_parameters_from_context(st->codecpar, s.enc) < 0)
		return fail("Could not create the GIF stream.");
	st->time_base = s.enc->time_base;
	if (!(s.ofmt->oformat->flags & AVFMT_NOFILE)) {
		if (avio_open(&s.ofmt->pb, out.constData(), AVIO_FLAG_WRITE) < 0)
			return fail("Could not open the GIF file for writing.");
	}
	if (avformat_write_header(s.ofmt, nullptr) < 0)
		return fail("Could not start writing the GIF.");
	s.headerWritten = true;

	// Trim range and fps sampling.
	const int64_t startV = av_rescale_q(p.startMs, {1, 1000}, vin->time_base);
	const int64_t endV = (p.endMs > 0) ? av_rescale_q(p.endMs, {1, 1000}, vin->time_base) : INT64_MAX;
	if (p.startMs > 0)
		av_seek_frame(s.ifmt, vIdx, startV, AVSEEK_FLAG_BACKWARD);
	avcodec_flush_buffers(s.vdec);

	// When endMs is 0 (export to the end of the clip), use the file duration so
	// the progress total stays positive.
	const int64_t durMs = s.ifmt->duration > 0 ? s.ifmt->duration / (AV_TIME_BASE / 1000) : 0;
	const double totalMs = double((p.endMs > 0 ? p.endMs : durMs) - p.startMs);
	const double speed = p.speed > 0.01 ? p.speed : 1.0;
	int outIndex = 0;             // GIF frame counter (pts in fps time base)
	double nextEmitMs = p.startMs; // next source time to sample

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	AVFrame *filt = av_frame_alloc();
	AVPacket *outPkt = av_packet_alloc();
	bool errored = false;
	bool done = false;

	auto drainSink = [&]() -> bool {
		while (true) {
			int r = av_buffersink_get_frame(s.sinkCtx, filt);
			if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
				break;
			if (r < 0)
				return false;
			if (avcodec_send_frame(s.enc, filt) < 0) {
				av_frame_unref(filt);
				return false;
			}
			av_frame_unref(filt);
			while (true) {
				int e = avcodec_receive_packet(s.enc, outPkt);
				if (e == AVERROR(EAGAIN) || e == AVERROR_EOF)
					break;
				if (e < 0)
					return false;
				av_packet_rescale_ts(outPkt, s.enc->time_base, st->time_base);
				outPkt->stream_index = 0;
				if (av_interleaved_write_frame(s.ofmt, outPkt) < 0)
					return false;
				av_packet_unref(outPkt);
			}
		}
		return true;
	};

	while (!errored && !done) {
		if (canceled())
			break;
		if (av_read_frame(s.ifmt, pkt) < 0)
			break;
		if (pkt->stream_index == vIdx && avcodec_send_packet(s.vdec, pkt) >= 0) {
			while (avcodec_receive_frame(s.vdec, frame) >= 0) {
				const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
							    ? frame->best_effort_timestamp
							    : 0;
				if (pts < startV) {
					av_frame_unref(frame);
					continue;
				}
				if (pts > endV) {
					done = true;
					av_frame_unref(frame);
					break;
				}
				const double tMs = pts * av_q2d(vin->time_base) * 1000.0;
				if (tMs + 0.001 < nextEmitMs) {
					av_frame_unref(frame); // skip: fps downsample
					continue;
				}
				// Advance the source sampling point by speed×: at 2× we sample
				// source time twice as fast while emitting at the GIF fps, so the
				// GIF plays back at speed×.
				nextEmitMs += (1000.0 / fps) * speed;

				// Crop + scale to rgb24 at the GIF size.
				const uint8_t *src[4] = {
					frame->data[0] + cy * frame->linesize[0] + cx,
					frame->data[1] + (cy / 2) * frame->linesize[1] + (cx / 2),
					frame->data[2] + (cy / 2) * frame->linesize[2] + (cx / 2), nullptr};
				const int srcStride[4] = {frame->linesize[0], frame->linesize[1],
							  frame->linesize[2], 0};
				if (av_frame_make_writable(s.rgb) < 0) {
					errored = true;
					av_frame_unref(frame);
					break;
				}
				sws_scale(s.sws, src, srcStride, 0, ch, s.rgb->data, s.rgb->linesize);
				s.rgb->pts = outIndex++;
				if (av_buffersrc_add_frame_flags(s.srcCtx, s.rgb, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
					errored = true;
					av_frame_unref(frame);
					break;
				}
				// palettegen buffers until EOF, so nothing to drain yet.
				av_frame_unref(frame);

				if (totalMs > 0) {
					const int pct = std::clamp(
						int((tMs - p.startMs) / totalMs * 100.0), 0, 99);
					progress(pct, 0, s.ofmt->pb ? avio_tell(s.ofmt->pb) : 0);
				}
			}
		}
		av_packet_unref(pkt);
	}

	if (!errored && !canceled()) {
		// Flush the graph: now palettegen emits the palette and paletteuse runs.
		if (av_buffersrc_add_frame(s.srcCtx, nullptr) >= 0 && drainSink()) {
			avcodec_send_frame(s.enc, nullptr);
			while (avcodec_receive_packet(s.enc, outPkt) >= 0) {
				av_packet_rescale_ts(outPkt, s.enc->time_base, st->time_base);
				outPkt->stream_index = 0;
				av_interleaved_write_frame(s.ofmt, outPkt);
				av_packet_unref(outPkt);
			}
			av_write_trailer(s.ofmt);
		} else {
			errored = true;
		}
	}

	av_packet_free(&pkt);
	av_frame_free(&frame);
	av_frame_free(&filt);
	av_packet_free(&outPkt);

	if (s.ofmt && s.ofmt->pb && !(s.ofmt->oformat->flags & AVFMT_NOFILE))
		avio_closep(&s.ofmt->pb);

	if (canceled())
		return false;
	if (errored)
		return fail("GIF encoding failed.");
	return true;
}

} // namespace harpia
