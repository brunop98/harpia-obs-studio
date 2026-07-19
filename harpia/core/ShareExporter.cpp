#include "ShareExporter.hpp"

#include <algorithm>
#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace harpia {

ShareExporter::Options ShareExporter::optionsFor(Level level)
{
	switch (level) {
	case Level::Low:
		// Smallest file: 480p cap, aggressive CRF, fast preset, light audio.
		return {480, 30, "veryfast", 96};
	case Level::High:
		// Best quality that still shrinks the file: 1080p cap, near-visually-
		// lossless CRF, slower preset for efficiency.
		return {1080, 22, "medium", 160};
	case Level::Balanced:
	default:
		// The WhatsApp/general-sharing sweet spot: 720p cap, balanced CRF.
		return {720, 26, "veryfast", 128};
	}
}

QString ShareExporter::levelName(Level level)
{
	switch (level) {
	case Level::Low:
		return QStringLiteral("Low");
	case Level::High:
		return QStringLiteral("High");
	case Level::Balanced:
	default:
		return QStringLiteral("Balanced");
	}
}

ShareExporter::ShareExporter(QObject *parent) : QObject(parent) {}

namespace {

// Round down to an even number — yuv420p / H.264 require even width & height.
int evenDown(int v)
{
	return v & ~1;
}

// All libav state for one transcode, freed in the destructor so every early
// return cleans up correctly.
struct Transcoder {
	AVFormatContext *ifmt = nullptr;
	AVFormatContext *ofmt = nullptr;

	int vInIdx = -1;
	int aInIdx = -1;

	AVCodecContext *vdec = nullptr;
	AVCodecContext *venc = nullptr;
	AVStream *vOut = nullptr;
	SwsContext *sws = nullptr;
	AVFrame *scaled = nullptr; // reusable yuv420p frame fed to the encoder

	bool audioCopy = false;
	AVCodecContext *adec = nullptr;
	AVCodecContext *aenc = nullptr;
	AVStream *aOut = nullptr;
	SwrContext *swr = nullptr;
	AVAudioFifo *afifo = nullptr;
	int64_t aNextPts = 0; // running pts for the re-encoded audio (in aenc time_base)

	bool headerWritten = false;

	~Transcoder()
	{
		if (scaled)
			av_frame_free(&scaled);
		if (sws)
			sws_freeContext(sws);
		if (swr)
			swr_free(&swr);
		if (afifo)
			av_audio_fifo_free(afifo);
		if (vdec)
			avcodec_free_context(&vdec);
		if (venc)
			avcodec_free_context(&venc);
		if (adec)
			avcodec_free_context(&adec);
		if (aenc)
			avcodec_free_context(&aenc);
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

void ShareExporter::run(const QString &inPath, const QString &outPath, Options opts)
{
	cancel_.store(false);
	Transcoder t;
	QString err;

	auto fail = [&](const QString &m) {
		emit finished(false, false, m);
	};

	const QByteArray inUtf8 = inPath.toUtf8();
	const QByteArray outUtf8 = outPath.toUtf8();

	// ---- Input ---------------------------------------------------------
	if (avformat_open_input(&t.ifmt, inUtf8.constData(), nullptr, nullptr) < 0)
		return fail(QStringLiteral("Could not open the source file."));
	if (avformat_find_stream_info(t.ifmt, nullptr) < 0)
		return fail(QStringLiteral("Could not read the source file."));

	t.vInIdx = av_find_best_stream(t.ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (t.vInIdx < 0)
		return fail(QStringLiteral("The file has no video track to optimize."));
	t.aInIdx = av_find_best_stream(t.ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

	AVStream *vin = t.ifmt->streams[t.vInIdx];

	// ---- Video decoder -------------------------------------------------
	const AVCodec *vdecCodec = avcodec_find_decoder(vin->codecpar->codec_id);
	if (!vdecCodec)
		return fail(QStringLiteral("Unsupported video codec in the source."));
	t.vdec = avcodec_alloc_context3(vdecCodec);
	if (!t.vdec || avcodec_parameters_to_context(t.vdec, vin->codecpar) < 0)
		return fail(QStringLiteral("Could not set up the video decoder."));
	t.vdec->pkt_timebase = vin->time_base;
	if (avcodec_open2(t.vdec, vdecCodec, nullptr) < 0)
		return fail(QStringLiteral("Could not open the video decoder."));

	// ---- Target size (cap height, never upscale, keep even dims) --------
	int outW = t.vdec->width;
	int outH = t.vdec->height;
	if (opts.maxHeight > 0 && outH > opts.maxHeight) {
		const double scale = double(opts.maxHeight) / double(outH);
		outH = opts.maxHeight;
		outW = int(t.vdec->width * scale + 0.5);
	}
	outW = std::max(2, evenDown(outW));
	outH = std::max(2, evenDown(outH));

	// ---- Video encoder (libx264, constant quality) ---------------------
	const AVCodec *x264 = avcodec_find_encoder_by_name("libx264");
	if (!x264)
		return fail(QStringLiteral("The H.264 encoder is not available in this build."));
	t.venc = avcodec_alloc_context3(x264);
	if (!t.venc)
		return fail(QStringLiteral("Could not allocate the video encoder."));

	const AVRational fr = av_guess_frame_rate(t.ifmt, vin, nullptr);
	t.venc->width = outW;
	t.venc->height = outH;
	t.venc->pix_fmt = AV_PIX_FMT_YUV420P;
	t.venc->time_base = vin->time_base; // frame pts pass through in this base
	t.venc->framerate = fr;
	t.venc->gop_size = (fr.num > 0 && fr.den > 0) ? std::max(1, int(av_q2d(fr) * 2.0)) : 60;
	t.venc->sample_aspect_ratio = t.vdec->sample_aspect_ratio;
	// x264 knobs: quality-based encode, tuned preset, broadly-compatible profile.
	av_opt_set(t.venc->priv_data, "preset", opts.preset, 0);
	av_opt_set(t.venc->priv_data, "profile", "high", 0);
	av_opt_set_int(t.venc->priv_data, "crf", opts.crf, 0);

	// ---- Output container ---------------------------------------------
	if (avformat_alloc_output_context2(&t.ofmt, nullptr, nullptr, outUtf8.constData()) < 0 || !t.ofmt)
		return fail(QStringLiteral("Could not create the output file."));
	if (t.ofmt->oformat->flags & AVFMT_GLOBALHEADER)
		t.venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	if (avcodec_open2(t.venc, x264, nullptr) < 0)
		return fail(QStringLiteral("Could not open the H.264 encoder."));

	t.vOut = avformat_new_stream(t.ofmt, nullptr);
	if (!t.vOut || avcodec_parameters_from_context(t.vOut->codecpar, t.venc) < 0)
		return fail(QStringLiteral("Could not create the output video stream."));
	t.vOut->time_base = t.venc->time_base;

	// ---- Scaler --------------------------------------------------------
	t.sws = sws_getContext(t.vdec->width, t.vdec->height, t.vdec->pix_fmt, outW, outH, AV_PIX_FMT_YUV420P,
			       SWS_BILINEAR, nullptr, nullptr, nullptr);
	t.scaled = av_frame_alloc();
	if (!t.sws || !t.scaled)
		return fail(QStringLiteral("Could not set up video scaling."));
	t.scaled->format = AV_PIX_FMT_YUV420P;
	t.scaled->width = outW;
	t.scaled->height = outH;
	if (av_frame_get_buffer(t.scaled, 0) < 0)
		return fail(QStringLiteral("Out of memory allocating a video frame."));

	// ---- Audio: copy if already MP4-friendly (AAC/MP3), else re-encode --
	AVStream *ain = (t.aInIdx >= 0) ? t.ifmt->streams[t.aInIdx] : nullptr;
	if (ain) {
		const AVCodecID aid = ain->codecpar->codec_id;
		t.audioCopy = (aid == AV_CODEC_ID_AAC || aid == AV_CODEC_ID_MP3);
		if (t.audioCopy) {
			t.aOut = avformat_new_stream(t.ofmt, nullptr);
			if (!t.aOut || avcodec_parameters_copy(t.aOut->codecpar, ain->codecpar) < 0)
				return fail(QStringLiteral("Could not copy the audio stream."));
			t.aOut->codecpar->codec_tag = 0;
			t.aOut->time_base = ain->time_base;
		} else {
			// Decode + resample + AAC-encode path.
			const AVCodec *adecCodec = avcodec_find_decoder(aid);
			const AVCodec *aac = avcodec_find_encoder_by_name("aac");
			if (adecCodec && aac) {
				t.adec = avcodec_alloc_context3(adecCodec);
				avcodec_parameters_to_context(t.adec, ain->codecpar);
				t.adec->pkt_timebase = ain->time_base;
				t.aenc = avcodec_alloc_context3(aac);
				t.aenc->sample_rate = t.adec->sample_rate;
				av_channel_layout_copy(&t.aenc->ch_layout, &t.adec->ch_layout);
				t.aenc->sample_fmt = aac->sample_fmts ? aac->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
				t.aenc->bit_rate = (int64_t)opts.audioKbps * 1000;
				t.aenc->time_base = {1, t.adec->sample_rate};
				if (t.ofmt->oformat->flags & AVFMT_GLOBALHEADER)
					t.aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
				if (avcodec_open2(t.adec, adecCodec, nullptr) < 0 ||
				    avcodec_open2(t.aenc, aac, nullptr) < 0)
					return fail(QStringLiteral("Could not set up audio conversion."));
				if (swr_alloc_set_opts2(&t.swr, &t.aenc->ch_layout, t.aenc->sample_fmt,
							t.aenc->sample_rate, &t.adec->ch_layout,
							t.adec->sample_fmt, t.adec->sample_rate, 0,
							nullptr) < 0 ||
				    swr_init(t.swr) < 0)
					return fail(QStringLiteral("Could not set up audio resampling."));
				t.afifo = av_audio_fifo_alloc(t.aenc->sample_fmt, t.aenc->ch_layout.nb_channels, 1);
				t.aOut = avformat_new_stream(t.ofmt, nullptr);
				if (!t.afifo || !t.aOut ||
				    avcodec_parameters_from_context(t.aOut->codecpar, t.aenc) < 0)
					return fail(QStringLiteral("Could not create the output audio stream."));
				t.aOut->time_base = t.aenc->time_base;
			} else {
				// No usable audio path — proceed video-only rather than fail.
				t.aInIdx = -1;
				ain = nullptr;
			}
		}
	}

	// ---- Open file + write header (+faststart) -------------------------
	if (!(t.ofmt->oformat->flags & AVFMT_NOFILE)) {
		if (avio_open(&t.ofmt->pb, outUtf8.constData(), AVIO_FLAG_WRITE) < 0)
			return fail(QStringLiteral("Could not open the output file for writing."));
	}
	{
		AVDictionary *mux = nullptr;
		av_dict_set(&mux, "movflags", "+faststart", 0);
		int hr = avformat_write_header(t.ofmt, &mux);
		av_dict_free(&mux);
		if (hr < 0)
			return fail(QStringLiteral("Could not start writing the output file."));
	}
	t.headerWritten = true;

	// ---- Transcode loop ------------------------------------------------
	const double totalSec = (t.ifmt->duration > 0) ? t.ifmt->duration / (double)AV_TIME_BASE : 0.0;
	const auto startWall = std::chrono::steady_clock::now();
	double processedSec = 0.0;
	int64_t vFallbackPts = 0;
	auto lastEmit = startWall;

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	AVPacket *outPkt = av_packet_alloc();
	if (!pkt || !frame || !outPkt) {
		if (pkt)
			av_packet_free(&pkt);
		if (frame)
			av_frame_free(&frame);
		if (outPkt)
			av_packet_free(&outPkt);
		return fail(QStringLiteral("Out of memory."));
	}

	bool errored = false;
	bool canceled = false;

	// Encode one (already-scaled) video frame and mux the resulting packets.
	auto drainVideoEncoder = [&](AVFrame *f) -> bool {
		if (avcodec_send_frame(t.venc, f) < 0)
			return false;
		while (true) {
			int r = avcodec_receive_packet(t.venc, outPkt);
			if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
				break;
			if (r < 0)
				return false;
			av_packet_rescale_ts(outPkt, t.venc->time_base, t.vOut->time_base);
			outPkt->stream_index = t.vOut->index;
			if (av_interleaved_write_frame(t.ofmt, outPkt) < 0)
				return false;
			av_packet_unref(outPkt);
		}
		return true;
	};

	// Pull full AAC frames out of the FIFO and mux them.
	auto drainAudioFifo = [&](bool flush) -> bool {
		const int fs = t.aenc->frame_size > 0 ? t.aenc->frame_size : 1024;
		while (av_audio_fifo_size(t.afifo) >= fs || (flush && av_audio_fifo_size(t.afifo) > 0)) {
			const int n = std::min(fs, av_audio_fifo_size(t.afifo));
			AVFrame *af = av_frame_alloc();
			af->nb_samples = n;
			af->format = t.aenc->sample_fmt;
			av_channel_layout_copy(&af->ch_layout, &t.aenc->ch_layout);
			af->sample_rate = t.aenc->sample_rate;
			if (av_frame_get_buffer(af, 0) < 0) {
				av_frame_free(&af);
				return false;
			}
			av_audio_fifo_read(t.afifo, (void **)af->data, n);
			af->pts = t.aNextPts;
			t.aNextPts += n;
			if (avcodec_send_frame(t.aenc, af) < 0) {
				av_frame_free(&af);
				return false;
			}
			av_frame_free(&af);
			while (true) {
				int r = avcodec_receive_packet(t.aenc, outPkt);
				if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
					break;
				if (r < 0)
					return false;
				av_packet_rescale_ts(outPkt, t.aenc->time_base, t.aOut->time_base);
				outPkt->stream_index = t.aOut->index;
				if (av_interleaved_write_frame(t.ofmt, outPkt) < 0)
					return false;
				av_packet_unref(outPkt);
			}
		}
		return true;
	};

	while (!errored) {
		if (cancel_.load()) {
			canceled = true;
			break;
		}
		int rr = av_read_frame(t.ifmt, pkt);
		if (rr < 0)
			break; // end of input

		if (pkt->stream_index == t.vInIdx) {
			if (avcodec_send_packet(t.vdec, pkt) >= 0) {
				while (true) {
					int r = avcodec_receive_frame(t.vdec, frame);
					if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
						break;
					if (r < 0) {
						errored = true;
						break;
					}
					if (av_frame_make_writable(t.scaled) < 0) {
						errored = true;
						break;
					}
					sws_scale(t.sws, frame->data, frame->linesize, 0, t.vdec->height,
						  t.scaled->data, t.scaled->linesize);
					int64_t bts = frame->best_effort_timestamp;
					if (bts == AV_NOPTS_VALUE)
						bts = vFallbackPts++;
					t.scaled->pts = bts;
					if (bts != AV_NOPTS_VALUE)
						processedSec = std::max(processedSec, bts * av_q2d(vin->time_base));
					if (!drainVideoEncoder(t.scaled)) {
						errored = true;
						break;
					}
				}
			}
		} else if (t.audioCopy && pkt->stream_index == t.aInIdx) {
			av_packet_rescale_ts(pkt, t.ifmt->streams[t.aInIdx]->time_base, t.aOut->time_base);
			pkt->stream_index = t.aOut->index;
			pkt->pos = -1;
			if (av_interleaved_write_frame(t.ofmt, pkt) < 0)
				errored = true;
		} else if (t.aenc && pkt->stream_index == t.aInIdx) {
			if (avcodec_send_packet(t.adec, pkt) >= 0) {
				while (true) {
					int r = avcodec_receive_frame(t.adec, frame);
					if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
						break;
					if (r < 0) {
						errored = true;
						break;
					}
					uint8_t **buf = nullptr;
					int outLines;
					int outCount = swr_get_out_samples(t.swr, frame->nb_samples);
					if (av_samples_alloc_array_and_samples(&buf, &outLines,
									       t.aenc->ch_layout.nb_channels,
									       outCount, t.aenc->sample_fmt,
									       0) < 0) {
						errored = true;
						break;
					}
					int conv = swr_convert(t.swr, buf, outCount,
							       (const uint8_t **)frame->data, frame->nb_samples);
					if (conv > 0)
						av_audio_fifo_write(t.afifo, (void **)buf, conv);
					if (buf)
						av_freep(&buf[0]);
					av_freep(&buf);
					if (!drainAudioFifo(false)) {
						errored = true;
						break;
					}
				}
			}
		}
		av_packet_unref(pkt);

		// Throttled progress (~4/sec) so the GUI thread isn't flooded.
		const auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEmit).count() >= 250) {
			lastEmit = now;
			const double wall =
				std::chrono::duration_cast<std::chrono::milliseconds>(now - startWall).count() /
				1000.0;
			const double speed = wall > 0.01 ? processedSec / wall : 0.0;
			const int percent = totalSec > 0.0
						    ? std::clamp(int(processedSec / totalSec * 100.0), 0, 99)
						    : 0;
			const qint64 etaMs = (totalSec > 0.0 && speed > 0.01)
						     ? qint64((totalSec - processedSec) / speed * 1000.0)
						     : 0;
			const qint64 bytes = t.ofmt->pb ? avio_tell(t.ofmt->pb) : 0;
			emit progress(percent, speed, etaMs, bytes);
		}
	}

	if (!errored && !canceled) {
		// Flush the decode/encode pipelines.
		avcodec_send_packet(t.vdec, nullptr);
		while (avcodec_receive_frame(t.vdec, frame) >= 0) {
			if (av_frame_make_writable(t.scaled) < 0)
				break;
			sws_scale(t.sws, frame->data, frame->linesize, 0, t.vdec->height, t.scaled->data,
				  t.scaled->linesize);
			int64_t bts = frame->best_effort_timestamp;
			if (bts == AV_NOPTS_VALUE)
				bts = vFallbackPts++;
			t.scaled->pts = bts;
			drainVideoEncoder(t.scaled);
		}
		drainVideoEncoder(nullptr); // flush video encoder

		if (t.aenc) {
			avcodec_send_packet(t.adec, nullptr);
			while (avcodec_receive_frame(t.adec, frame) >= 0) {
				uint8_t **buf = nullptr;
				int outLines;
				int outCount = swr_get_out_samples(t.swr, frame->nb_samples);
				if (av_samples_alloc_array_and_samples(&buf, &outLines,
								       t.aenc->ch_layout.nb_channels, outCount,
								       t.aenc->sample_fmt, 0) >= 0) {
					int conv = swr_convert(t.swr, buf, outCount, (const uint8_t **)frame->data,
							       frame->nb_samples);
					if (conv > 0)
						av_audio_fifo_write(t.afifo, (void **)buf, conv);
					if (buf)
						av_freep(&buf[0]);
					av_freep(&buf);
				}
			}
			drainAudioFifo(true);            // remaining buffered samples
			avcodec_send_frame(t.aenc, nullptr); // flush aac encoder
			while (avcodec_receive_packet(t.aenc, outPkt) >= 0) {
				av_packet_rescale_ts(outPkt, t.aenc->time_base, t.aOut->time_base);
				outPkt->stream_index = t.aOut->index;
				av_interleaved_write_frame(t.ofmt, outPkt);
				av_packet_unref(outPkt);
			}
		}
		av_write_trailer(t.ofmt);
	}

	av_packet_free(&pkt);
	av_frame_free(&frame);
	av_packet_free(&outPkt);

	// Close the output file handle now (Transcoder dtor is a no-op then) so the
	// caller can immediately stat/delete the result.
	if (t.ofmt && t.ofmt->pb && !(t.ofmt->oformat->flags & AVFMT_NOFILE))
		avio_closep(&t.ofmt->pb);

	if (canceled)
		emit finished(false, true, QString());
	else if (errored)
		emit finished(false, false, QStringLiteral("Encoding failed."));
	else
		emit finished(true, false, QString());
}

} // namespace harpia
