#include "VoiceoverMixer.hpp"

#include <QFile>
#include <QFileInfo>
#include <QStringList>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>

namespace harpia {

namespace {
constexpr int kRate = 48000;
constexpr int kCh = 2;

// Decode any audio file to interleaved stereo float @ 48 kHz. Empty vector if the
// file has no audio stream or can't be decoded (caller treats that as silence).
std::vector<float> decodeToFloat(const QString &path)
{
	std::vector<float> out;
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0)
		return out;
	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		avformat_close_input(&fmt);
		return out;
	}
	const int aIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (aIdx < 0) {
		avformat_close_input(&fmt);
		return out;
	}
	AVStream *st = fmt->streams[aIdx];
	const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
	AVCodecContext *dc = dec ? avcodec_alloc_context3(dec) : nullptr;
	if (!dc) {
		avformat_close_input(&fmt);
		return out;
	}
	avcodec_parameters_to_context(dc, st->codecpar);
	if (avcodec_open2(dc, dec, nullptr) < 0) {
		avcodec_free_context(&dc);
		avformat_close_input(&fmt);
		return out;
	}

	SwrContext *swr = nullptr;
	AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
	if (dc->ch_layout.nb_channels <= 0)
		av_channel_layout_default(&dc->ch_layout, 1);
	if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, kRate, &dc->ch_layout,
				dc->sample_fmt, dc->sample_rate > 0 ? dc->sample_rate : kRate, 0,
				nullptr) < 0 ||
	    swr_init(swr) < 0) {
		if (swr)
			swr_free(&swr);
		avcodec_free_context(&dc);
		avformat_close_input(&fmt);
		return out;
	}

	AVPacket *pkt = av_packet_alloc();
	AVFrame *fr = av_frame_alloc();
	auto convert = [&](AVFrame *f, bool flush) {
		const int inSamples = flush ? 0 : f->nb_samples;
		const int maxOut = int(swr_get_out_samples(swr, inSamples));
		if (maxOut <= 0)
			return;
		const size_t base = out.size();
		out.resize(base + size_t(maxOut) * kCh);
		uint8_t *op = reinterpret_cast<uint8_t *>(out.data() + base);
		const uint8_t **in = flush ? nullptr : (const uint8_t **)f->extended_data;
		const int got = swr_convert(swr, &op, maxOut, in, inSamples);
		if (got > 0)
			out.resize(base + size_t(got) * kCh);
		else
			out.resize(base);
	};

	while (av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == aIdx && avcodec_send_packet(dc, pkt) >= 0) {
			while (avcodec_receive_frame(dc, fr) >= 0)
				convert(fr, false);
		}
		av_packet_unref(pkt);
	}
	avcodec_send_packet(dc, nullptr);
	while (avcodec_receive_frame(dc, fr) >= 0)
		convert(fr, false);
	convert(fr, true); // flush the resampler

	av_frame_free(&fr);
	av_packet_free(&pkt);
	swr_free(&swr);
	avcodec_free_context(&dc);
	avformat_close_input(&fmt);
	return out;
}
} // namespace

namespace {
// Pitch-preserving time stretch of interleaved stereo float @ 48 kHz, using
// FFmpeg's atempo filter. speed > 1 shortens the audio. atempo only accepts
// 0.5..2.0 per instance, so larger changes are chained.
std::vector<float> atempoStretch(const std::vector<float> &in, double speed)
{
	speed = std::clamp(speed, 0.05, 50.0);
	if (in.empty() || std::abs(speed - 1.0) <= 0.001)
		return in;

	// Factor the requested speed into a chain of legal atempo steps.
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
	if (!graph)
		return in;
	std::vector<float> out;
	AVFilterContext *src = nullptr;
	AVFilterContext *sink = nullptr;
	AVFrame *inFrame = av_frame_alloc();
	AVFrame *outFrame = av_frame_alloc();

	auto cleanup = [&]() {
		if (inFrame)
			av_frame_free(&inFrame);
		if (outFrame)
			av_frame_free(&outFrame);
		avfilter_graph_free(&graph);
	};

	const QString args = QStringLiteral("time_base=1/%1:sample_rate=%1:sample_fmt=flt:"
					    "channel_layout=stereo")
				     .arg(kRate);
	if (avfilter_graph_create_filter(&src, avfilter_get_by_name("abuffer"), "in",
					 args.toUtf8().constData(), nullptr, graph) < 0 ||
	    avfilter_graph_create_filter(&sink, avfilter_get_by_name("abuffersink"), "out", nullptr,
					 nullptr, graph) < 0) {
		cleanup();
		return in;
	}
	// The sink format is pinned with an aformat filter rather than the sink's
	// own sample_fmts option, because setting that needs av_opt_set_int_list,
	// whose deprecated helper MSVC rejects under warnings-as-errors. Same
	// approach as AudioRetimer.

	// Chain: in -> atempo... -> aformat -> out
	AVFilterContext *prev = src;
	for (int i = 0; i < steps.size(); ++i) {
		const QString spec = steps[i];
		const int eq = spec.indexOf(QLatin1Char('='));
		AVFilterContext *f = nullptr;
		if (avfilter_graph_create_filter(&f, avfilter_get_by_name("atempo"),
						 QStringLiteral("t%1").arg(i).toUtf8().constData(),
						 spec.mid(eq + 1).toUtf8().constData(), nullptr,
						 graph) < 0 ||
		    avfilter_link(prev, 0, f, 0) < 0) {
			cleanup();
			return in;
		}
		prev = f;
	}

	AVFilterContext *fmt = nullptr;
	const QString fmtArgs = QStringLiteral("sample_fmts=flt:sample_rates=%1:channel_layouts=stereo")
					.arg(kRate);
	if (avfilter_graph_create_filter(&fmt, avfilter_get_by_name("aformat"), "fmt",
					 fmtArgs.toUtf8().constData(), nullptr, graph) < 0 ||
	    avfilter_link(prev, 0, fmt, 0) < 0) {
		cleanup();
		return in;
	}
	prev = fmt;

	if (avfilter_link(prev, 0, sink, 0) < 0 || avfilter_graph_config(graph, nullptr) < 0) {
		cleanup();
		return in;
	}

	// Push everything in one frame, then flush.
	const int frames = int(in.size() / kCh);
	inFrame->format = AV_SAMPLE_FMT_FLT;
	inFrame->sample_rate = kRate;
	av_channel_layout_from_mask(&inFrame->ch_layout, AV_CH_LAYOUT_STEREO);
	inFrame->nb_samples = frames;
	if (av_frame_get_buffer(inFrame, 0) < 0) {
		cleanup();
		return in;
	}
	std::memcpy(inFrame->data[0], in.data(), in.size() * sizeof(float));
	if (av_buffersrc_add_frame(src, inFrame) < 0) {
		cleanup();
		return in;
	}
	if (av_buffersrc_add_frame(src, nullptr) < 0) { // EOF
		cleanup();
		return in;
	}

	while (av_buffersink_get_frame(sink, outFrame) >= 0) {
		const auto *p = reinterpret_cast<const float *>(outFrame->data[0]);
		out.insert(out.end(), p, p + size_t(outFrame->nb_samples) * kCh);
		av_frame_unref(outFrame);
	}
	cleanup();
	return out.empty() ? in : out;
}
} // namespace

bool VoiceoverMixer::decodeToWav(const QString &inPath, const QString &outWav, double speed)
{
	std::vector<float> s = decodeToFloat(inPath); // interleaved stereo @ 48k
	if (s.empty())
		return false;
	if (std::abs(speed - 1.0) > 0.001)
		s = atempoStretch(s, speed);
	if (s.empty())
		return false;
	const qint64 frames = (qint64)s.size() / kCh;

	QFile f(outWav);
	if (!f.open(QIODevice::WriteOnly))
		return false;
	auto pU32 = [](quint32 v) {
		char b[4] = {char(v & 0xff), char((v >> 8) & 0xff), char((v >> 16) & 0xff),
			     char((v >> 24) & 0xff)};
		return QByteArray(b, 4);
	};
	auto pU16 = [](quint16 v) {
		char b[2] = {char(v & 0xff), char((v >> 8) & 0xff)};
		return QByteArray(b, 2);
	};
	const qint64 dataBytes = frames * kCh * 2;
	f.write("RIFF");
	f.write(pU32(quint32(36 + dataBytes)));
	f.write("WAVE");
	f.write("fmt ");
	f.write(pU32(16));
	f.write(pU16(1));               // PCM
	f.write(pU16(quint16(kCh)));
	f.write(pU32(kRate));
	f.write(pU32(quint32(kRate * kCh * 2)));
	f.write(pU16(quint16(kCh * 2)));
	f.write(pU16(16));
	f.write("data");
	f.write(pU32(quint32(dataBytes)));
	// Float [-1,1] → interleaved S16LE.
	QByteArray pcm;
	pcm.resize(int(dataBytes));
	auto *o = reinterpret_cast<qint16 *>(pcm.data());
	for (qint64 i = 0; i < frames * kCh; ++i)
		o[i] = qint16(std::clamp(s[i], -1.0f, 1.0f) * 32767.0f);
	f.write(pcm);
	f.close();
	return true;
}

void VoiceoverMixer::addTake(std::vector<float> &mix, const std::vector<float> &take,
			     long long startFrame, double volume, int fadeInFrames,
			     int fadeOutFrames, FadeCurve inCurve, FadeCurve outCurve)
{
	const long long frames = (long long)take.size() / kCh;
	for (long long j = 0; j < frames; ++j) {
		const long long o = startFrame + j;
		if (o < 0)
			continue;
		if ((o + 1) * kCh > (long long)mix.size())
			break;
		double f = 1.0;
		if (fadeInFrames > 0 && j < fadeInFrames)
			f *= fadeGain(inCurve, double(j) / fadeInFrames);
		if (fadeOutFrames > 0 && j >= frames - fadeOutFrames)
			f *= fadeGain(outCurve, double(frames - 1 - j) / fadeOutFrames);
		const float g = float(volume * std::clamp(f, 0.0, 1.0));
		for (int c = 0; c < kCh; ++c)
			mix[o * kCh + c] += take[j * kCh + c] * g;
	}
}

std::vector<float> VoiceoverMixer::duckEnvelope(long long totalFrames,
						const std::vector<std::pair<long long, long long>> &spans,
						float duckLevel, int attackFrames, int releaseFrames)
{
	// Target: duckLevel inside any voiceover span, 1.0 elsewhere.
	std::vector<float> target(std::max<long long>(0, totalFrames), 1.0f);
	for (const auto &s : spans) {
		const long long a = std::max<long long>(0, s.first);
		const long long b = std::min<long long>(totalFrames, s.second);
		for (long long i = a; i < b; ++i)
			target[i] = duckLevel;
	}
	// One-pole smoothing with separate fall (attack) / rise (release) rates.
	std::vector<float> env(target.size(), 1.0f);
	const float aC = attackFrames > 0 ? 1.0f / attackFrames : 1.0f;
	const float rC = releaseFrames > 0 ? 1.0f / releaseFrames : 1.0f;
	float g = 1.0f;
	for (size_t i = 0; i < target.size(); ++i) {
		const float t = target[i];
		g += (t < g ? aC : rC) * (t - g);
		env[i] = g;
	}
	return env;
}

std::vector<float> VoiceoverMixer::renderTakes(const std::vector<Take> &takes,
					      std::atomic<bool> *cancel)
{
	auto canceled = [&]() { return cancel && cancel->load(); };
	struct Rendered {
		std::vector<float> samples;
		long long startFrame = 0;
		double volume = 1.0;
		int fadeIn = 0, fadeOut = 0;
		FadeCurve inCurve = FadeCurve::Linear, outCurve = FadeCurve::Linear;
	};
	std::vector<Rendered> rendered;
	rendered.reserve(takes.size());
	long long totalFrames = 0;

	for (const Take &t : takes) {
		if (canceled())
			return {};
		const std::vector<float> full = decodeToFloat(t.path);
		if (full.empty())
			continue;
		// Honor trim/split: take only [srcStart, srcStart+play] of the source.
		const long long totalF = (long long)full.size() / kCh;
		const long long s0 = std::clamp<long long>(t.srcStartMs * kRate / 1000, 0, totalF);
		long long len = t.playMs > 0 ? t.playMs * kRate / 1000 : totalF - s0;
		len = std::clamp<long long>(len, 0, totalF - s0);
		if (len <= 0)
			continue;
		Rendered r;
		r.samples.assign(full.begin() + s0 * kCh, full.begin() + (s0 + len) * kCh);
		r.startFrame = t.outStartMs * kRate / 1000;
		r.volume = t.volume;
		r.fadeIn = std::max(0, t.fadeInMs) * kRate / 1000;
		r.fadeOut = std::max(0, t.fadeOutMs) * kRate / 1000;
		r.inCurve = t.fadeInCurve;
		r.outCurve = t.fadeOutCurve;
		totalFrames = std::max(totalFrames, r.startFrame + len);
		rendered.push_back(std::move(r));
	}
	if (totalFrames <= 0)
		return {};

	std::vector<float> mixbuf(size_t(totalFrames) * kCh, 0.0f);
	for (const Rendered &r : rendered) {
		if (canceled())
			return {};
		addTake(mixbuf, r.samples, r.startFrame, r.volume, r.fadeIn, r.fadeOut,
			r.inCurve, r.outCurve);
	}
	for (float &s : mixbuf)
		s = std::clamp(s, -1.0f, 1.0f);
	return mixbuf;
}

QString VoiceoverMixer::mix(const QString &videoPath, double originalVolume, bool duck,
			    const std::vector<Take> &takes, std::atomic<bool> *cancel)
{
	auto canceled = [&]() { return cancel && cancel->load(); };

	// 1) Decode the base audio and every take to float @ 48k stereo.
	std::vector<float> base = decodeToFloat(videoPath);
	long long totalFrames = (long long)base.size() / kCh;

	struct Rendered {
		std::vector<float> samples;
		long long startFrame = 0;
		double volume = 1.0;
		int fadeIn = 0, fadeOut = 0;
		FadeCurve inCurve = FadeCurve::Linear, outCurve = FadeCurve::Linear;
	};
	std::vector<Rendered> rendered;
	rendered.reserve(takes.size());
	for (const Take &t : takes) {
		if (canceled())
			return QStringLiteral("Canceled.");
		Rendered r;
		std::vector<float> full = decodeToFloat(t.path);
		if (full.empty())
			continue;
		// Honor trim/split: take only [srcStart, srcStart+play] of the source.
		const long long totalF = (long long)full.size() / kCh;
		long long s0 = std::clamp<long long>(t.srcStartMs * kRate / 1000, 0, totalF);
		long long len = t.playMs > 0 ? t.playMs * kRate / 1000 : totalF - s0;
		len = std::clamp<long long>(len, 0, totalF - s0);
		if (len <= 0)
			continue;
		r.samples.assign(full.begin() + s0 * kCh, full.begin() + (s0 + len) * kCh);
		r.startFrame = t.outStartMs * kRate / 1000;
		r.volume = t.volume;
		r.fadeIn = std::max(0, t.fadeInMs) * kRate / 1000;
		r.fadeOut = std::max(0, t.fadeOutMs) * kRate / 1000;
		r.inCurve = t.fadeInCurve;
		r.outCurve = t.fadeOutCurve;
		rendered.push_back(std::move(r));
		const long long end = rendered.back().startFrame +
				      (long long)rendered.back().samples.size() / kCh;
		totalFrames = std::max(totalFrames, end);
	}
	if (totalFrames <= 0)
		return QStringLiteral("Nothing to mix.");

	// 2) Build the mixed buffer: base * original volume * (duck envelope), then
	//    add each take.
	std::vector<float> mixbuf(size_t(totalFrames) * kCh, 0.0f);
	std::vector<float> env;
	if (duck) {
		std::vector<std::pair<long long, long long>> spans;
		for (const Rendered &r : rendered)
			spans.emplace_back(r.startFrame,
					   r.startFrame + (long long)r.samples.size() / kCh);
		env = duckEnvelope(totalFrames, spans, 0.35f, kRate / 20, kRate / 4); // 50ms/250ms
	}
	const long long baseFrames = (long long)base.size() / kCh;
	for (long long i = 0; i < totalFrames; ++i) {
		const float dg = duck ? env[i] : 1.0f;
		for (int c = 0; c < kCh; ++c) {
			float v = (i < baseFrames) ? base[i * kCh + c] : 0.0f;
			mixbuf[i * kCh + c] = v * float(originalVolume) * dg;
		}
	}
	for (const Rendered &r : rendered)
		addTake(mixbuf, r.samples, r.startFrame, r.volume, r.fadeIn, r.fadeOut,
			r.inCurve, r.outCurve);
	for (float &s : mixbuf)
		s = std::clamp(s, -1.0f, 1.0f);
	base.clear();
	base.shrink_to_fit();

	// 3) Re-encode as AAC and remux with the copied video into a temp file.
	const QString tmp = videoPath + QStringLiteral(".vomix.tmp.") +
			    QFileInfo(videoPath).suffix();

	AVFormatContext *in = nullptr;
	if (avformat_open_input(&in, videoPath.toUtf8().constData(), nullptr, nullptr) < 0)
		return QStringLiteral("Voiceover mix: could not reopen the export.");
	avformat_find_stream_info(in, nullptr);
	const int vIdx = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (vIdx < 0) {
		avformat_close_input(&in);
		return QStringLiteral("Voiceover mix: no video stream to keep.");
	}
	AVStream *inV = in->streams[vIdx];

	AVFormatContext *out = nullptr;
	if (avformat_alloc_output_context2(&out, nullptr, nullptr, tmp.toUtf8().constData()) < 0 ||
	    !out) {
		avformat_close_input(&in);
		return QStringLiteral("Voiceover mix: could not create the output.");
	}

	QString err;
	AVStream *outV = avformat_new_stream(out, nullptr);
	AVStream *outA = avformat_new_stream(out, nullptr);
	const AVCodec *aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
	AVCodecContext *enc = aac ? avcodec_alloc_context3(aac) : nullptr;
	AVPacket *pkt = av_packet_alloc();
	AVPacket *vpkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();

	auto fail = [&](const QString &m) -> QString {
		if (enc)
			avcodec_free_context(&enc);
		if (frame)
			av_frame_free(&frame);
		if (pkt)
			av_packet_free(&pkt);
		if (vpkt)
			av_packet_free(&vpkt);
		if (out) {
			if (out->pb && !(out->oformat->flags & AVFMT_NOFILE))
				avio_closep(&out->pb);
			avformat_free_context(out);
		}
		if (in)
			avformat_close_input(&in);
		QFile::remove(tmp);
		return m;
	};

	if (!outV || !outA || !enc)
		return fail(QStringLiteral("Voiceover mix: allocation failed."));

	// Video: straight stream copy.
	if (avcodec_parameters_copy(outV->codecpar, inV->codecpar) < 0)
		return fail(QStringLiteral("Voiceover mix: could not copy the video stream."));
	outV->codecpar->codec_tag = 0;
	outV->time_base = inV->time_base;

	// Audio: AAC @ 48k stereo.
	enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
	enc->sample_rate = kRate;
	av_channel_layout_from_mask(&enc->ch_layout, AV_CH_LAYOUT_STEREO);
	enc->bit_rate = 192000;
	enc->time_base = AVRational{1, kRate};
	if (out->oformat->flags & AVFMT_GLOBALHEADER)
		enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	if (avcodec_open2(enc, aac, nullptr) < 0)
		return fail(QStringLiteral("Voiceover mix: could not open the AAC encoder."));
	avcodec_parameters_from_context(outA->codecpar, enc);
	outA->time_base = enc->time_base;

	if (!(out->oformat->flags & AVFMT_NOFILE) &&
	    avio_open(&out->pb, tmp.toUtf8().constData(), AVIO_FLAG_WRITE) < 0)
		return fail(QStringLiteral("Voiceover mix: could not open the output file."));
	if (avformat_write_header(out, nullptr) < 0)
		return fail(QStringLiteral("Voiceover mix: could not write the header."));

	const int frameSize = enc->frame_size > 0 ? enc->frame_size : 1024;
	long long mixPos = 0; // frame cursor into mixbuf
	const long long mixFrames = (long long)mixbuf.size() / kCh;
	long long aPts = 0;
	bool encFlushed = false;

	// Pull the next encoded audio packet, feeding the encoder on demand. Returns
	// false at EOF.
	auto nextAudio = [&](AVPacket *dst) -> bool {
		for (;;) {
			const int r = avcodec_receive_packet(enc, dst);
			if (r == 0)
				return true;
			if (r == AVERROR_EOF)
				return false;
			if (r != AVERROR(EAGAIN))
				return false;
			// Need more input.
			if (mixPos < mixFrames) {
				const int n = int(std::min<long long>(frameSize, mixFrames - mixPos));
				av_frame_unref(frame);
				frame->nb_samples = n;
				frame->format = AV_SAMPLE_FMT_FLTP;
				av_channel_layout_from_mask(&frame->ch_layout, AV_CH_LAYOUT_STEREO);
				frame->sample_rate = kRate;
				if (av_frame_get_buffer(frame, 0) < 0)
					return false;
				auto *l = reinterpret_cast<float *>(frame->data[0]);
				auto *rr = reinterpret_cast<float *>(frame->data[1]);
				for (int j = 0; j < n; ++j) {
					l[j] = mixbuf[(mixPos + j) * kCh + 0];
					rr[j] = mixbuf[(mixPos + j) * kCh + 1];
				}
				frame->pts = aPts;
				aPts += n;
				mixPos += n;
				avcodec_send_frame(enc, frame);
			} else if (!encFlushed) {
				encFlushed = true;
				avcodec_send_frame(enc, nullptr);
			} else {
				return false;
			}
		}
	};

	// Interleave copied video packets with encoded audio by presentation time.
	bool haveV = (av_read_frame(in, vpkt) >= 0);
	while (haveV && vpkt->stream_index != vIdx) { // skip original audio packets
		av_packet_unref(vpkt);
		haveV = (av_read_frame(in, vpkt) >= 0);
	}
	bool haveA = nextAudio(pkt);

	auto writeVideo = [&]() -> bool {
		av_packet_rescale_ts(vpkt, inV->time_base, outV->time_base);
		vpkt->stream_index = outV->index;
		const bool ok = av_interleaved_write_frame(out, vpkt) >= 0;
		av_packet_unref(vpkt);
		haveV = (av_read_frame(in, vpkt) >= 0);
		while (haveV && vpkt->stream_index != vIdx) {
			av_packet_unref(vpkt);
			haveV = (av_read_frame(in, vpkt) >= 0);
		}
		return ok;
	};
	auto writeAudio = [&]() -> bool {
		av_packet_rescale_ts(pkt, enc->time_base, outA->time_base);
		pkt->stream_index = outA->index;
		const bool ok = av_interleaved_write_frame(out, pkt) >= 0;
		av_packet_unref(pkt);
		haveA = nextAudio(pkt);
		return ok;
	};

	while (haveV || haveA) {
		if (canceled())
			return fail(QStringLiteral("Canceled."));
		if (haveV && haveA) {
			const double vt = vpkt->dts != AV_NOPTS_VALUE
						  ? vpkt->dts * av_q2d(inV->time_base)
						  : 0.0;
			const double at = pkt->pts != AV_NOPTS_VALUE
						  ? pkt->pts * av_q2d(enc->time_base)
						  : 0.0;
			if (vt <= at) {
				if (!writeVideo())
					return fail(QStringLiteral("Voiceover mix: video write failed."));
			} else {
				if (!writeAudio())
					return fail(QStringLiteral("Voiceover mix: audio write failed."));
			}
		} else if (haveV) {
			if (!writeVideo())
				return fail(QStringLiteral("Voiceover mix: video write failed."));
		} else {
			if (!writeAudio())
				return fail(QStringLiteral("Voiceover mix: audio write failed."));
		}
	}

	av_write_trailer(out);

	avcodec_free_context(&enc);
	av_frame_free(&frame);
	av_packet_free(&pkt);
	av_packet_free(&vpkt);
	if (out->pb && !(out->oformat->flags & AVFMT_NOFILE))
		avio_closep(&out->pb);
	avformat_free_context(out);
	avformat_close_input(&in);

	// Replace the export with the mixed version.
	if (!QFile::remove(videoPath) || !QFile::rename(tmp, videoPath)) {
		QFile::remove(tmp);
		return QStringLiteral("Voiceover mix: could not replace the exported file.");
	}
	return QString();
}

} // namespace harpia
