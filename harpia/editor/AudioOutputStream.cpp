#include "AudioOutputStream.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <QByteArray>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace harpia {
namespace {
constexpr int kInputChunkFrames = 1024;
constexpr int kMaxScratchBytes = 4 * 1024 * 1024;
AVSampleFormat sampleFormat(QAudioFormat::SampleFormat format)
{
	switch (format) {
	case QAudioFormat::UInt8: return AV_SAMPLE_FMT_U8;
	case QAudioFormat::Int16: return AV_SAMPLE_FMT_S16;
	case QAudioFormat::Int32: return AV_SAMPLE_FMT_S32;
	case QAudioFormat::Float: return AV_SAMPLE_FMT_FLT;
	default: return AV_SAMPLE_FMT_NONE;
	}
}
QAudioFormat::ChannelConfig effectiveChannels(const QAudioFormat &format)
{
	return format.channelConfig() != QAudioFormat::ChannelConfigUnknown ? format.channelConfig()
		: QAudioFormat::defaultChannelConfigForChannelCount(format.channelCount());
}
uint64_t channelMask(const QAudioFormat &format)
{
	// Qt orders samples by increasing channel position. These matching AV
	// positions have the same order, including the later height/LFE channels.
	constexpr AVChannel positions[] = {AV_CHAN_FRONT_LEFT, AV_CHAN_FRONT_RIGHT, AV_CHAN_FRONT_CENTER,
		AV_CHAN_LOW_FREQUENCY, AV_CHAN_BACK_LEFT, AV_CHAN_BACK_RIGHT, AV_CHAN_FRONT_LEFT_OF_CENTER,
		AV_CHAN_FRONT_RIGHT_OF_CENTER, AV_CHAN_BACK_CENTER, AV_CHAN_SIDE_LEFT, AV_CHAN_SIDE_RIGHT,
		AV_CHAN_TOP_CENTER, AV_CHAN_TOP_FRONT_LEFT, AV_CHAN_TOP_FRONT_CENTER, AV_CHAN_TOP_FRONT_RIGHT,
		AV_CHAN_TOP_BACK_LEFT, AV_CHAN_TOP_BACK_CENTER, AV_CHAN_TOP_BACK_RIGHT, AV_CHAN_LOW_FREQUENCY_2,
		AV_CHAN_TOP_SIDE_LEFT, AV_CHAN_TOP_SIDE_RIGHT, AV_CHAN_BOTTOM_FRONT_CENTER,
		AV_CHAN_BOTTOM_FRONT_LEFT, AV_CHAN_BOTTOM_FRONT_RIGHT};
	const quint32 channels = effectiveChannels(format);
	uint64_t mask = 0;
	for (int i = 0; i < int(sizeof(positions) / sizeof(positions[0])); ++i)
		if (channels & (quint32(1) << (i + 1)))
			mask |= uint64_t(1) << positions[i];
	return mask;
}
QString avError(int result)
{
	char message[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(result, message, sizeof(message));
	return QString::fromUtf8(message);
}
} // namespace

struct AudioOutputStream::Impl {
	const std::vector<float> &pcm;
	QAudioFormat format;
	qint64 frame = 0;
	qint64 totalFrames = 0;
	qint64 byte = 0;
	bool direct = false;
	bool flushed = false;
	SwrContext *resampler = nullptr;
	QByteArray pending;
	qsizetype pendingOffset = 0;
	mutable std::mutex errorMutex;
	QString why;

	Impl(const std::vector<float> &mix, qint64 start, const QAudioFormat &output)
		: pcm(mix), format(output)
	{
		totalFrames = qint64(pcm.size() / kInputChannels);
		frame = std::clamp<qint64>(start, 0, totalFrames);
		byte = frame * kInputChannels * qint64(sizeof(float));
		if (!canConvertTo(format)) {
			fail(QStringLiteral("The selected audio output format cannot be converted safely."));
			return;
		}
		direct = format.sampleRate() == kInputRate && format.channelCount() == kInputChannels &&
			format.sampleFormat() == QAudioFormat::Float &&
			effectiveChannels(format) == QAudioFormat::ChannelConfigStereo;
		if (direct)
			return;
		AVChannelLayout input{}, outputLayout{};
		av_channel_layout_default(&input, kInputChannels);
		av_channel_layout_from_mask(&outputLayout, channelMask(format));
		int result = swr_alloc_set_opts2(&resampler, &outputLayout, sampleFormat(format.sampleFormat()),
			format.sampleRate(), &input, AV_SAMPLE_FMT_FLT, kInputRate, 0, nullptr);
		av_channel_layout_uninit(&input);
		av_channel_layout_uninit(&outputLayout);
		if (result >= 0 && resampler)
			result = swr_init(resampler);
		if (result < 0 || !resampler)
			fail(QStringLiteral("Could not prepare audio output conversion: %1").arg(avError(result)));
	}
	~Impl() { swr_free(&resampler); }
	void fail(const QString &message)
	{
		std::lock_guard<std::mutex> lock(errorMutex);
		why = message;
	}
	QString error() const
	{
		std::lock_guard<std::mutex> lock(errorMutex);
		return why;
	}
	bool refill()
	{
		pending.resize(0);
		pendingOffset = 0;
		while (!flushed) {
			const int inputFrames = int(std::min<qint64>(kInputChunkFrames, totalFrames - frame));
			const int capacity = swr_get_out_samples(resampler, inputFrames);
			const qint64 capacityBytes = qint64(capacity) * format.bytesPerFrame();
			if (inputFrames == 0 && capacity == 0) {
				flushed = true;
				return true;
			}
			if (capacity < 0 || capacityBytes <= 0 || capacityBytes > kMaxScratchBytes) {
				fail(QStringLiteral("The audio converter requested an unsupported output buffer."));
				return false;
			}
			pending.resize(qsizetype(capacityBytes));
			uint8_t *out = reinterpret_cast<uint8_t *>(pending.data());
			const uint8_t *in = inputFrames > 0
				? reinterpret_cast<const uint8_t *>(pcm.data() + frame * kInputChannels) : nullptr;
			const int got = swr_convert(resampler, &out, capacity, inputFrames > 0 ? &in : nullptr, inputFrames);
			if (got < 0) {
				pending.clear();
				fail(QStringLiteral("Audio output conversion failed: %1").arg(avError(got)));
				return false;
			}
			frame += inputFrames;
			pending.resize(got * format.bytesPerFrame());
			if (got > 0)
				return true;
			if (inputFrames == 0)
				flushed = true;
		}
		return true;
	}
};

AudioOutputStream::AudioOutputStream(const std::vector<float> &pcm, qint64 startFrame, const QAudioFormat &format)
	: d_(std::make_unique<Impl>(pcm, startFrame, format)) {}
AudioOutputStream::~AudioOutputStream() = default;

QAudioFormat AudioOutputStream::mixFormat()
{
	QAudioFormat out;
	out.setSampleRate(kInputRate);
	out.setChannelCount(kInputChannels);
	out.setSampleFormat(QAudioFormat::Float);
	return out;
}

bool AudioOutputStream::canConvertTo(const QAudioFormat &format)
{
	// Qt's packed PCM formats; bounded to the rates/channel layouts supported
	// by this preview path. Negotiation can still choose stereo on a wider device.
	return format.isValid() && format.sampleRate() >= 8000 && format.sampleRate() <= 384000 &&
		format.channelCount() >= 1 && format.channelCount() <= 8 &&
		sampleFormat(format.sampleFormat()) != AV_SAMPLE_FMT_NONE && channelMask(format) != 0;
}

QAudioFormat AudioOutputStream::chooseFormat(const QAudioFormat &preferred,
	const std::function<bool(const QAudioFormat &)> &supported)
{
	const QAudioFormat original = mixFormat();
	if (supported(original))
		return original;
	if (canConvertTo(preferred) && supported(preferred))
		return preferred;
	for (const int rate : {48000, 44100})
		for (const int channels : {2, 1})
			for (const auto sample : {QAudioFormat::Float, QAudioFormat::Int16, QAudioFormat::Int32, QAudioFormat::UInt8}) {
				QAudioFormat candidate;
				candidate.setSampleRate(rate);
				candidate.setChannelCount(channels);
				candidate.setSampleFormat(sample);
				if (supported(candidate))
					return candidate;
			}
	return {};
}

bool AudioOutputStream::isValid() const { return d_->error().isEmpty(); }
QString AudioOutputStream::error() const { return d_->error(); }

qint64 AudioOutputStream::read(char *data, qint64 maxBytes)
{
	if (maxBytes <= 0)
		return 0;
	if (!data) {
		d_->fail(QStringLiteral("The audio output buffer is invalid."));
		return -1;
	}
	if (!isValid())
		return -1;
	if (d_->direct) {
		const qint64 totalBytes = d_->totalFrames * kInputChannels * qint64(sizeof(float));
		const qint64 copy = std::min(maxBytes, totalBytes - d_->byte);
		if (copy > 0)
			std::memcpy(data, reinterpret_cast<const char *>(d_->pcm.data()) + d_->byte, size_t(copy));
		if (copy < maxBytes)
			std::memset(data + copy, 0, size_t(maxBytes - copy));
		d_->byte += copy;
		return maxBytes;
	}
	qint64 written = 0;
	while (written < maxBytes) {
		if (d_->pendingOffset >= d_->pending.size()) {
			if (!d_->refill())
				return -1;
			if (d_->pending.isEmpty()) {
				const int silence = d_->format.sampleFormat() == QAudioFormat::UInt8 ? 0x80 : 0;
				std::memset(data + written, silence, size_t(maxBytes - written));
				return maxBytes;
			}
		}
		const qint64 copy = std::min<qint64>(maxBytes - written, d_->pending.size() - d_->pendingOffset);
		std::memcpy(data + written, d_->pending.constData() + d_->pendingOffset, size_t(copy));
		d_->pendingOffset += qsizetype(copy);
		written += copy;
	}
	return written;
}

bool AudioOutputStream::atEndOfMix() const
{
	return d_->direct ? d_->byte >= d_->totalFrames * kInputChannels * qint64(sizeof(float))
		: d_->flushed && d_->pendingOffset >= d_->pending.size();
}
qsizetype AudioOutputStream::bufferedBytes() const { return d_->pending.size() - d_->pendingOffset; }

} // namespace harpia
