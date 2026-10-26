#include "BeatDetector.hpp"

#include <QByteArray>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdlib>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
}

namespace harpia::beat_detection {
namespace {

constexpr int kAnalysisRate = 22050;
// Mono float PCM stays below 160 MB, independently of the source file's
// resolution, channel count and length. Seeked clips do not decode their prefix.
constexpr qint64 kMaxAnalysisMs = 30 * 60 * 1000;

struct Decoder {
	AVFormatContext *format = nullptr;
	AVCodecContext *codec = nullptr;
	AVPacket *packet = nullptr;
	AVFrame *frame = nullptr;
	SwrContext *resample = nullptr;
	~Decoder()
	{
		swr_free(&resample);
		av_frame_free(&frame);
		av_packet_free(&packet);
		avcodec_free_context(&codec);
		avformat_close_input(&format);
	}
};

int interrupted(void *opaque)
{
	return opaque && static_cast<std::atomic<bool> *>(opaque)->load() ? 1 : 0;
}

QString avMessage(int code)
{
	char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(code, buffer, sizeof(buffer));
	return QString::fromUtf8(buffer);
}

} // namespace

Result analyzeFile(const QString &path, qint64 sourceStartMs, qint64 sourceEndMs,
		   const Options &options, std::atomic<bool> *cancel)
{
	Result result;
	const auto canceled = [&]() {
		if (!cancel || !cancel->load())
			return false;
		result.canceled = true;
		result.error.clear();
		return true;
	};
	if (canceled())
		return result;
	if (sourceStartMs < 0 || sourceEndMs <= sourceStartMs ||
	    sourceEndMs > std::numeric_limits<qint64>::max() / kAnalysisRate) {
		result.error = QStringLiteral("This clip has an invalid audio time range.");
		return result;
	}
	if (sourceEndMs - sourceStartMs > kMaxAnalysisMs) {
		result.error = QStringLiteral("Analyze up to 30 minutes of music at once. Trim or split the clip into shorter sections first.");
		return result;
	}
	if (!QFileInfo(path).isFile()) {
		result.error = QStringLiteral("The music file could not be found. Relink the clip and try again.");
		return result;
	}

	Decoder decoder;
	decoder.format = avformat_alloc_context();
	if (!decoder.format) {
		result.error = QStringLiteral("Could not allocate an audio reader.");
		return result;
	}
	decoder.format->interrupt_callback = {interrupted, cancel};
	const QByteArray fileName = path.toUtf8();
	int status = avformat_open_input(&decoder.format, fileName.constData(), nullptr, nullptr);
	if (status < 0) {
		if (!canceled())
			result.error = QStringLiteral("Could not open the music: %1").arg(avMessage(status));
		return result;
	}
	status = avformat_find_stream_info(decoder.format, nullptr);
	if (status < 0) {
		if (!canceled())
			result.error = QStringLiteral("Could not read the music: %1").arg(avMessage(status));
		return result;
	}
	const int streamIndex = av_find_best_stream(decoder.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (streamIndex < 0) {
		result.error = QStringLiteral("This clip has no audio track to analyze.");
		return result;
	}
	AVStream *stream = decoder.format->streams[streamIndex];
	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	decoder.codec = codec ? avcodec_alloc_context3(codec) : nullptr;
	if (!decoder.codec || avcodec_parameters_to_context(decoder.codec, stream->codecpar) < 0 ||
	    avcodec_open2(decoder.codec, codec, nullptr) < 0 || decoder.codec->sample_rate <= 0) {
		result.error = QStringLiteral("This audio format could not be decoded.");
		return result;
	}
	decoder.codec->pkt_timebase = stream->time_base;
	if (decoder.codec->ch_layout.nb_channels <= 0)
		av_channel_layout_default(&decoder.codec->ch_layout, 1);
	AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
	status = swr_alloc_set_opts2(&decoder.resample, &mono, AV_SAMPLE_FMT_FLT, kAnalysisRate,
		&decoder.codec->ch_layout, decoder.codec->sample_fmt, decoder.codec->sample_rate, 0, nullptr);
	if (status < 0 || !decoder.resample || swr_init(decoder.resample) < 0) {
		result.error = QStringLiteral("Could not prepare the music for beat detection.");
		return result;
	}
	decoder.packet = av_packet_alloc();
	decoder.frame = av_frame_alloc();
	if (!decoder.packet || !decoder.frame) {
		result.error = QStringLiteral("Could not allocate an audio buffer.");
		return result;
	}

	const AVRational samplesBase{1, kAnalysisRate};
	const qint64 originUs = decoder.format->start_time != AV_NOPTS_VALUE
		? decoder.format->start_time : 0;
	const qint64 originSamples = av_rescale_q(originUs, AV_TIME_BASE_Q, samplesBase);
	const qint64 firstSample = av_rescale(sourceStartMs, kAnalysisRate, 1000);
	const qint64 endSample = av_rescale(sourceEndMs, kAnalysisRate, 1000);
	const qint64 maxSamples = endSample - firstSample;
	if (sourceStartMs > 0) {
		// A short preroll lets lossy decoders/resamplers settle without decoding
		// minutes of unused audio before a later clip.
		const qint64 seekUs = originUs + std::max<qint64>(0, sourceStartMs - 250) * 1000;
		const qint64 seekTs = av_rescale_q(seekUs, AV_TIME_BASE_Q, stream->time_base);
		status = avformat_seek_file(decoder.format, streamIndex,
			std::numeric_limits<int64_t>::min(), seekTs, seekTs, AVSEEK_FLAG_BACKWARD);
		if (status < 0) {
			if (!canceled())
				result.error = QStringLiteral("Could not seek to this part of the music: %1").arg(avMessage(status));
			return result;
		}
		avcodec_flush_buffers(decoder.codec);
	}

	std::vector<float> pcm;
	std::vector<float> converted;
	qint64 outputPosition = 0;
	qint64 expectedInputEnd = 0;
	bool havePosition = false;
	bool haveAudio = false;
	bool reachedEnd = false;
	bool failed = false;
	const auto append = [&](int count) {
		const qint64 from = std::max<qint64>(0, firstSample - outputPosition);
		const qint64 to = std::min<qint64>(count, endSample - outputPosition);
		if (to > from) {
			const qint64 offset = outputPosition + from - firstSample;
			const qint64 required = std::min(maxSamples, offset + to - from);
			if (required > qint64(pcm.size()))
				pcm.resize(size_t(required), 0.0f);
			for (qint64 i = from; i < to; ++i) {
				const float value = converted[size_t(i)];
				pcm[size_t(offset + i - from)] = std::isfinite(value) ? value : 0.0f;
			}
			haveAudio = true;
		}
		outputPosition += count;
	};
	const auto convert = [&](AVFrame *frame) {
		const int capacity = swr_get_out_samples(decoder.resample, frame ? frame->nb_samples : 0) + 64;
		if (capacity <= 0 || capacity > kAnalysisRate * 10) {
			result.error = QStringLiteral("The music contains an invalid audio frame.");
			return false;
		}
		converted.resize(size_t(capacity));
		uint8_t *destination = reinterpret_cast<uint8_t *>(converted.data());
		const int count = swr_convert(decoder.resample, &destination, capacity,
			frame ? const_cast<const uint8_t **>(frame->extended_data) : nullptr,
			frame ? frame->nb_samples : 0);
		if (count < 0) {
			result.error = QStringLiteral("Could not convert the music: %1").arg(avMessage(count));
			return false;
		}
		append(count);
		return true;
	};
	const auto receive = [&]() {
		while (!canceled()) {
			const int received = avcodec_receive_frame(decoder.codec, decoder.frame);
			if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
				return true;
			if (received < 0) {
				result.error = QStringLiteral("Could not decode the music: %1").arg(avMessage(received));
				return false;
			}
			const int64_t timestamp = decoder.frame->best_effort_timestamp;
			const qint64 position = timestamp != AV_NOPTS_VALUE
				? av_rescale_q(timestamp, stream->time_base, samplesBase) - originSamples
				: havePosition ? expectedInputEnd : 0;
			if (!havePosition || std::llabs(position - expectedInputEnd) > kAnalysisRate / 500) {
				if (havePosition && !convert(nullptr))
					return false;
				if (havePosition) {
					swr_close(decoder.resample);
					if (swr_init(decoder.resample) < 0) {
						result.error = QStringLiteral("Could not resume audio conversion after a timestamp gap.");
						return false;
					}
				}
				outputPosition = position;
				havePosition = true;
			}
			if (position >= endSample) {
				reachedEnd = true;
				av_frame_unref(decoder.frame);
				return true;
			}
			expectedInputEnd = position + av_rescale(decoder.frame->nb_samples,
				kAnalysisRate, decoder.codec->sample_rate);
			const bool convertedOk = convert(decoder.frame);
			av_frame_unref(decoder.frame);
			if (!convertedOk)
				return false;
			if (expectedInputEnd >= endSample) {
				reachedEnd = true;
				return true;
			}
		}
		return true;
	};

	while (!reachedEnd && !canceled()) {
		status = av_read_frame(decoder.format, decoder.packet);
		if (status < 0)
			break;
		if (decoder.packet->stream_index == streamIndex) {
			status = avcodec_send_packet(decoder.codec, decoder.packet);
			if (status == AVERROR(EAGAIN)) {
				if (!receive()) {
					failed = true;
					break;
				}
				status = avcodec_send_packet(decoder.codec, decoder.packet);
			}
			if (status < 0 || !receive()) {
				if (status < 0 && !canceled())
					result.error = QStringLiteral("Could not decode the music: %1").arg(avMessage(status));
				failed = true;
				break;
			}
		}
		av_packet_unref(decoder.packet);
	}
	if (canceled())
		return result;
	if (failed)
		return result;
	if (!reachedEnd) {
		if (status != AVERROR_EOF) {
			result.error = QStringLiteral("Could not finish reading the music: %1").arg(avMessage(status));
			return result;
		}
		avcodec_send_packet(decoder.codec, nullptr);
		if (!receive())
			return result;
	}
	if (havePosition && !convert(nullptr))
		return result;
	if (canceled())
		return result;
	if (!haveAudio || pcm.empty()) {
		result.error = QStringLiteral("No audio samples were found in this part of the clip.");
		return result;
	}
	return detect(pcm, kAnalysisRate, 1, options, cancel);
}

} // namespace harpia::beat_detection
