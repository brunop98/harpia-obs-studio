#include "FrameSeeker.hpp"
#include "SwsColor.hpp"

#include <algorithm>
#include <cmath>

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
	if (cur_)
		av_frame_free(&cur_);
	if (ahead_)
		av_frame_free(&ahead_);
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
	haveCur_ = haveAhead_ = false;
	cacheImg_ = QImage();
	cacheW_ = cacheH_ = 0;
	frameDurMs_ = 0.0;
	lastPtsMs_ = -1.0;
}

bool FrameSeeker::open(const QString &path, bool fastThumbnails)
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
	// Decode across cores. A seek lands on the previous keyframe and then has to
	// roll forward to the target — up to a whole GOP — so single-threaded decode
	// dominated both filmstrip generation and scrub latency.
	dec_->thread_count = 0; // 0 = pick from the CPU
	dec_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
	if (fastThumbnails) {
		// A filmstrip tile is ~128px wide, so the deblocking filter and the
		// non-reference frames contribute nothing visible to it.
		dec_->skip_loop_filter = AVDISCARD_ALL;
		dec_->skip_frame = AVDISCARD_NONREF;
		dec_->flags2 |= AV_CODEC_FLAG2_FAST;
	}
	if (avcodec_open2(dec_, codec, nullptr) < 0) {
		close();
		return false;
	}

	seqPkt_ = av_packet_alloc();
	seqFrame_ = av_frame_alloc();
	rndPkt_ = av_packet_alloc();
	rndFrame_ = av_frame_alloc();
	rndBest_ = av_frame_alloc();
	cur_ = av_frame_alloc();
	ahead_ = av_frame_alloc();

	width_ = dec_->width;
	height_ = dec_->height;
	codecName_ = QString::fromLatin1(codec->name ? codec->name : "");
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
	// An HDR frame (an iPhone's HLG, an HDR10 file) is not just another
	// matrix: it is tone mapped to SDR, as OBS does on an SDR canvas.
	if (hdr::kindOf(f).needsConversion()) {
		const AVPixFmtDescriptor *hd = av_pix_fmt_desc_get((AVPixelFormat)f->format);
		const bool hdrAlpha = hd && (hd->flags & AV_PIX_FMT_FLAG_ALPHA);
		QImage img(dw, dh, hdrAlpha ? QImage::Format_RGBA8888 : QImage::Format_RGBX8888);
		const bool bigReduction = f->width >= dw * 3 / 2 && f->height >= dh * 3 / 2;
		if (!hdr_.toRgb(f, dw, dh, AV_PIX_FMT_RGBA, img.bits(), int(img.bytesPerLine()),
				bigReduction ? SWS_AREA : SWS_BILINEAR))
			return {};
		return img;
	}
	if (!sws_ || swsW_ != dw || swsH_ != dh || swsFmt_ != f->format) {
		if (sws_)
			sws_freeContext(sws_);
		// Bilinear reads two source pixels per output pixel, so on a big
		// reduction -- 4K into a 720p preview is 3x -- it both misses most of
		// the picture and costs more than area averaging, which swscale has a
		// fast path for. Measured on 3840x2160 -> 1280x720: 18.7 ms a frame
		// bilinear against 9.8 ms area, and area is also the CLOSER of the two
		// to a Lanczos reference (39.35 dB against 39.14). Only for real
		// reductions: area has nothing to average when it is enlarging.
		// (SWS_FAST_BILINEAR is faster still and was rejected -- 24.5 dB, i.e.
		// visibly aliased.)
		const bool bigReduction = f->width >= dw * 3 / 2 && f->height >= dh * 3 / 2;
		sws_ = sws_getContext(f->width, f->height, (AVPixelFormat)f->format, dw, dh, AV_PIX_FMT_RGBA,
				      bigReduction ? SWS_AREA : SWS_BILINEAR, nullptr, nullptr, nullptr);
		// The video's own matrix (usually BT.709), not swscale's BT.601
		// default, which shifted every colour in the preview and in the
		// timeline export. See SwsColor.hpp.
		swsDecodeColors(sws_, f);
		swsW_ = dw;
		swsH_ = dh;
		swsFmt_ = f->format;
	}
	if (!sws_)
		return {};
	// A frame from a source with no alpha channel -- every screen recording,
	// camera file and YUV video -- is tagged RGBX8888: the same bytes (swscale
	// writes 255 in the fourth byte), but QPainter then KNOWS it is opaque and
	// blits it in a third of the time a straight-alpha RGBA8888 image takes;
	// its fast blend table has no entry for a non-premultiplied source, so an
	// RGBA8888 frame goes through a per-pixel convert-blend-convert. Measured
	// at 1080p: 0.77 ms against 2.14 ms for the one draw every composited
	// frame makes. Sources that can carry transparency (yuva, rgba, a palette)
	// stay RGBA8888. Components retag before touching a frame (renderClip).
	const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get((AVPixelFormat)f->format);
	const bool mayHaveAlpha = pd && (pd->flags & (AV_PIX_FMT_FLAG_ALPHA | AV_PIX_FMT_FLAG_PAL));
	QImage img(dw, dh, mayHaveAlpha ? QImage::Format_RGBA8888 : QImage::Format_RGBX8888);
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
	const double tbMs = av_q2d(st->time_base) * 1000.0;

	// The frame on screen at `ms`: the last one at or before it, as a player
	// shows it -- give or take a millisecond, because callers ask in whole
	// milliseconds (an export's 60 fps is 0, 17, 33, 50, ...) and 33 means the
	// frame at 33.3. This used to be the first frame AT OR AFTER `ms`: 17 then
	// fell just past the frame at 16.7 and got the one at 33.3, and 33 got it
	// again -- a frame lost and a frame doubled, one in three, in every export.
	// The frame spacing is measured from the frames themselves, so a file
	// whose header guesses its rate wrong (a phone's variable rate) still
	// reads frame by frame.
	const double dur = frameDurMs_ > 0.5 ? frameDurMs_ : (fps_ > 1.0 ? 1000.0 / fps_ : 1000.0 / 30.0);
	const double limit = double(ms) + std::min(1.0, dur / 4.0);

	// Still the frame on screen (at the same size)? Answer from cache, so slow
	// drags inside one frame -- and an export at more frames per second than
	// the clip has -- cost nothing.
	// (Before a file's first frame, that first frame is what shows.)
	const bool atOrPastCur = haveCur_ && (limit >= curPtsMs_ || curIsFirst_);
	if (atOrPastCur && !cacheImg_.isNull() && cacheW_ == maxW && cacheH_ == maxH &&
	    limit < (haveAhead_ ? aheadPtsMs_ : curPtsMs_ + dur))
		return cacheImg_;

	AVFrame *best = rndBest_;
	av_frame_unref(best);
	bool haveBest = false;
	double bestMs = 0.0;

	// At or past the frame on screen, and not far: decode on from it -- the
	// next frame of an export, or a rightward handle drag, costs only the
	// frames in between instead of everything since the previous keyframe.
	// Anything else seeks.
	bool first = false; // the answer is the file's first frame, which comes after `ms`
	if (atOrPastCur && double(ms) - curPtsMs_ <= 2000.0) {
		first = curIsFirst_;
		av_frame_ref(best, cur_);
		haveBest = true;
		bestMs = curPtsMs_;
	} else {
		++seeks_;
		const int64_t target = av_rescale_q_rnd(int64_t(std::floor(limit * 1000.0)), AVRational{1, 1000000},
							st->time_base, AV_ROUND_DOWN);
		av_seek_frame(fmt_, vIdx_, target, AVSEEK_FLAG_BACKWARD);
		avcodec_flush_buffers(dec_);
		flushed_ = false;
		av_frame_unref(ahead_);
		haveAhead_ = false;
		dropCurrent();
		lastPtsMs_ = -1.0;
	}

	// Frames in order until one comes after `limit`: the one before it is the
	// answer, and that one goes back for the next call -- it is the next frame.
	AVFrame *frame = rndFrame_;
	while (pullFrame(frame, rndPkt_)) {
		const double at = frame->best_effort_timestamp != AV_NOPTS_VALUE
					  ? double(frame->best_effort_timestamp) * tbMs
					  : (haveBest ? bestMs + dur : limit);
		if (lastPtsMs_ >= 0.0 && at - lastPtsMs_ > 0.5 && at - lastPtsMs_ < 1000.0)
			frameDurMs_ = at - lastPtsMs_;
		lastPtsMs_ = at;
		if (haveBest && at > limit) {
			av_frame_unref(ahead_);
			av_frame_move_ref(ahead_, frame);
			haveAhead_ = true;
			aheadPtsMs_ = at;
			break;
		}
		av_frame_unref(best);
		av_frame_move_ref(best, frame);
		first = !haveBest && at > limit;
		haveBest = true;
		bestMs = at;
		if (first)
			break; // the file's first frame comes after `ms`: it is what shows
	}
	// Out of frames (the end of the file): the last one stays on screen.
	av_frame_unref(frame);
	av_packet_unref(rndPkt_);
	if (!haveBest)
		return {};

	QImage img = toImage(best, maxW, maxH);
	av_frame_unref(cur_);
	av_frame_move_ref(cur_, best);
	haveCur_ = true;
	curIsFirst_ = first;
	curPtsMs_ = bestMs;
	posMs_ = qint64(std::llround(bestMs));
	cacheImg_ = img;
	cacheW_ = maxW;
	cacheH_ = maxH;
	return img;
}

void FrameSeeker::dropCurrent()
{
	if (cur_)
		av_frame_unref(cur_);
	haveCur_ = false;
}

bool FrameSeeker::pullFrame(AVFrame *into, AVPacket *pkt)
{
	// The frame frameAt looked at and put back comes first: it is the next one.
	if (haveAhead_) {
		av_frame_unref(into);
		av_frame_move_ref(into, ahead_);
		haveAhead_ = false;
		return true;
	}
	while (true) {
		const int r = avcodec_receive_frame(dec_, into);
		if (r == 0)
			return true;
		if (r != AVERROR(EAGAIN))
			return false; // the end of the stream, or an error

		// Need more input: feed the next video packet, or flush at EOF.
		bool fed = false;
		while (av_read_frame(fmt_, pkt) >= 0) {
			if (pkt->stream_index == vIdx_) {
				avcodec_send_packet(dec_, pkt);
				av_packet_unref(pkt);
				fed = true;
				break;
			}
			av_packet_unref(pkt);
		}
		if (!fed) {
			if (flushed_)
				return false;
			avcodec_send_packet(dec_, nullptr);
			flushed_ = true;
		}
	}
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
	av_frame_unref(ahead_);
	haveAhead_ = false;
	dropCurrent();
	lastPtsMs_ = -1.0;
	return true;
}

bool FrameSeeker::decodeNextInto(qint64 *ptsMs)
{
	AVStream *st = fmt_->streams[vIdx_];
	if (!pullFrame(seqFrame_, seqPkt_))
		return false;
	const int64_t pts =
		seqFrame_->best_effort_timestamp != AV_NOPTS_VALUE ? seqFrame_->best_effort_timestamp : 0;
	if (ptsMs)
		*ptsMs = (qint64)(pts * av_q2d(st->time_base) * 1000.0);
	// The reader has moved on: frameAt's frame on screen is no longer the one
	// just before the decoder's position.
	dropCurrent();
	lastPtsMs_ = -1.0;
	return true;
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
	posMs_ = ptsMs;
	QImage img = toImage(seqFrame_, maxW, maxH);
	av_frame_unref(seqFrame_);
	return img;
}

// NOTE FOR CALLERS: this always advances by at least one frame. It cannot show
// you the frame you are already on. A caller stepping a wall clock must check
// positionMs() first and simply not call when the clock has not yet moved past
// the current frame -- otherwise the picture outruns the clock, which is how
// Multi-Cut playback used to sail past a cut's end handle on any source whose
// frame interval is longer than the tick.
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
