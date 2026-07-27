#include "AudioPreview.hpp"

#include "TimelineAudio.hpp"

#include <QAudioDevice>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>

#include <algorithm>
#include <cstring>

namespace harpia {

// Streams the mix to the device. Past the end it returns silence rather than
// closing, so a timeline whose audio stops before its picture does keeps
// playing (and keeps clocking) to the end.
class AudioPreview::Reader : public QIODevice {
public:
	Reader(const std::vector<float> &pcm, qint64 startFrame)
		: pcm_(pcm), pos_(startFrame * TimelineAudio::kChannels)
	{
	}

	bool isSequential() const override { return true; }
	qint64 bytesAvailable() const override { return 1 << 20; } // always ready

	bool atEndOfMix() const { return pos_ >= qint64(pcm_.size()); }

protected:
	qint64 readData(char *data, qint64 maxSize) override
	{
		const qint64 wantSamples = maxSize / qint64(sizeof(float));
		if (wantSamples <= 0)
			return 0;
		const qint64 have = std::clamp<qint64>(qint64(pcm_.size()) - pos_, 0, wantSamples);
		if (have > 0)
			std::memcpy(data, pcm_.data() + pos_, size_t(have) * sizeof(float));
		if (have < wantSamples) // ran off the end: feed silence, don't stop
			std::memset(data + have * qint64(sizeof(float)), 0,
				    size_t(wantSamples - have) * sizeof(float));
		pos_ += wantSamples;
		return wantSamples * qint64(sizeof(float));
	}
	qint64 writeData(const char *, qint64) override { return -1; }

private:
	const std::vector<float> &pcm_;
	qint64 pos_ = 0; // in samples (not frames)
};

AudioPreview::AudioPreview(QObject *parent) : QObject(parent)
{
	format_.setSampleRate(TimelineAudio::kRate);
	format_.setChannelCount(TimelineAudio::kChannels);
	format_.setSampleFormat(QAudioFormat::Float);
}

AudioPreview::~AudioPreview()
{
	stop();
}

bool AudioPreview::available()
{
	return !QMediaDevices::defaultAudioOutput().isNull();
}

void AudioPreview::setBuffer(std::vector<float> pcm)
{
	stop();
	pcm_ = std::move(pcm);
}

void AudioPreview::clear()
{
	stop();
	pcm_.clear();
	pcm_.shrink_to_fit();
}

bool AudioPreview::hasAudio() const
{
	return !pcm_.empty();
}

qint64 AudioPreview::durationMs() const
{
	return qint64(pcm_.size()) / TimelineAudio::kChannels * 1000 / TimelineAudio::kRate;
}

void AudioPreview::start(qint64 fromMs)
{
	stop();
	if (pcm_.empty() || muted_ || !available())
		return;
	const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
	if (!dev.isFormatSupported(format_))
		return; // no float stereo 48k path: stay silent rather than play noise

	// Starting past the end would only produce silence.
	const qint64 startFrame = std::clamp<qint64>(TimelineAudio::frameForMs(fromMs), 0,
						     qint64(pcm_.size()) / TimelineAudio::kChannels);
	if (startFrame * TimelineAudio::kChannels >= qint64(pcm_.size()))
		return;

	startMs_ = fromMs;
	reader_ = std::make_unique<Reader>(pcm_, startFrame);
	reader_->open(QIODevice::ReadOnly);

	sink_ = std::make_unique<QAudioSink>(dev, format_);
	sink_->setVolume(qreal(std::clamp(volume_, 0.0, 1.0)));
	sink_->start(reader_.get());
}

void AudioPreview::stop()
{
	if (sink_) {
		sink_->stop();
		sink_.reset();
	}
	if (reader_) {
		reader_->close();
		reader_.reset();
	}
}

bool AudioPreview::isPlaying() const
{
	return sink_ && sink_->state() == QAudio::ActiveState;
}

qint64 AudioPreview::positionMs() const
{
	if (!sink_)
		return -1;
	// processedUSecs() counts what the device has actually consumed, which is the
	// only honest answer to "where is the sound right now".
	return startMs_ + qint64(sink_->processedUSecs() / 1000);
}

void AudioPreview::setMuted(bool on)
{
	muted_ = on;
	if (on)
		stop();
}

void AudioPreview::setVolume(double v)
{
	volume_ = std::clamp(v, 0.0, 1.0);
	if (sink_)
		sink_->setVolume(qreal(volume_));
}

} // namespace harpia
