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
	if (seqPkt_)
		av_packet_free(&seqPkt_);
	if (seqFrame_)
		av_frame_free(&seqFrame_);
	if (rndPkt_)
		av_packet_free(&rndPkt_);
	if (rndFrame_)
		av_frame_free(&rndFrame_);
	if (rndBest_)
		av_frame_free(&rndBest_);
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
	flushed_ = false;
	posMs_ = -1;
	cacheImg_ = QImage();
	cacheMs_ = -1;
	cacheW_ = cacheH_ = 0;
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

	seqPkt_ = av_packet_alloc();
	seqFrame_ = av_frame_alloc();
	rndPkt_ = av_packet_alloc();
	rndFrame_ = av_frame_alloc();
	rndBest_ = av_frame_alloc();

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

QImage FrameSeeker::toImage(AVFrame *f, int maxW, int maxH)
{
	if (!f || f->width <= 0 || f->height <= 0)
		return {};
	int dw = f->width, dh = f->height;
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
		sws_ = sws_getContext(f->width, f->height, (AVPixelFormat)f->format, dw, dh, AV_PIX_FMT_RGBA,
				      SWS_BILINEAR, nullptr, nullptr, nullptr);
		swsW_ = dw;
		swsH_ = dh;
	}
	if (!sws_)
		return {};
	QImage img(dw, dh, QImage::Format_RGBA8888);
	uint8_t *dst[4] = {img.bits(), nullptr, nullptr, nullptr};
	int dstStride[4] = {(int)img.bytesPerLine(), 0, 0, 0};
	sws_scale(sws_, f->data, f->linesize, 0, f->height, dst, dstStride);
	return img;
}

QImage FrameSeeker::frameAt(qint64 ms, int maxW, int maxH)
{
	if (!fmt_ || !dec_ || vIdx_ < 0)
		return {};

	AVStream *st = fmt_->streams[vIdx_];
	const int64_t target = av_rescale_q(ms, {1, 1000}, st->time_base);

	// Same frame as last time (at the same preview size)? Answer from cache so
	// slow drags inside one frame cost nothing.
	const qint64 frameMs = fps_ > 1.0 ? qint64(1000.0 / fps_) : 33;
	if (!cacheImg_.isNull() && cacheW_ == maxW && cacheH_ == maxH && ms >= cacheMs_ &&
	    ms < cacheMs_ + frameMs)
		return cacheImg_;

	// Slightly ahead of the decoder's position? Roll forward without seeking —
	// a rightward handle drag then decodes only the few frames in between,
	// instead of everything since the previous keyframe.
	const bool roll = posMs_ >= 0 && !flushed_ && ms > posMs_ && (ms - posMs_) <= 2000;
	if (!roll) {
		av_seek_frame(fmt_, vIdx_, target, AVSEEK_FLAG_BACKWARD);
		avcodec_flush_buffers(dec_);
		flushed_ = false;
	}

	// Reused scratch (members) — no per-call libav alloc/free during drags.
	AVPacket *pkt = rndPkt_;
	AVFrame *frame = rndFrame_;
	AVFrame *best = rndBest_;
	av_frame_unref(best);
	bool haveBest = false;

	auto keep = [&]() {
		av_frame_unref(best);
		av_frame_move_ref(best, frame);
		haveBest = true;
	};

	bool done = false;
	int64_t bestPts = AV_NOPTS_VALUE;
	// When rolling forward, frames buffered in the decoder come out first.
	while (!done) {
		int r = avcodec_receive_frame(dec_, frame);
		if (r == 0) {
			const int64_t pts = frame->best_effort_timestamp;
			bestPts = pts;
			keep();
			if (pts != AV_NOPTS_VALUE && pts >= target) {
				done = true;
				break;
			}
			av_frame_unref(frame);
			continue;
		}
		if (r != AVERROR(EAGAIN))
			break; // EOF or error — fall through to the drain below
		if (av_read_frame(fmt_, pkt) < 0)
			break;
		if (pkt->stream_index == vIdx_)
			avcodec_send_packet(dec_, pkt);
		av_packet_unref(pkt);
	}
	if (!done) {
		avcodec_send_packet(dec_, nullptr);
		while (avcodec_receive_frame(dec_, frame) >= 0) {
			bestPts = frame->best_effort_timestamp;
			keep();
			av_frame_unref(frame);
		}
		avcodec_flush_buffers(dec_);
		posMs_ = -1; // read position is at EOF — no rolling from here
	}

	QImage img = haveBest ? toImage(best, maxW, maxH) : QImage();
	if (haveBest) {
		const qint64 shownMs = bestPts != AV_NOPTS_VALUE
					       ? qint64(bestPts * av_q2d(st->time_base) * 1000.0)
					       : ms;
		if (done)
			posMs_ = shownMs;
		cacheImg_ = img;
		cacheMs_ = shownMs;
		cacheW_ = maxW;
		cacheH_ = maxH;
	}
	av_frame_unref(best);
	av_frame_unref(frame);
	av_packet_unref(pkt);
	return img;
}

bool FrameSeeker::seekTo(qint64 ms)
{
	if (!fmt_ || vIdx_ < 0)
		return false;
	AVStream *st = fmt_->streams[vIdx_];
	const int64_t target = av_rescale_q(ms, {1, 1000}, st->time_base);
	av_seek_frame(fmt_, vIdx_, target, AVSEEK_FLAG_BACKWARD);
	avcodec_flush_buffers(dec_);
	flushed_ = false;
	posMs_ = -1; // at the keyframe before ms — exact position unknown until decode
	return true;
}

bool FrameSeeker::decodeNextInto(qint64 *ptsMs)
{
	AVStream *st = fmt_->streams[vIdx_];

	while (true) {
		int r = avcodec_receive_frame(dec_, seqFrame_);
		if (r == 0) {
			const int64_t pts = seqFrame_->best_effort_timestamp != AV_NOPTS_VALUE
						    ? seqFrame_->best_effort_timestamp
						    : 0;
			if (ptsMs)
				*ptsMs = (qint64)(pts * av_q2d(st->time_base) * 1000.0);
			return true;
		}
		if (r == AVERROR_EOF)
			return false;
		if (r != AVERROR(EAGAIN))
			return false;

		// Need more input: feed the next video packet, or flush at EOF.
		bool fed = false;
		while (av_read_frame(fmt_, seqPkt_) >= 0) {
			if (seqPkt_->stream_index == vIdx_) {
				avcodec_send_packet(dec_, seqPkt_);
				av_packet_unref(seqPkt_);
				fed = true;
				break;
			}
			av_packet_unref(seqPkt_);
		}
		if (!fed) {
			if (!flushed_) {
				avcodec_send_packet(dec_, nullptr);
				flushed_ = true;
			} else {
				return false;
			}
		}
	}
}

QImage FrameSeeker::nextFrame(qint64 *outMs, int maxW, int maxH)
{
	if (!fmt_ || !dec_ || vIdx_ < 0)
		return {};
	qint64 ptsMs = 0;
	if (!decodeNextInto(&ptsMs))
		return {};
	if (outMs)
		*outMs = ptsMs;
	posMs_ = ptsMs; // keep frameAt's roll-forward anchor in sync
	QImage img = toImage(seqFrame_, maxW, maxH);
	av_frame_unref(seqFrame_);
	return img;
}

QImage FrameSeeker::nextFrameAt(qint64 targetMs, qint64 *outMs, int maxW, int maxH, int maxFrames)
{
	if (!fmt_ || !dec_ || vIdx_ < 0)
		return {};
	for (int i = 0; i < maxFrames; ++i) {
		qint64 ptsMs = 0;
		if (!decodeNextInto(&ptsMs))
			return {}; // end of stream — the caller loops/advances
		posMs_ = ptsMs;
		if (ptsMs >= targetMs || i == maxFrames - 1) {
			// The frame we'll actually show — the only one converted.
			if (outMs)
				*outMs = ptsMs;
			QImage img = toImage(seqFrame_, maxW, maxH);
			av_frame_unref(seqFrame_);
			return img;
		}
		av_frame_unref(seqFrame_); // skipped catch-up frame: no conversion
	}
	return {};
}

} // namespace harpia
