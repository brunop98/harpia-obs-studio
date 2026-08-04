#include "AudioExtract.hpp"

#include <QFile>
#include <QFileInfo>

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace harpia {

namespace {

// The encoder each format needs. WAV is written by hand -- there is nothing to
// encode, and going through libav for a header and a memcpy buys nothing.
const char *encoderNameFor(AudioFormat f)
{
	switch (f) {
	case AudioFormat::Mp3:
		return "libmp3lame";
	case AudioFormat::M4a:
		return "aac";
	case AudioFormat::Wav:
		return nullptr;
	}
	return nullptr;
}

// Write interleaved float as 16-bit PCM in a RIFF/WAVE container. Same shape as
// the writers in AudioRecorder and VoiceoverMixer; kept local rather than
// shared because those two write their own fixed format and this one has to
// follow whatever rate and channel count the dialog resolved.
bool writeWav(const QString &path, const std::vector<float> &pcm, int rate, int channels,
	      QString *err)
{
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly)) {
		if (err)
			*err = QStringLiteral("Could not create %1").arg(QFileInfo(path).fileName());
		return false;
	}
	const quint32 dataBytes = quint32(pcm.size() * sizeof(qint16));
	const quint32 byteRate = quint32(rate * channels * 2);
	auto u32 = [&](quint32 v) { f.write(reinterpret_cast<const char *>(&v), 4); };
	auto u16 = [&](quint16 v) { f.write(reinterpret_cast<const char *>(&v), 2); };

	f.write("RIFF");
	u32(36 + dataBytes);
	f.write("WAVE");
	f.write("fmt ");
	u32(16);
	u16(1); // PCM
	u16(quint16(channels));
	u32(quint32(rate));
	u32(byteRate);
	u16(quint16(channels * 2)); // block align
	u16(16);                    // bits per sample
	f.write("data");
	u32(dataBytes);

	// In blocks, so a long recording does not need a second full copy of
	// itself in memory just to change sample format.
	std::vector<qint16> block;
	block.reserve(8192);
	for (size_t i = 0; i < pcm.size(); ++i) {
		const float s = std::clamp(pcm[i], -1.0f, 1.0f);
		block.push_back(qint16(std::lround(s * 32767.0f)));
		if (block.size() == 8192) {
			f.write(reinterpret_cast<const char *>(block.data()),
				qint64(block.size() * sizeof(qint16)));
			block.clear();
		}
	}
	if (!block.empty())
		f.write(reinterpret_cast<const char *>(block.data()),
			qint64(block.size() * sizeof(qint16)));
	f.close();
	if (f.error() != QFile::NoError) {
		if (err)
			*err = QStringLiteral("Could not finish writing %1 — the disk may be full.")
				       .arg(QFileInfo(path).fileName());
		return false;
	}
	return true;
}

// The sample format an encoder wants. FFmpeg 7.1 removed AVCodec::sample_fmts
// in favour of avcodec_get_supported_config(), and this builds against both the
// system FFmpeg here and whatever obs-deps ships on Windows -- so ask whichever
// way this header offers, rather than finding out at someone else's build.
AVSampleFormat preferredSampleFormat(const AVCodec *enc, AVCodecContext *ac)
{
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
	const void *cfg = nullptr;
	int count = 0;
	if (avcodec_get_supported_config(ac, enc, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfg, &count) >= 0 &&
	    cfg && count > 0)
		return static_cast<const AVSampleFormat *>(cfg)[0];
#else
	Q_UNUSED(ac);
	if (enc->sample_fmts)
		return enc->sample_fmts[0];
#endif
	// Planar float is what both libmp3lame and the native AAC encoder take, so
	// it is the right guess when the query gives nothing.
	return AV_SAMPLE_FMT_FLTP;
}

} // namespace

bool audioFormatAvailable(AudioFormat f)
{
	const char *enc = encoderNameFor(f);
	if (!enc)
		return true; // WAV: written directly, always possible
	return avcodec_find_encoder_by_name(enc) != nullptr;
}

QStringList availableAudioFormatNames()
{
	QStringList out;
	for (AudioFormat f : {AudioFormat::Mp3, AudioFormat::M4a, AudioFormat::Wav})
		if (audioFormatAvailable(f))
			out << QString::fromLatin1(audioFormatName(f));
	return out;
}

std::vector<float> decodeAudioToPcm(const QString &path, int rate, int channels, QString *err)
{
	std::vector<float> out;
	const QByteArray p = path.toUtf8();
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, p.constData(), nullptr, nullptr) < 0) {
		if (err)
			*err = QStringLiteral("Could not open %1").arg(QFileInfo(path).fileName());
		return out;
	}
	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		avformat_close_input(&fmt);
		if (err)
			*err = QStringLiteral("Could not read %1").arg(QFileInfo(path).fileName());
		return out;
	}
	const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (idx < 0) {
		avformat_close_input(&fmt);
		// Not a failure of ours: plenty of recordings have no microphone and no
		// desktop audio. The caller says so in those words.
		if (err)
			*err = QStringLiteral("no-audio");
		return out;
	}

	AVStream *st = fmt->streams[idx];
	const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
	AVCodecContext *dc = dec ? avcodec_alloc_context3(dec) : nullptr;
	if (!dc || avcodec_parameters_to_context(dc, st->codecpar) < 0 ||
	    avcodec_open2(dc, dec, nullptr) < 0) {
		if (dc)
			avcodec_free_context(&dc);
		avformat_close_input(&fmt);
		if (err)
			*err = QStringLiteral("No decoder for this audio track.");
		return out;
	}
	if (dc->ch_layout.nb_channels <= 0)
		av_channel_layout_default(&dc->ch_layout, 1);

	AVChannelLayout outLayout;
	av_channel_layout_default(&outLayout, channels);
	SwrContext *swr = nullptr;
	if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, rate, &dc->ch_layout,
				dc->sample_fmt, dc->sample_rate, 0, nullptr) < 0 ||
	    swr_init(swr) < 0) {
		if (swr)
			swr_free(&swr);
		avcodec_free_context(&dc);
		avformat_close_input(&fmt);
		if (err)
			*err = QStringLiteral("Could not set up audio conversion.");
		return out;
	}

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	std::vector<float> chunk;
	const auto drain = [&](AVFrame *f) {
		// Worst case for the output size: swr may hold samples back, so ask it
		// rather than computing from the input count.
		const int maxOut = int(swr_get_out_samples(swr, f ? f->nb_samples : 0)) + 256;
		chunk.resize(size_t(maxOut) * channels);
		uint8_t *dst = reinterpret_cast<uint8_t *>(chunk.data());
		const int got = swr_convert(swr, &dst, maxOut,
					    f ? const_cast<const uint8_t **>(f->extended_data) : nullptr,
					    f ? f->nb_samples : 0);
		if (got > 0)
			out.insert(out.end(), chunk.begin(), chunk.begin() + size_t(got) * channels);
	};

	while (av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == idx && avcodec_send_packet(dc, pkt) >= 0) {
			while (avcodec_receive_frame(dc, frame) >= 0) {
				drain(frame);
				av_frame_unref(frame);
			}
		}
		av_packet_unref(pkt);
	}
	// Flush the decoder, then the resampler: without both, the tail of the
	// recording is silently missing -- a second or so on a long file, which
	// looks like a trim gone wrong rather than a missing flush.
	avcodec_send_packet(dc, nullptr);
	while (avcodec_receive_frame(dc, frame) >= 0) {
		drain(frame);
		av_frame_unref(frame);
	}
	drain(nullptr);

	av_frame_free(&frame);
	av_packet_free(&pkt);
	swr_free(&swr);
	avcodec_free_context(&dc);
	avformat_close_input(&fmt);

	if (out.empty() && err)
		*err = QStringLiteral("This file's audio track is empty.");
	return out;
}

std::vector<float> retimePcm(const std::vector<float> &pcm, int rate, int channels, double speed,
			     QString *err)
{
	speed = std::clamp(speed, AudioEdit::kMinSpeed, AudioEdit::kMaxSpeed);
	if (pcm.empty() || std::abs(speed - 1.0) <= 0.001)
		return pcm;

	// atempo only accepts 0.5..100 per instance, so a factor outside that is
	// built from a chain. Same approach as the voiceover mixer's stretch.
	QStringList steps;
	double remaining = speed;
	while (remaining > 2.0) {
		steps << QStringLiteral("atempo=2.0");
		remaining /= 2.0;
	}
	while (remaining < 0.5) {
		steps << QStringLiteral("atempo=0.5");
		remaining /= 0.5;
	}
	steps << QStringLiteral("atempo=%1").arg(remaining, 0, 'f', 6);

	AVFilterGraph *graph = avfilter_graph_alloc();
	if (!graph) {
		if (err)
			*err = QStringLiteral("Could not set up the speed filter.");
		return pcm;
	}
	AVChannelLayout layout;
	av_channel_layout_default(&layout, channels);
	char layoutName[64] = {0};
	av_channel_layout_describe(&layout, layoutName, sizeof(layoutName));

	const QString srcArgs = QStringLiteral("sample_rate=%1:sample_fmt=flt:channel_layout=%2:time_base=1/%1")
					.arg(rate)
					.arg(QString::fromLatin1(layoutName));
	AVFilterContext *src = nullptr, *sink = nullptr;
	bool okGraph = avfilter_graph_create_filter(&src, avfilter_get_by_name("abuffer"), "in",
						    srcArgs.toUtf8().constData(), nullptr, graph) >= 0 &&
		       avfilter_graph_create_filter(&sink, avfilter_get_by_name("abuffersink"), "out",
						    nullptr, nullptr, graph) >= 0;
	AVFilterContext *prev = src;
	for (int i = 0; okGraph && i < steps.size(); ++i) {
		const QStringList kv = steps[i].split(QLatin1Char('='));
		AVFilterContext *f = nullptr;
		okGraph = avfilter_graph_create_filter(&f, avfilter_get_by_name("atempo"),
						       QStringLiteral("t%1").arg(i).toUtf8().constData(),
						       kv.value(1).toUtf8().constData(), nullptr,
						       graph) >= 0 &&
			  avfilter_link(prev, 0, f, 0) >= 0;
		prev = f;
	}
	okGraph = okGraph && avfilter_link(prev, 0, sink, 0) >= 0 &&
		  avfilter_graph_config(graph, nullptr) >= 0;
	if (!okGraph) {
		avfilter_graph_free(&graph);
		if (err)
			*err = QStringLiteral("Could not set up the speed filter.");
		return pcm;
	}

	AVFrame *in = av_frame_alloc();
	in->nb_samples = int(pcm.size() / channels);
	in->format = AV_SAMPLE_FMT_FLT;
	in->sample_rate = rate;
	av_channel_layout_copy(&in->ch_layout, &layout);
	std::vector<float> out;
	if (av_frame_get_buffer(in, 0) >= 0) {
		std::memcpy(in->data[0], pcm.data(), pcm.size() * sizeof(float));
		if (av_buffersrc_add_frame(src, in) >= 0) {
			// Flush: without it the filter holds the tail back and the
			// last fraction of a second never comes out.
			if (av_buffersrc_add_frame(src, nullptr) < 0 && err)
				*err = QStringLiteral("The speed change did not finish cleanly.");
			AVFrame *got = av_frame_alloc();
			while (av_buffersink_get_frame(sink, got) >= 0) {
				const float *d = reinterpret_cast<const float *>(got->data[0]);
				out.insert(out.end(), d, d + size_t(got->nb_samples) * channels);
				av_frame_unref(got);
			}
			av_frame_free(&got);
		}
	}
	av_frame_free(&in);
	avfilter_graph_free(&graph);

	if (out.empty()) {
		// Better to export at the original speed than to export nothing.
		if (err)
			*err = QStringLiteral("The speed change failed; exported at normal speed.");
		return pcm;
	}
	return out;
}

bool encodeAudioFile(const QString &path, AudioFormat format, const std::vector<float> &pcm,
		     int rate, int channels, int bitrateKbps, QString *err)
{
	if (pcm.empty()) {
		if (err)
			*err = QStringLiteral("There is nothing to export — the selection is empty.");
		return false;
	}
	if (format == AudioFormat::Wav)
		return writeWav(path, pcm, rate, channels, err);

	const char *encName = encoderNameFor(format);
	const AVCodec *enc = encName ? avcodec_find_encoder_by_name(encName) : nullptr;
	if (!enc) {
		if (err)
			*err = QStringLiteral("This build cannot write %1 files.")
				       .arg(QString::fromLatin1(audioFormatName(format)));
		return false;
	}

	const QByteArray p = path.toUtf8();
	AVFormatContext *fmt = nullptr;
	if (avformat_alloc_output_context2(&fmt, nullptr, nullptr, p.constData()) < 0 || !fmt) {
		if (err)
			*err = QStringLiteral("Could not create %1").arg(QFileInfo(path).fileName());
		return false;
	}
	AVStream *st = avformat_new_stream(fmt, nullptr);
	AVCodecContext *ac = avcodec_alloc_context3(enc);
	if (!st || !ac) {
		if (ac)
			avcodec_free_context(&ac);
		avformat_free_context(fmt);
		if (err)
			*err = QStringLiteral("Could not set up the audio encoder.");
		return false;
	}

	// The encoder picks its own sample format; the resampler below converts
	// into it. Hard-coding interleaved float here works for AAC and fails for
	// MP3, which wants planar.
	ac->sample_fmt = preferredSampleFormat(enc, ac);
	ac->sample_rate = rate;
	ac->bit_rate = qint64(std::clamp(bitrateKbps, 32, 512)) * 1000;
	av_channel_layout_default(&ac->ch_layout, channels);
	if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
		ac->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	QString failure;
	SwrContext *swr = nullptr;
	AVFrame *frame = nullptr;
	AVPacket *pkt = nullptr;
	bool wroteHeader = false;

	const auto fail = [&](const QString &why) {
		failure = why;
		return false;
	};

	bool okAll = true;
	if (avcodec_open2(ac, enc, nullptr) < 0)
		okAll = fail(QStringLiteral("Could not start the audio encoder."));
	if (okAll && avcodec_parameters_from_context(st->codecpar, ac) < 0)
		okAll = fail(QStringLiteral("Could not describe the audio stream."));
	if (okAll) {
		st->time_base = AVRational{1, rate};
		if (!(fmt->oformat->flags & AVFMT_NOFILE) &&
		    avio_open(&fmt->pb, p.constData(), AVIO_FLAG_WRITE) < 0)
			okAll = fail(QStringLiteral("Could not create %1")
					     .arg(QFileInfo(path).fileName()));
	}
	if (okAll && avformat_write_header(fmt, nullptr) < 0)
		okAll = fail(QStringLiteral("Could not write the file header."));
	else if (okAll)
		wroteHeader = true;

	if (okAll) {
		AVChannelLayout inLayout;
		av_channel_layout_default(&inLayout, channels);
		if (swr_alloc_set_opts2(&swr, &ac->ch_layout, ac->sample_fmt, rate, &inLayout,
					AV_SAMPLE_FMT_FLT, rate, 0, nullptr) < 0 ||
		    swr_init(swr) < 0)
			okAll = fail(QStringLiteral("Could not set up audio conversion."));
	}

	// Some encoders take any frame size; the ones that do not state it.
	const int frameSize = (okAll && ac->frame_size > 0) ? ac->frame_size : 1024;
	if (okAll) {
		frame = av_frame_alloc();
		pkt = av_packet_alloc();
		frame->format = ac->sample_fmt;
		frame->sample_rate = rate;
		frame->nb_samples = frameSize;
		av_channel_layout_copy(&frame->ch_layout, &ac->ch_layout);
		if (!frame || !pkt || av_frame_get_buffer(frame, 0) < 0)
			okAll = fail(QStringLiteral("Out of memory preparing the audio."));
	}

	const auto writeEncoded = [&]() {
		while (avcodec_receive_packet(ac, pkt) >= 0) {
			pkt->stream_index = st->index;
			av_packet_rescale_ts(pkt, AVRational{1, rate}, st->time_base);
			av_interleaved_write_frame(fmt, pkt);
			av_packet_unref(pkt);
		}
	};

	if (okAll) {
		const qint64 totalFrames = qint64(pcm.size()) / channels;
		qint64 done = 0;
		qint64 pts = 0;
		while (done < totalFrames) {
			const int n = int(std::min<qint64>(frameSize, totalFrames - done));
			if (av_frame_make_writable(frame) < 0)
				break;
			const uint8_t *src =
				reinterpret_cast<const uint8_t *>(pcm.data() + done * channels);
			// A short final frame is normal; the encoder pads it.
			frame->nb_samples = n;
			swr_convert(swr, frame->extended_data, n, &src, n);
			frame->pts = pts;
			pts += n;
			done += n;
			if (avcodec_send_frame(ac, frame) >= 0)
				writeEncoded();
		}
		avcodec_send_frame(ac, nullptr);
		writeEncoded();
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

	if (!okAll) {
		QFile::remove(path); // no half-written file left behind
		if (err)
			*err = failure;
		return false;
	}
	return true;
}

} // namespace harpia
