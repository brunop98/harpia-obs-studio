#include "SceneDetector.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

#include <algorithm>
#include <cstdio>

namespace harpia {

QVector<qint64> SceneDetector::detect(const QString &path, double threshold,
				      std::atomic<bool> *cancel,
				      const std::function<void(int)> &progress, QString *err)
{
	QVector<qint64> cuts;
	auto setErr = [&](const char *m) {
		if (err)
			*err = QString::fromUtf8(m);
		return cuts;
	};

	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0)
		return setErr("Could not open the video.");
	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		avformat_close_input(&fmt);
		return setErr("Could not read the video streams.");
	}
	const int vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (vIdx < 0) {
		avformat_close_input(&fmt);
		return setErr("No video stream found.");
	}
	AVStream *st = fmt->streams[vIdx];
	const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
	AVCodecContext *dc = dec ? avcodec_alloc_context3(dec) : nullptr;
	if (!dc) {
		avformat_close_input(&fmt);
		return setErr("No decoder for this video.");
	}
	avcodec_parameters_to_context(dc, st->codecpar);
	if (avcodec_open2(dc, dec, nullptr) < 0) {
		avcodec_free_context(&dc);
		avformat_close_input(&fmt);
		return setErr("Could not open the decoder.");
	}

	// Build: buffer → select='gt(scene,T)' → buffersink.
	AVFilterGraph *graph = avfilter_graph_alloc();
	AVFilterContext *bufsrc = nullptr, *sel = nullptr, *bufsink = nullptr;
	const AVRational tb = st->time_base;
	const AVRational sar =
		dc->sample_aspect_ratio.num ? dc->sample_aspect_ratio : AVRational{1, 1};
	char args[512];
	std::snprintf(args, sizeof(args),
		      "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", dc->width,
		      dc->height, (int)dc->pix_fmt, tb.num, tb.den, sar.num, sar.den);
	char selargs[96];
	std::snprintf(selargs, sizeof(selargs), "gt(scene,%f)", threshold);

	auto cleanup = [&]() {
		avfilter_graph_free(&graph);
		avcodec_free_context(&dc);
		avformat_close_input(&fmt);
	};

	if (!graph ||
	    avfilter_graph_create_filter(&bufsrc, avfilter_get_by_name("buffer"), "in", args, nullptr,
					 graph) < 0 ||
	    avfilter_graph_create_filter(&sel, avfilter_get_by_name("select"), "sel", selargs, nullptr,
					 graph) < 0 ||
	    avfilter_graph_create_filter(&bufsink, avfilter_get_by_name("buffersink"), "out", nullptr,
					 nullptr, graph) < 0 ||
	    avfilter_link(bufsrc, 0, sel, 0) < 0 || avfilter_link(sel, 0, bufsink, 0) < 0 ||
	    avfilter_graph_config(graph, nullptr) < 0) {
		cleanup();
		return setErr("Could not build the scene-detection filter.");
	}

	AVPacket *pkt = av_packet_alloc();
	AVFrame *fr = av_frame_alloc();
	AVFrame *out = av_frame_alloc();

	// Total duration (in stream ticks) for progress.
	int64_t durTb = st->duration > 0 ? st->duration : 0;
	if (durTb <= 0 && fmt->duration > 0)
		durTb = av_rescale_q(fmt->duration, AVRational{1, AV_TIME_BASE}, tb);

	auto drainSink = [&]() {
		while (av_buffersink_get_frame(bufsink, out) >= 0) {
			int64_t pts = out->pts != AV_NOPTS_VALUE ? out->pts : out->best_effort_timestamp;
			if (pts != AV_NOPTS_VALUE)
				cuts.append(qint64(pts * av_q2d(tb) * 1000.0));
			av_frame_unref(out);
		}
	};

	bool canceled = false;
	while (av_read_frame(fmt, pkt) >= 0) {
		if (cancel && cancel->load()) {
			av_packet_unref(pkt);
			canceled = true;
			break;
		}
		if (pkt->stream_index == vIdx && avcodec_send_packet(dc, pkt) >= 0) {
			while (avcodec_receive_frame(dc, fr) >= 0) {
				fr->pts = fr->best_effort_timestamp;
				if (av_buffersrc_add_frame_flags(bufsrc, fr, AV_BUFFERSRC_FLAG_KEEP_REF) >= 0)
					drainSink();
				if (progress && durTb > 0 && fr->best_effort_timestamp != AV_NOPTS_VALUE)
					progress(std::clamp(
						int(100.0 * double(fr->best_effort_timestamp) / double(durTb)),
						0, 99));
				av_frame_unref(fr);
			}
		}
		av_packet_unref(pkt);
	}
	if (!canceled) {
		// Flush the decoder, then the graph.
		avcodec_send_packet(dc, nullptr);
		while (avcodec_receive_frame(dc, fr) >= 0) {
			fr->pts = fr->best_effort_timestamp;
			if (av_buffersrc_add_frame_flags(bufsrc, fr, AV_BUFFERSRC_FLAG_KEEP_REF) >= 0)
				drainSink();
			av_frame_unref(fr);
		}
		if (av_buffersrc_add_frame(bufsrc, nullptr) >= 0) // EOF
			drainSink();
		if (progress)
			progress(100);
	}

	av_frame_free(&out);
	av_frame_free(&fr);
	av_packet_free(&pkt);
	cleanup();
	return cuts;
}

} // namespace harpia
