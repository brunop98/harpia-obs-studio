#include "ClipExporter.hpp"

#include "AudioRetimer.hpp"
#include "GifEncoder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace harpia {

QString ClipExporter::extensionFor(Format f)
{
	switch (f) {
	case Format::Mkv:
		return QStringLiteral("mkv");
	case Format::Mov:
		return QStringLiteral("mov");
	case Format::WebM:
		return QStringLiteral("webm");
	case Format::Gif:
		return QStringLiteral("gif");
	case Format::Mp4:
	default:
		return QStringLiteral("mp4");
	}
}

bool ClipExporter::webmAvailable()
{
	return avcodec_find_encoder_by_name("libvpx-vp9") != nullptr;
}

ClipExporter::ClipExporter(QObject *parent) : QObject(parent) {}

namespace {

int evenDown(int v)
{
	return v & ~1;
}

int64_t durationMsOf(AVFormatContext *fmt)
{
	return fmt && fmt->duration > 0 ? fmt->duration / (AV_TIME_BASE / 1000) : 0;
}

// Everything freed in the destructor so early returns clean up.
struct VideoState {
	AVFormatContext *ifmt = nullptr;
	AVFormatContext *ofmt = nullptr;
	AVCodecContext *vdec = nullptr;
	AVCodecContext *venc = nullptr;
	SwsContext *toYuv = nullptr; // only if the source isn't yuv420p already
	AVFrame *fullYuv = nullptr;  // full-frame yuv420p scratch (for non-yuv sources)
	AVFrame *cropFrame = nullptr; // cropped yuv420p frame fed to the encoder
	AVStream *vOut = nullptr;
	AVStream *aOut = nullptr; // audio stream-copy (optional)
	bool headerWritten = false;

	~VideoState()
	{
		if (toYuv)
			sws_freeContext(toYuv);
		if (fullYuv)
			av_frame_free(&fullYuv);
		if (cropFrame)
			av_frame_free(&cropFrame);
		if (vdec)
			avcodec_free_context(&vdec);
		if (venc)
			avcodec_free_context(&venc);
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

void ClipExporter::run(const QString &inPath, const QString &outPath, const Options &opts)
{
	cancel_.store(false);

	if (opts.format == Format::Gif) {
		if (!opts.cuts.empty()) {
			emit finished(false, false,
				      QStringLiteral("GIF export is not available in Multi-Cut mode yet — "
						     "choose MP4, MKV or MOV."));
			return;
		}
		// Delegate to the avfilter-based GIF encoder; bridge its callbacks to our
		// Qt signals.
		GifEncoder::Params gp;
		gp.startMs = opts.startMs;
		gp.endMs = opts.endMs;
		gp.crop = opts.crop;
		gp.cropX = opts.cropX;
		gp.cropY = opts.cropY;
		gp.cropW = opts.cropW;
		gp.cropH = opts.cropH;
		gp.fps = opts.gifFps;
		gp.width = opts.gifWidth;
		gp.speed = opts.speed;
		QString err;
		const bool ok = GifEncoder::encode(
			inPath, outPath, gp, [this]() { return cancel_.load(); },
			[this](int pct, qint64 eta, qint64 bytes) { emit progress(pct, eta, bytes); }, &err);
		if (cancel_.load())
			emit finished(false, true, QString());
		else
			emit finished(ok, false, ok ? QString() : err);
		return;
	}

	const QString err = opts.cuts.empty() ? runVideo(inPath, outPath, opts)
					      : runVideoCuts(inPath, outPath, opts);
	if (cancel_.load())
		emit finished(false, true, QString());
	else
		emit finished(err.isEmpty(), false, err);
}

QString ClipExporter::runVideo(const QString &inPath, const QString &outPath, const Options &opts)
{
	VideoState s;
	const QByteArray in = inPath.toUtf8();
	const QByteArray out = outPath.toUtf8();

	if (avformat_open_input(&s.ifmt, in.constData(), nullptr, nullptr) < 0)
		return QStringLiteral("Could not open the source file.");
	if (avformat_find_stream_info(s.ifmt, nullptr) < 0)
		return QStringLiteral("Could not read the source file.");

	const int vIdx = av_find_best_stream(s.ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (vIdx < 0)
		return QStringLiteral("The file has no video track.");
	AVStream *vin = s.ifmt->streams[vIdx];

	const double speed = opts.speed > 0.01 ? opts.speed : 1.0;
	const bool container_h264 = opts.format != Format::WebM;
	// Speeding the video up would desync stream-copied audio (and pitch-shift it),
	// so audio is only kept at normal speed.
	const bool wantAudio = opts.keepAudio && container_h264 && speed == 1.0;
	int aIdx = wantAudio ? av_find_best_stream(s.ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) : -1;
	AVStream *ain = (aIdx >= 0) ? s.ifmt->streams[aIdx] : nullptr;
	// Only stream-copy AAC/MP3 audio (MP4/MKV/MOV-friendly); otherwise drop it.
	if (ain && ain->codecpar->codec_id != AV_CODEC_ID_AAC && ain->codecpar->codec_id != AV_CODEC_ID_MP3) {
		aIdx = -1;
		ain = nullptr;
	}

	// ---- Video decoder ----
	const AVCodec *vdc = avcodec_find_decoder(vin->codecpar->codec_id);
	if (!vdc)
		return QStringLiteral("Unsupported source video codec.");
	s.vdec = avcodec_alloc_context3(vdc);
	if (!s.vdec || avcodec_parameters_to_context(s.vdec, vin->codecpar) < 0)
		return QStringLiteral("Could not set up the video decoder.");
	s.vdec->pkt_timebase = vin->time_base;
	if (avcodec_open2(s.vdec, vdc, nullptr) < 0)
		return QStringLiteral("Could not open the video decoder.");

	// ---- Resolve crop (default = full frame), even-aligned & clamped ----
	int cx = 0, cy = 0, cw = s.vdec->width, ch = s.vdec->height;
	if (opts.crop && opts.cropW > 0 && opts.cropH > 0) {
		cx = std::clamp(opts.cropX, 0, s.vdec->width - 2);
		cy = std::clamp(opts.cropY, 0, s.vdec->height - 2);
		cw = std::min(opts.cropW, s.vdec->width - cx);
		ch = std::min(opts.cropH, s.vdec->height - cy);
	}
	cx = evenDown(cx);
	cy = evenDown(cy);
	cw = std::max(2, evenDown(cw));
	ch = std::max(2, evenDown(ch));
	// No crop selected (the default) -> decoded frames are fed straight to
	// the encoder; the per-frame full-image copy is skipped entirely.
	const bool cropNeeded = (cx != 0 || cy != 0 || cw != s.vdec->width || ch != s.vdec->height);

	// ---- Video encoder ----
	const char *encName = (opts.format == Format::WebM) ? "libvpx-vp9" : "libx264";
	const AVCodec *vc = avcodec_find_encoder_by_name(encName);
	if (!vc)
		return QStringLiteral("The output video encoder is not available in this build.");
	s.venc = avcodec_alloc_context3(vc);
	if (!s.venc)
		return QStringLiteral("Could not allocate the video encoder.");

	const AVRational fr = av_guess_frame_rate(s.ifmt, vin, nullptr);
	s.venc->width = cw;
	s.venc->height = ch;
	s.venc->pix_fmt = AV_PIX_FMT_YUV420P;
	s.venc->time_base = vin->time_base;
	s.venc->framerate = fr;
	s.venc->gop_size = (fr.num > 0 && fr.den > 0) ? std::max(1, int(av_q2d(fr) * 2.0)) : 60;

	// ---- Output container ----
	if (avformat_alloc_output_context2(&s.ofmt, nullptr, nullptr, out.constData()) < 0 || !s.ofmt)
		return QStringLiteral("Could not create the output file.");
	if (s.ofmt->oformat->flags & AVFMT_GLOBALHEADER)
		s.venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	if (opts.format == Format::WebM) {
		s.venc->bit_rate = 0; // constant-quality VP9
		av_opt_set_int(s.venc->priv_data, "crf", opts.videoCrf, 0);
		av_opt_set(s.venc->priv_data, "deadline", "good", 0);
		av_opt_set_int(s.venc->priv_data, "cpu-used", 5, 0);
		av_opt_set(s.venc->priv_data, "row-mt", "1", 0);
	} else {
		av_opt_set(s.venc->priv_data, "preset", "veryfast", 0);
		av_opt_set(s.venc->priv_data, "profile", "high", 0);
		av_opt_set_int(s.venc->priv_data, "crf", opts.videoCrf, 0);
	}
	if (avcodec_open2(s.venc, vc, nullptr) < 0)
		return QStringLiteral("Could not open the video encoder.");

	s.vOut = avformat_new_stream(s.ofmt, nullptr);
	if (!s.vOut || avcodec_parameters_from_context(s.vOut->codecpar, s.venc) < 0)
		return QStringLiteral("Could not create the output video stream.");
	s.vOut->time_base = s.venc->time_base;

	if (ain) {
		s.aOut = avformat_new_stream(s.ofmt, nullptr);
		if (!s.aOut || avcodec_parameters_copy(s.aOut->codecpar, ain->codecpar) < 0)
			return QStringLiteral("Could not copy the audio stream.");
		s.aOut->codecpar->codec_tag = 0;
		s.aOut->time_base = ain->time_base;
	}

	// ---- Scratch frames ----
	s.cropFrame = av_frame_alloc();
	s.cropFrame->format = AV_PIX_FMT_YUV420P;
	s.cropFrame->width = cw;
	s.cropFrame->height = ch;
	if (av_frame_get_buffer(s.cropFrame, 0) < 0)
		return QStringLiteral("Out of memory.");

	// ---- Open + write header (+faststart for MP4) ----
	if (!(s.ofmt->oformat->flags & AVFMT_NOFILE)) {
		if (avio_open(&s.ofmt->pb, out.constData(), AVIO_FLAG_WRITE) < 0)
			return QStringLiteral("Could not open the output file for writing.");
	}
	{
		AVDictionary *mux = nullptr;
		if (opts.format == Format::Mp4 || opts.format == Format::Mov)
			av_dict_set(&mux, "movflags", "+faststart", 0);
		int hr = avformat_write_header(s.ofmt, &mux);
		av_dict_free(&mux);
		if (hr < 0)
			return QStringLiteral("Could not start writing the output file.");
	}
	s.headerWritten = true;

	// ---- Trim range in each stream's time base ----
	const int64_t startV = av_rescale_q(opts.startMs, {1, 1000}, vin->time_base);
	const int64_t endV = (opts.endMs > 0) ? av_rescale_q(opts.endMs, {1, 1000}, vin->time_base) : INT64_MAX;
	const int64_t startA = ain ? av_rescale_q(opts.startMs, {1, 1000}, ain->time_base) : 0;
	const int64_t endA = ain ? ((opts.endMs > 0) ? av_rescale_q(opts.endMs, {1, 1000}, ain->time_base)
						     : INT64_MAX)
				 : 0;

	// Seek to the keyframe at/before the start so we don't decode from 0.
	if (opts.startMs > 0)
		av_seek_frame(s.ifmt, vIdx, startV, AVSEEK_FLAG_BACKWARD);
	avcodec_flush_buffers(s.vdec);

	const double totalMs = double((opts.endMs > 0 ? opts.endMs : (durationMsOf(s.ifmt))) - opts.startMs);
	const auto startWall = std::chrono::steady_clock::now();
	auto lastEmit = startWall;
	double processedMs = 0.0;

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	AVPacket *outPkt = av_packet_alloc();
	bool errored = false;
	bool videoDone = false;

	// Encoder pts, rebased to the trim start and compressed by the speed factor
	// (2× speed => timestamps half as far apart => plays twice as fast), kept
	// strictly increasing.
	int64_t lastEncPts = -1;
	auto scaledPts = [&](int64_t pts) -> int64_t {
		int64_t v = (int64_t)llround(double(pts - startV) / speed);
		if (v <= lastEncPts)
			v = lastEncPts + 1;
		lastEncPts = v;
		return v;
	};

	auto ensureYuvFull = [&](AVFrame *f) -> AVFrame * {
		if ((AVPixelFormat)f->format == AV_PIX_FMT_YUV420P)
			return f;
		if (!s.fullYuv) {
			s.fullYuv = av_frame_alloc();
			s.fullYuv->format = AV_PIX_FMT_YUV420P;
			s.fullYuv->width = s.vdec->width;
			s.fullYuv->height = s.vdec->height;
			av_frame_get_buffer(s.fullYuv, 0);
		}
		if (!s.toYuv)
			s.toYuv = sws_getContext(s.vdec->width, s.vdec->height, (AVPixelFormat)f->format,
						 s.vdec->width, s.vdec->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
						 nullptr, nullptr, nullptr);
		av_frame_make_writable(s.fullYuv); // encoder may still hold a ref
		sws_scale(s.toYuv, f->data, f->linesize, 0, s.vdec->height, s.fullYuv->data, s.fullYuv->linesize);
		return s.fullYuv;
	};

	auto encodeVideo = [&](AVFrame *f) -> bool {
		if (avcodec_send_frame(s.venc, f) < 0)
			return false;
		while (true) {
			int r = avcodec_receive_packet(s.venc, outPkt);
			if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
				break;
			if (r < 0)
				return false;
			av_packet_rescale_ts(outPkt, s.venc->time_base, s.vOut->time_base);
			outPkt->stream_index = s.vOut->index;
			if (av_interleaved_write_frame(s.ofmt, outPkt) < 0)
				return false;
			av_packet_unref(outPkt);
		}
		return true;
	};

	while (!errored && !videoDone) {
		if (cancel_.load())
			break;
		if (av_read_frame(s.ifmt, pkt) < 0)
			break;

		if (pkt->stream_index == vIdx) {
			if (avcodec_send_packet(s.vdec, pkt) >= 0) {
				while (avcodec_receive_frame(s.vdec, frame) >= 0) {
					const int64_t pts =
						frame->best_effort_timestamp != AV_NOPTS_VALUE
							? frame->best_effort_timestamp
							: 0;
					if (pts < startV) {
						av_frame_unref(frame);
						continue;
					}
					if (pts > endV) {
						videoDone = true;
						av_frame_unref(frame);
						break;
					}
					// Crop into the encoder frame only when a crop is set —
					// otherwise the decoded frame goes straight through.
					AVFrame *yf = ensureYuvFull(frame);
					AVFrame *toEnc = yf;
					if (cropNeeded) {
						if (av_frame_make_writable(s.cropFrame) < 0) {
							errored = true;
							av_frame_unref(frame);
							break;
						}
						const uint8_t *src[4] = {
							yf->data[0] + cy * yf->linesize[0] + cx,
							yf->data[1] + (cy / 2) * yf->linesize[1] + (cx / 2),
							yf->data[2] + (cy / 2) * yf->linesize[2] + (cx / 2),
							nullptr};
						av_image_copy(s.cropFrame->data, s.cropFrame->linesize, src,
							      yf->linesize, AV_PIX_FMT_YUV420P, cw, ch);
						toEnc = s.cropFrame;
					}
					toEnc->pts = scaledPts(pts);
					processedMs = std::max(processedMs, (pts - startV) * av_q2d(vin->time_base) *
										     1000.0);
					if (!encodeVideo(toEnc)) {
						errored = true;
						av_frame_unref(frame);
						break;
					}
					av_frame_unref(frame);
				}
			}
		} else if (ain && pkt->stream_index == aIdx) {
			if (pkt->pts != AV_NOPTS_VALUE && pkt->pts >= startA && pkt->pts <= endA) {
				pkt->stream_index = s.aOut->index;
				// Rebase timestamps so the trimmed clip starts at 0.
				if (pkt->pts != AV_NOPTS_VALUE)
					pkt->pts -= startA;
				if (pkt->dts != AV_NOPTS_VALUE)
					pkt->dts -= startA;
				av_packet_rescale_ts(pkt, ain->time_base, s.aOut->time_base);
				pkt->pos = -1;
				if (av_interleaved_write_frame(s.ofmt, pkt) < 0)
					errored = true;
			}
		}
		av_packet_unref(pkt);

		const auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEmit).count() >= 200) {
			lastEmit = now;
			const double wall =
				std::chrono::duration_cast<std::chrono::milliseconds>(now - startWall).count() /
				1000.0;
			const int pct = totalMs > 0 ? std::clamp(int(processedMs / totalMs * 100.0), 0, 99) : 0;
			const double speed = wall > 0.05 ? (processedMs / 1000.0) / wall : 0.0;
			const qint64 eta = (totalMs > 0 && speed > 0.01)
						   ? qint64((totalMs - processedMs) / 1000.0 / speed * 1000.0)
						   : 0;
			emit progress(pct, eta, s.ofmt->pb ? avio_tell(s.ofmt->pb) : 0);
		}
	}

	if (!errored && !cancel_.load()) {
		avcodec_send_packet(s.vdec, nullptr);
		while (avcodec_receive_frame(s.vdec, frame) >= 0) {
			const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
						    ? frame->best_effort_timestamp
						    : 0;
			if (pts >= startV && pts <= endV) {
				AVFrame *yf = ensureYuvFull(frame);
				AVFrame *toEnc = yf;
				bool ok = true;
				if (cropNeeded) {
					if (av_frame_make_writable(s.cropFrame) == 0) {
						const uint8_t *src[4] = {
							yf->data[0] + cy * yf->linesize[0] + cx,
							yf->data[1] + (cy / 2) * yf->linesize[1] + (cx / 2),
							yf->data[2] + (cy / 2) * yf->linesize[2] + (cx / 2),
							nullptr};
						av_image_copy(s.cropFrame->data, s.cropFrame->linesize, src,
							      yf->linesize, AV_PIX_FMT_YUV420P, cw, ch);
						toEnc = s.cropFrame;
					} else {
						ok = false;
					}
				}
				if (ok) {
					toEnc->pts = scaledPts(pts);
					encodeVideo(toEnc);
				}
			}
			av_frame_unref(frame);
		}
		encodeVideo(nullptr); // flush encoder
		av_write_trailer(s.ofmt);
	}

	av_packet_free(&pkt);
	av_frame_free(&frame);
	av_packet_free(&outPkt);

	if (s.ofmt && s.ofmt->pb && !(s.ofmt->oformat->flags & AVFMT_NOFILE))
		avio_closep(&s.ofmt->pb);

	if (cancel_.load())
		return QString();
	return errored ? QStringLiteral("Encoding failed.") : QString();
}

QString ClipExporter::runVideoCuts(const QString &inPath, const QString &outPath, const Options &opts)
{
	VideoState s;
	const QByteArray in = inPath.toUtf8();
	const QByteArray out = outPath.toUtf8();

	if (avformat_open_input(&s.ifmt, in.constData(), nullptr, nullptr) < 0)
		return QStringLiteral("Could not open the source file.");
	if (avformat_find_stream_info(s.ifmt, nullptr) < 0)
		return QStringLiteral("Could not read the source file.");

	const int vIdx = av_find_best_stream(s.ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (vIdx < 0)
		return QStringLiteral("The file has no video track.");
	AVStream *vin = s.ifmt->streams[vIdx];

	// ---- Normalize the cut list ----
	const qint64 fileDurMs = durationMsOf(s.ifmt);
	std::vector<Cut> cuts;
	cuts.reserve(opts.cuts.size());
	for (Cut c : opts.cuts) {
		if (fileDurMs > 0) {
			c.startMs = std::clamp<qint64>(c.startMs, 0, fileDurMs);
			c.endMs = (c.endMs > 0) ? std::min(c.endMs, fileDurMs) : fileDurMs;
		}
		c.speed = std::clamp(c.speed, 0.05, 50.0);
		if (c.endMs - c.startMs >= 10)
			cuts.push_back(c);
	}
	if (cuts.empty())
		return QStringLiteral("There are no cuts to export.");

	// ---- Video decoder ----
	const AVCodec *vdc = avcodec_find_decoder(vin->codecpar->codec_id);
	if (!vdc)
		return QStringLiteral("Unsupported source video codec.");
	s.vdec = avcodec_alloc_context3(vdc);
	if (!s.vdec || avcodec_parameters_to_context(s.vdec, vin->codecpar) < 0)
		return QStringLiteral("Could not set up the video decoder.");
	s.vdec->pkt_timebase = vin->time_base;
	if (avcodec_open2(s.vdec, vdc, nullptr) < 0)
		return QStringLiteral("Could not open the video decoder.");

	// ---- Resolve crop (default = full frame), even-aligned & clamped ----
	int cx = 0, cy = 0, cw = s.vdec->width, ch = s.vdec->height;
	if (opts.crop && opts.cropW > 0 && opts.cropH > 0) {
		cx = std::clamp(opts.cropX, 0, s.vdec->width - 2);
		cy = std::clamp(opts.cropY, 0, s.vdec->height - 2);
		cw = std::min(opts.cropW, s.vdec->width - cx);
		ch = std::min(opts.cropH, s.vdec->height - cy);
	}
	cx = evenDown(cx);
	cy = evenDown(cy);
	cw = std::max(2, evenDown(cw));
	ch = std::max(2, evenDown(ch));
	// No crop selected (the default) -> decoded frames are fed straight to
	// the encoder; the per-frame full-image copy is skipped entirely.
	const bool cropNeeded = (cx != 0 || cy != 0 || cw != s.vdec->width || ch != s.vdec->height);

	// ---- Video encoder ----
	const char *encName = (opts.format == Format::WebM) ? "libvpx-vp9" : "libx264";
	const AVCodec *vc = avcodec_find_encoder_by_name(encName);
	if (!vc)
		return QStringLiteral("The output video encoder is not available in this build.");
	s.venc = avcodec_alloc_context3(vc);
	if (!s.venc)
		return QStringLiteral("Could not allocate the video encoder.");

	const AVRational fr = av_guess_frame_rate(s.ifmt, vin, nullptr);
	s.venc->width = cw;
	s.venc->height = ch;
	s.venc->pix_fmt = AV_PIX_FMT_YUV420P;
	s.venc->time_base = vin->time_base;
	s.venc->framerate = fr;
	s.venc->gop_size = (fr.num > 0 && fr.den > 0) ? std::max(1, int(av_q2d(fr) * 2.0)) : 60;

	// ---- Output container ----
	if (avformat_alloc_output_context2(&s.ofmt, nullptr, nullptr, out.constData()) < 0 || !s.ofmt)
		return QStringLiteral("Could not create the output file.");
	if (s.ofmt->oformat->flags & AVFMT_GLOBALHEADER)
		s.venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	if (opts.format == Format::WebM) {
		s.venc->bit_rate = 0; // constant-quality VP9
		av_opt_set_int(s.venc->priv_data, "crf", opts.videoCrf, 0);
		av_opt_set(s.venc->priv_data, "deadline", "good", 0);
		av_opt_set_int(s.venc->priv_data, "cpu-used", 5, 0);
		av_opt_set(s.venc->priv_data, "row-mt", "1", 0);
	} else {
		av_opt_set(s.venc->priv_data, "preset", "veryfast", 0);
		av_opt_set(s.venc->priv_data, "profile", "high", 0);
		av_opt_set_int(s.venc->priv_data, "crf", opts.videoCrf, 0);
	}
	if (avcodec_open2(s.venc, vc, nullptr) < 0)
		return QStringLiteral("Could not open the video encoder.");

	s.vOut = avformat_new_stream(s.ofmt, nullptr);
	if (!s.vOut || avcodec_parameters_from_context(s.vOut->codecpar, s.venc) < 0)
		return QStringLiteral("Could not create the output video stream.");
	s.vOut->time_base = s.venc->time_base;

	// ---- Audio: decode → atempo per cut → one continuous AAC track ----
	AudioRetimer retimer;
	int aIdx = (opts.keepAudio && opts.format != Format::WebM)
			   ? av_find_best_stream(s.ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0)
			   : -1;
	AVStream *ain = (aIdx >= 0) ? s.ifmt->streams[aIdx] : nullptr;
	if (ain) {
		QString aerr;
		if (retimer.init(ain->codecpar, ain->time_base.num, ain->time_base.den,
				 (s.ofmt->oformat->flags & AVFMT_GLOBALHEADER) != 0, &aerr)) {
			s.aOut = avformat_new_stream(s.ofmt, nullptr);
			if (!s.aOut ||
			    avcodec_parameters_from_context(s.aOut->codecpar, retimer.encoder()) < 0)
				return QStringLiteral("Could not create the output audio stream.");
			s.aOut->time_base = AVRational{1, retimer.sampleRate()};
		} else {
			ain = nullptr; // no usable audio — export video-only
			aIdx = -1;
		}
	}

	// ---- Scratch frames ----
	s.cropFrame = av_frame_alloc();
	s.cropFrame->format = AV_PIX_FMT_YUV420P;
	s.cropFrame->width = cw;
	s.cropFrame->height = ch;
	if (av_frame_get_buffer(s.cropFrame, 0) < 0)
		return QStringLiteral("Out of memory.");

	// ---- Open + write header (+faststart for MP4) ----
	if (!(s.ofmt->oformat->flags & AVFMT_NOFILE)) {
		if (avio_open(&s.ofmt->pb, out.constData(), AVIO_FLAG_WRITE) < 0)
			return QStringLiteral("Could not open the output file for writing.");
	}
	{
		AVDictionary *mux = nullptr;
		if (opts.format == Format::Mp4 || opts.format == Format::Mov)
			av_dict_set(&mux, "movflags", "+faststart", 0);
		int hr = avformat_write_header(s.ofmt, &mux);
		av_dict_free(&mux);
		if (hr < 0)
			return QStringLiteral("Could not start writing the output file.");
	}
	s.headerWritten = true;

	// ---- Shared encode helpers ----
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	AVPacket *outPkt = av_packet_alloc();
	bool errored = false;
	QString audioErr;

	auto ensureYuvFull = [&](AVFrame *f) -> AVFrame * {
		if ((AVPixelFormat)f->format == AV_PIX_FMT_YUV420P)
			return f;
		if (!s.fullYuv) {
			s.fullYuv = av_frame_alloc();
			s.fullYuv->format = AV_PIX_FMT_YUV420P;
			s.fullYuv->width = s.vdec->width;
			s.fullYuv->height = s.vdec->height;
			av_frame_get_buffer(s.fullYuv, 0);
		}
		if (!s.toYuv)
			s.toYuv = sws_getContext(s.vdec->width, s.vdec->height, (AVPixelFormat)f->format,
						 s.vdec->width, s.vdec->height, AV_PIX_FMT_YUV420P,
						 SWS_BILINEAR, nullptr, nullptr, nullptr);
		av_frame_make_writable(s.fullYuv); // encoder may still hold a ref
		sws_scale(s.toYuv, f->data, f->linesize, 0, s.vdec->height, s.fullYuv->data,
			  s.fullYuv->linesize);
		return s.fullYuv;
	};

	auto encodeVideo = [&](AVFrame *f) -> bool {
		if (avcodec_send_frame(s.venc, f) < 0)
			return false;
		while (true) {
			int r = avcodec_receive_packet(s.venc, outPkt);
			if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
				break;
			if (r < 0)
				return false;
			av_packet_rescale_ts(outPkt, s.venc->time_base, s.vOut->time_base);
			outPkt->stream_index = s.vOut->index;
			if (av_interleaved_write_frame(s.ofmt, outPkt) < 0)
				return false;
			av_packet_unref(outPkt);
		}
		return true;
	};

	auto writeAudio = [&](AVPacket *p) -> bool {
		av_packet_rescale_ts(p, AVRational{1, retimer.sampleRate()}, s.aOut->time_base);
		p->stream_index = s.aOut->index;
		p->pos = -1;
		return av_interleaved_write_frame(s.ofmt, p) >= 0;
	};

	// Output-video pts: each cut's frames land on an accumulating base, with the
	// in-cut offset compressed by that cut's speed. Kept strictly increasing.
	int64_t lastEncPts = -1;
	double outBaseTicks = 0.0; // in vin->time_base units
	double doneOutMs = 0.0;    // completed cuts' output duration (progress)
	double totalOutMs = 0.0;
	for (const Cut &c : cuts)
		totalOutMs += double(c.endMs - c.startMs) / c.speed;

	const auto startWall = std::chrono::steady_clock::now();
	auto lastEmit = startWall;
	double processedMs = 0.0;

	for (size_t ci = 0; ci < cuts.size() && !errored; ++ci) {
		if (cancel_.load())
			break;
		const Cut &cut = cuts[ci];
		const int64_t startV = av_rescale_q(cut.startMs, {1, 1000}, vin->time_base);
		const int64_t endV = av_rescale_q(cut.endMs, {1, 1000}, vin->time_base);
		const int64_t endA = ain ? av_rescale_q(cut.endMs, {1, 1000}, ain->time_base) : 0;

		av_seek_frame(s.ifmt, vIdx, startV, AVSEEK_FLAG_BACKWARD);
		avcodec_flush_buffers(s.vdec);
		if (ain && !retimer.beginSegment(cut.startMs, cut.endMs, cut.speed, &audioErr)) {
			errored = true;
			break;
		}

		bool videoDone = false;
		bool audioDone = (ain == nullptr);

		auto handleDecodedFrame = [&](AVFrame *df) -> bool {
			const int64_t pts = df->best_effort_timestamp != AV_NOPTS_VALUE
						    ? df->best_effort_timestamp
						    : 0;
			if (pts < startV)
				return true;
			if (pts > endV) {
				videoDone = true;
				return true;
			}
			// Crop only when set — otherwise feed the decoded frame straight.
			AVFrame *yf = ensureYuvFull(df);
			AVFrame *toEnc = yf;
			if (cropNeeded) {
				if (av_frame_make_writable(s.cropFrame) < 0)
					return false;
				const uint8_t *src[4] = {yf->data[0] + cy * yf->linesize[0] + cx,
							 yf->data[1] + (cy / 2) * yf->linesize[1] + (cx / 2),
							 yf->data[2] + (cy / 2) * yf->linesize[2] + (cx / 2),
							 nullptr};
				av_image_copy(s.cropFrame->data, s.cropFrame->linesize, src, yf->linesize,
					      AV_PIX_FMT_YUV420P, cw, ch);
				toEnc = s.cropFrame;
			}
			int64_t v = (int64_t)llround(outBaseTicks + double(pts - startV) / cut.speed);
			if (v <= lastEncPts)
				v = lastEncPts + 1;
			lastEncPts = v;
			toEnc->pts = v;
			processedMs = std::max(processedMs,
					       doneOutMs + (pts - startV) * av_q2d(vin->time_base) *
								   1000.0 / cut.speed);
			return encodeVideo(toEnc);
		};

		while (!errored && !(videoDone && audioDone)) {
			if (cancel_.load())
				break;
			if (av_read_frame(s.ifmt, pkt) < 0)
				break; // end of file

			if (pkt->stream_index == vIdx && !videoDone) {
				if (avcodec_send_packet(s.vdec, pkt) >= 0) {
					while (avcodec_receive_frame(s.vdec, frame) >= 0) {
						if (!handleDecodedFrame(frame)) {
							errored = true;
							av_frame_unref(frame);
							break;
						}
						av_frame_unref(frame);
						if (videoDone)
							break;
					}
				}
			} else if (aIdx >= 0 && pkt->stream_index == aIdx && !audioDone) {
				if (pkt->pts != AV_NOPTS_VALUE && pkt->pts > endA) {
					audioDone = true;
				} else if (!retimer.push(pkt, writeAudio, &audioErr)) {
					errored = true;
				}
			}
			av_packet_unref(pkt);

			const auto now = std::chrono::steady_clock::now();
			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEmit)
				    .count() >= 200) {
				lastEmit = now;
				const double wall =
					std::chrono::duration_cast<std::chrono::milliseconds>(
						now - startWall)
						.count() /
					1000.0;
				const int pct = totalOutMs > 0
							? std::clamp(int(processedMs / totalOutMs * 100.0),
								     0, 99)
							: 0;
				const double rate = wall > 0.05 ? (processedMs / 1000.0) / wall : 0.0;
				const qint64 eta =
					(totalOutMs > 0 && rate > 0.01)
						? qint64((totalOutMs - processedMs) / 1000.0 / rate *
							 1000.0)
						: 0;
				emit progress(pct, eta, s.ofmt->pb ? avio_tell(s.ofmt->pb) : 0);
			}
		}

		if (errored || cancel_.load())
			break;

		// Drain the video decoder so the cut's last (reordered) frames are kept,
		// then reset it for the next cut's seek.
		avcodec_send_packet(s.vdec, nullptr);
		while (avcodec_receive_frame(s.vdec, frame) >= 0) {
			const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
						    ? frame->best_effort_timestamp
						    : 0;
			if (pts >= startV && pts <= endV && !videoDone) {
				if (!handleDecodedFrame(frame))
					errored = true;
			}
			av_frame_unref(frame);
		}
		avcodec_flush_buffers(s.vdec);

		if (ain && !retimer.endSegment(writeAudio, &audioErr))
			errored = true;

		outBaseTicks += double(endV - startV) / cut.speed;
		doneOutMs += double(cut.endMs - cut.startMs) / cut.speed;
	}

	if (!errored && !cancel_.load()) {
		if (ain && !retimer.finish(writeAudio, &audioErr))
			errored = true;
		if (!errored) {
			encodeVideo(nullptr); // flush the video encoder
			av_write_trailer(s.ofmt);
		}
	}

	av_packet_free(&pkt);
	av_frame_free(&frame);
	av_packet_free(&outPkt);

	if (s.ofmt && s.ofmt->pb && !(s.ofmt->oformat->flags & AVFMT_NOFILE))
		avio_closep(&s.ofmt->pb);

	if (cancel_.load())
		return QString();
	if (errored)
		return audioErr.isEmpty() ? QStringLiteral("Encoding failed.") : audioErr;
	return QString();
}

} // namespace harpia
