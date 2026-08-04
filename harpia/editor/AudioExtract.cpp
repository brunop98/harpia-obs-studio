#include "AudioExtract.hpp"

#include "core/AudioFileWriter.hpp"

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

} // namespace

bool audioFormatAvailable(AudioFormat f)
{
	const char *enc = encoderNameFor(f);
	if (!enc)
		return true; // WAV: written directly, always possible
	return audioEncoderAvailable(enc);
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
	if (!audioEncoderAvailable(encName)) {
		if (err)
			*err = QStringLiteral("This build cannot write %1 files.")
				       .arg(QString::fromLatin1(audioFormatName(format)));
		return false;
	}

	// The encoder itself lives in core/AudioFileWriter, shared with Audio Only
	// recording, which streams a file too long to hold in memory. Here the
	// whole clip is already decoded, so the pull is a walk through it.
	const qint64 totalFrames = qint64(pcm.size()) / channels;
	qint64 done = 0;
	const auto pull = [&](float *dst, int maxFrames) -> int {
		const int n = int(std::min<qint64>(maxFrames, totalFrames - done));
		if (n <= 0)
			return 0;
		std::memcpy(dst, pcm.data() + done * channels,
			    size_t(n) * size_t(channels) * sizeof(float));
		done += n;
		return n;
	};

	std::string why;
	if (!encodeAudioStream(path.toStdString(), encName, pull, rate, channels, bitrateKbps, &why)) {
		if (err)
			*err = QString::fromStdString(why);
		return false;
	}
	return true;
}

} // namespace harpia
