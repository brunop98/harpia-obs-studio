#include "FrameSeeker.hpp"

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace harpia {

FrameSeeker::~FrameSeeker()
{
	close();
}

void FrameSeeker::close()
{
	if (sws_) {
		sws_freeContext(sws_);
		sws_ = nullptr;
	}
	if (dec_)
		avcodec_free_context(&dec_);
	if (fmt_)
		avformat_close_input(&fmt_);
	swsW_ = swsH_ = 0;
	vIdx_ = -1;
}

bool FrameSeeker::open(const QString &path)
{
	close();
	const QByteArray p = path.toUtf8();
	if (avformat_open_input(&fmt_, p.constData(), nullptr, nullptr) < 0)
		return false;
	if (avformat_find_stream_info(fmt_, nullptr) < 0) {
		close();
		return false;
	}
	vIdx_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (vIdx_ < 0) {
		close();
		return false;
	}
	AVStream *st = fmt_->streams[vIdx_];
	const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec) {
		close();
		return false;
	}
	dec_ = avcodec_alloc_context3(codec);
	if (!dec_ || avcodec_parameters_to_context(dec_, st->codecpar) < 0) {
		close();
		return false;
	}
	dec_->pkt_timebase = st->time_base;
	if (avcodec_open2(dec_, codec, nullptr) < 0) {
		close();
		return false;
	}

	width_ = dec_->width;
	height_ = dec_->height;
	const AVRational fr = av_guess_frame_rate(fmt_, st, nullptr);
	fps_ = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 30.0;
	if (fmt_->duration > 0)
		durationMs_ = fmt_->duration / (AV_TIME_BASE / 1000);
	else if (st->duration > 0)
		durationMs_ = (qint64)(st->duration * av_q2d(st->time_base) * 1000.0);
	else
		durationMs_ = 0;
	return true;
}

QImage FrameSeeker::frameAt(qint64 ms, int maxW, int maxH)
{
	if (!fmt_ || !dec_ || vIdx_ < 0)
		return {};

	AVStream *st = fmt_->streams[vIdx_];
	const int64_t target = av_rescale_q(ms, {1, 1000}, st->time_base);

	// Seek to the keyframe at or before the target, then decode forward to it.
	av_seek_frame(fmt_, vIdx_, target, AVSEEK_FLAG_BACKWARD);
	avcodec_flush_buffers(dec_);

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	AVFrame *best = av_frame_alloc();
	bool haveBest = false;

	auto keep = [&]() {
		av_frame_unref(best);
		av_frame_move_ref(best, frame);
		haveBest = true;
	};

	bool done = false;
	while (!done && av_read_frame(fmt_, pkt) >= 0) {
		if (pkt->stream_index == vIdx_ && avcodec_send_packet(dec_, pkt) >= 0) {
			while (avcodec_receive_frame(dec_, frame) >= 0) {
				const int64_t pts = frame->best_effort_timestamp;
				keep(); // always keep the latest decoded frame as a fallback
				if (pts != AV_NOPTS_VALUE && pts >= target) {
					done = true;
					break;
				}
				av_frame_unref(frame);
			}
		}
		av_packet_unref(pkt);
	}
	// Flush the decoder if we ran out of packets before reaching the target.
	if (!done) {
		avcodec_send_packet(dec_, nullptr);
		while (avcodec_receive_frame(dec_, frame) >= 0) {
			keep();
			av_frame_unref(frame);
		}
	}

	QImage img;
	if (haveBest && best->width > 0 && best->height > 0) {
		// Fit within maxW x maxH, preserving aspect.
		int dw = best->width, dh = best->height;
		if (maxW > 0 && maxH > 0) {
			const double s = std::min(double(maxW) / dw, double(maxH) / dh);
			if (s < 1.0) {
				dw = std::max(1, int(dw * s));
				dh = std::max(1, int(dh * s));
			}
		}
		if (!sws_ || swsW_ != dw || swsH_ != dh) {
			if (sws_)
				sws_freeContext(sws_);
			sws_ = sws_getContext(best->width, best->height, (AVPixelFormat)best->format, dw, dh,
					      AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
			swsW_ = dw;
			swsH_ = dh;
		}
		if (sws_) {
			img = QImage(dw, dh, QImage::Format_RGBA8888);
			uint8_t *dst[4] = {img.bits(), nullptr, nullptr, nullptr};
			int dstStride[4] = {(int)img.bytesPerLine(), 0, 0, 0};
			sws_scale(sws_, best->data, best->linesize, 0, best->height, dst, dstStride);
		}
	}

	av_frame_free(&best);
	av_frame_free(&frame);
	av_packet_free(&pkt);
	return img;
}

} // namespace harpia
