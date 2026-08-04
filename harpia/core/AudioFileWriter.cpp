#include "AudioFileWriter.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace harpia {

namespace {

std::string baseName(const std::string &path)
{
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The encoder picks its own sample format and the resampler converts into it.
// Hard-coding interleaved float here works for AAC and fails for MP3, which
// wants planar -- and the failure is silence, not an error.
AVSampleFormat preferredSampleFormat(const AVCodec *enc, AVCodecContext *ac)
{
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
	const void *cfg = nullptr;
	int count = 0;
	if (avcodec_get_supported_config(ac, enc, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfg, &count) >= 0 &&
	    cfg && count > 0)
		return static_cast<const AVSampleFormat *>(cfg)[0];
#else
	(void)ac;
	if (enc->sample_fmts)
		return enc->sample_fmts[0];
#endif
	// Planar float is what both libmp3lame and the native AAC encoder take, so
	// it is the right guess when the query gives nothing.
	return AV_SAMPLE_FMT_FLTP;
}

} // namespace

bool audioEncoderAvailable(const char *encoderName)
{
	return encoderName && avcodec_find_encoder_by_name(encoderName) != nullptr;
}

bool encodeAudioStream(const std::string &path, const char *encoderName,
		       const std::function<int(float *dst, int maxFrames)> &pull, int rate,
		       int channels, int bitrateKbps, std::string *err)
{
	const auto fail = [&](const std::string &why) {
		if (err)
			*err = why;
		return false;
	};

	if (!pull || rate <= 0 || channels <= 0)
		return fail("Nothing to encode.");

	const AVCodec *enc = encoderName ? avcodec_find_encoder_by_name(encoderName) : nullptr;
	if (!enc)
		return fail(std::string("This build has no '") + (encoderName ? encoderName : "?") +
			    "' audio encoder.");

	AVFormatContext *fmt = nullptr;
	if (avformat_alloc_output_context2(&fmt, nullptr, nullptr, path.c_str()) < 0 || !fmt)
		return fail("Could not create " + baseName(path));

	AVStream *st = avformat_new_stream(fmt, nullptr);
	AVCodecContext *ac = avcodec_alloc_context3(enc);
	if (!st || !ac) {
		if (ac)
			avcodec_free_context(&ac);
		avformat_free_context(fmt);
		return fail("Could not set up the audio encoder.");
	}

	ac->sample_fmt = preferredSampleFormat(enc, ac);
	ac->sample_rate = rate;
	ac->bit_rate = int64_t(std::clamp(bitrateKbps, 32, 512)) * 1000;
	av_channel_layout_default(&ac->ch_layout, channels);
	if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
		ac->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	std::string failure;
	SwrContext *swr = nullptr;
	AVFrame *frame = nullptr;
	AVPacket *pkt = nullptr;
	bool wroteHeader = false;
	bool ok = true;

	// Each step runs only if the previous one worked, so a failed
	// avcodec_open2 is never followed by a call that reads what it did not
	// produce.
	const auto step = [&](const char *why, bool cond) {
		if (!cond) {
			ok = false;
			failure = why;
		}
		return ok;
	};

	ok = step("Could not start the audio encoder.", avcodec_open2(ac, enc, nullptr) >= 0);
	if (ok)
		ok = step("Could not describe the audio stream.",
			  avcodec_parameters_from_context(st->codecpar, ac) >= 0);
	if (ok) {
		st->time_base = AVRational{1, rate};
		if (!(fmt->oformat->flags & AVFMT_NOFILE))
			ok = step("Could not create the output file.",
				  avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0);
	}
	if (ok) {
		ok = step("Could not write the file header.", avformat_write_header(fmt, nullptr) >= 0);
		wroteHeader = ok;
	}
	if (ok) {
		AVChannelLayout inLayout;
		av_channel_layout_default(&inLayout, channels);
		ok = step("Could not set up audio conversion.",
			  swr_alloc_set_opts2(&swr, &ac->ch_layout, ac->sample_fmt, rate, &inLayout,
					      AV_SAMPLE_FMT_FLT, rate, 0, nullptr) >= 0 &&
				  swr_init(swr) >= 0);
	}

	// Some encoders take any frame size; the ones that do not state it.
	const int frameSize = (ok && ac->frame_size > 0) ? ac->frame_size : 1024;
	std::vector<float> block;
	if (ok) {
		frame = av_frame_alloc();
		pkt = av_packet_alloc();
		if (frame) {
			frame->format = ac->sample_fmt;
			frame->sample_rate = rate;
			frame->nb_samples = frameSize;
			av_channel_layout_copy(&frame->ch_layout, &ac->ch_layout);
		}
		ok = step("Out of memory preparing the audio.",
			  frame && pkt && av_frame_get_buffer(frame, 0) >= 0);
		if (ok)
			block.resize(size_t(frameSize) * size_t(channels));
	}

	const auto drain = [&]() {
		while (avcodec_receive_packet(ac, pkt) >= 0) {
			pkt->stream_index = st->index;
			av_packet_rescale_ts(pkt, AVRational{1, rate}, st->time_base);
			av_interleaved_write_frame(fmt, pkt);
			av_packet_unref(pkt);
		}
	};

	int64_t pts = 0;
	if (ok) {
		for (;;) {
			const int n = pull(block.data(), frameSize);
			if (n <= 0)
				break;
			if (av_frame_make_writable(frame) < 0)
				break;
			const uint8_t *src = reinterpret_cast<const uint8_t *>(block.data());
			// A short final frame is normal; the encoder pads it.
			frame->nb_samples = n;
			swr_convert(swr, frame->extended_data, n, &src, n);
			frame->pts = pts;
			pts += n;
			if (avcodec_send_frame(ac, frame) >= 0)
				drain();
		}
		avcodec_send_frame(ac, nullptr);
		drain();
	}

	if (wroteHeader)
		av_write_trailer(fmt);
	if (frame)
		av_frame_free(&frame);
	if (pkt)
		av_packet_free(&pkt);
	if (swr)
		swr_free(&swr);
	avcodec_free_context(&ac);
	if (fmt->pb && !(fmt->oformat->flags & AVFMT_NOFILE))
		avio_closep(&fmt->pb);
	avformat_free_context(fmt);

	if (!ok) {
		std::remove(path.c_str()); // no half-written file left behind
		return fail(failure);
	}
	return true;
}

} // namespace harpia
