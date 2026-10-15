#include "CoverArt.hpp"

#include "SwsColor.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>

namespace harpia::cover {

QSize sizeFor(QSize video)
{
	if (video.width() <= 0 || video.height() <= 0)
		return {};
	const double k = std::min(1.0, double(kMaxSide) / std::max(video.width(), video.height()));
	const auto even = [](double v) { return std::max(2, int(std::lround(v)) & ~1); };
	return QSize(even(video.width() * k), even(video.height() * k));
}

QByteArray jpeg(const AVFrame *f, QSize size, AVColorSpace cs, AVColorRange range)
{
	if (!f || f->width <= 0 || f->height <= 0 || size.isEmpty())
		return {};
	const int w = size.width(), h = size.height();
	QByteArray out;
	// Through RGB: what the YUV means (the file's tags), then JFIF's BT.601
	// full range. A YUV -> YUV conversion would keep BT.709's numbers under a
	// BT.601 label, and every colour on the cover would shift.
	SwsContext *toRgb = sws_getContext(f->width, f->height, AVPixelFormat(f->format), w, h, AV_PIX_FMT_RGB24,
					   SWS_BICUBIC, nullptr, nullptr, nullptr);
	SwsContext *toJpeg = sws_getContext(w, h, AV_PIX_FMT_RGB24, w, h, AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr,
					    nullptr, nullptr);
	AVFrame *rgb = av_frame_alloc();
	AVFrame *yuv = av_frame_alloc();
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
	AVCodecContext *enc = codec ? avcodec_alloc_context3(codec) : nullptr;
	AVPacket *pkt = av_packet_alloc();
	if (toRgb && toJpeg && rgb && yuv && enc && pkt) {
		swsDecodeColors(toRgb, AVPixelFormat(f->format), cs, range, f->height);
		const int *bt601 = sws_getCoefficients(SWS_CS_ITU601);
		sws_setColorspaceDetails(toJpeg, bt601, 1, bt601, 1, 0, 1 << 16, 1 << 16);
		rgb->format = AV_PIX_FMT_RGB24;
		rgb->width = w;
		rgb->height = h;
		yuv->format = AV_PIX_FMT_YUV420P;
		yuv->width = w;
		yuv->height = h;
		yuv->color_range = AVCOL_RANGE_JPEG;
		enc->width = w;
		enc->height = h;
		enc->pix_fmt = AV_PIX_FMT_YUV420P;
		enc->color_range = AVCOL_RANGE_JPEG; // what makes plain yuv420p a JPEG's
		enc->time_base = AVRational{1, 25};
		enc->flags |= AV_CODEC_FLAG_QSCALE;
		enc->global_quality = FF_QP2LAMBDA * 3; // high quality, ~100 KB at 720 x 1280
		if (av_frame_get_buffer(rgb, 0) >= 0 && av_frame_get_buffer(yuv, 0) >= 0 &&
		    avcodec_open2(enc, codec, nullptr) >= 0) {
			sws_scale(toRgb, f->data, f->linesize, 0, f->height, rgb->data, rgb->linesize);
			sws_scale(toJpeg, rgb->data, rgb->linesize, 0, h, yuv->data, yuv->linesize);
			yuv->quality = enc->global_quality;
			yuv->pts = 0;
			if (avcodec_send_frame(enc, yuv) >= 0 && avcodec_send_frame(enc, nullptr) >= 0 &&
			    avcodec_receive_packet(enc, pkt) >= 0)
				out = QByteArray(reinterpret_cast<const char *>(pkt->data), pkt->size);
		}
	}
	av_packet_free(&pkt);
	avcodec_free_context(&enc);
	av_frame_free(&yuv);
	av_frame_free(&rgb);
	sws_freeContext(toJpeg);
	sws_freeContext(toRgb);
	return out;
}

Picker::Picker(QSize size, AVColorSpace cs, AVColorRange range, int keep)
	: size_(size), cs_(cs), range_(range), keep_(std::max(2, keep))
{
}

void Picker::offer(const AVFrame *f, double t)
{
	if (!f || size_.isEmpty())
		return;
	if (first_ < 0.0)
		first_ = t;
	last_ = std::max(last_, t);
	if (t < next_)
		return;
	const QByteArray j = jpeg(f, size_, cs_, range_);
	if (j.isEmpty())
		return;
	shots_.push_back({t, j});
	next_ = t + step_;
	if (shots_.size() >= 2 * keep_) {
		// Too many: every other one goes, and they are taken half as often.
		QVector<QPair<double, QByteArray>> kept;
		for (int i = 0; i < shots_.size(); i += 2)
			kept.push_back(shots_[i]);
		shots_ = kept;
		step_ *= 2.0;
		next_ = shots_.last().first + step_;
	}
}

QByteArray Picker::pick() const
{
	if (shots_.isEmpty())
		return {};
	const double middle = (first_ + last_) / 2.0;
	const auto best = std::min_element(shots_.begin(), shots_.end(), [middle](const auto &a, const auto &b) {
		return std::abs(a.first - middle) < std::abs(b.first - middle);
	});
	return best->second;
}

QVector<double> Picker::times() const
{
	QVector<double> t;
	for (const auto &s : shots_)
		t.push_back(s.first);
	return t;
}

} // namespace harpia::cover
