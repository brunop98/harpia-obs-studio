#include "AudioPreview.hpp"

#include "TimelineAudio.hpp"
#include "AudioOutputStream.hpp"

#include <QAudioDevice>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>

#include <algorithm>

namespace harpia {

// Streams the mix to the device. Past the end it returns silence rather than
// closing, so a timeline whose audio stops before its picture does keeps
// playing (and keeps clocking) to the end.
class AudioPreview::Reader : public QIODevice {
public:
	Reader(const std::vector<float> &pcm, qint64 startFrame, const QAudioFormat &format)
		: stream_(pcm, startFrame, format) {}
	bool isSequential() const override { return true; }
	qint64 bytesAvailable() const override { return (1 << 20) + QIODevice::bytesAvailable(); }
	bool isValid() const { return stream_.isValid(); }
	QString conversionError() const { return stream_.error(); }
protected:
	qint64 readData(char *data, qint64 maxSize) override { return stream_.read(data, maxSize); }
	qint64 writeData(const char *, qint64) override { return -1; }
private:
	AudioOutputStream stream_;
};

namespace {
QString sinkError(QAudio::Error error)
{
	switch (error) {
	case QAudio::OpenError: return QStringLiteral("The audio output device could not be opened.");
	case QAudio::IOError: return QStringLiteral("The audio output device stopped accepting sound.");
	case QAudio::UnderrunError: return QStringLiteral("The audio output device ran out of data.");
	case QAudio::FatalError: return QStringLiteral("The audio output device reported a fatal error.");
	default: return QStringLiteral("The audio output device could not start playback.");
	}
}
} // namespace

AudioPreview::AudioPreview(QObject *parent) : QObject(parent)
{
	static_assert(TimelineAudio::kRate == AudioOutputStream::kInputRate &&
		TimelineAudio::kChannels == AudioOutputStream::kInputChannels);
	format_ = AudioOutputStream::mixFormat();
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
	lastError_.clear();
	if (pcm_.empty() || muted_)
		return;
	const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
	if (dev.isNull()) {
		reportFailure(QStringLiteral("No audio output device is available. Connect speakers or headphones and try again."));
		return;
	}
	format_ = AudioOutputStream::chooseFormat(dev.preferredFormat(),
		[&dev](const QAudioFormat &candidate) { return dev.isFormatSupported(candidate); });
	if (!format_.isValid()) {
		reportFailure(QStringLiteral("%1 does not provide a supported audio output format. Choose another output device.")
			.arg(dev.description()));
		return;
	}

	// Starting past the end would only produce silence.
	const qint64 startFrame = std::clamp<qint64>(TimelineAudio::frameForMs(fromMs), 0,
						     qint64(pcm_.size()) / TimelineAudio::kChannels);
	if (startFrame * TimelineAudio::kChannels >= qint64(pcm_.size()))
		return;

	startMs_ = startFrame * 1000 / TimelineAudio::kRate;
	reader_ = std::make_unique<Reader>(pcm_, startFrame, format_);
	if (!reader_->isValid() || !reader_->open(QIODevice::ReadOnly)) {
		const QString why = reader_->conversionError();
		stop();
		reportFailure(why.isEmpty() ? QStringLiteral("Could not open the preview audio stream.") : why);
		return;
	}
	sink_ = std::make_unique<QAudioSink>(dev, format_);
	const QString outputDescription = QStringLiteral("%1 (%2 Hz, %3 channels)")
		.arg(dev.description()).arg(format_.sampleRate()).arg(format_.channelCount());
	const quint64 generation = playbackGeneration_;
	connect(sink_.get(), &QAudioSink::stateChanged, this, [this, generation, outputDescription](QAudio::State state) {
		if (generation != playbackGeneration_ || !sink_ || state != QAudio::StoppedState || sink_->error() == QAudio::NoError)
			return;
		QString why = reader_ ? reader_->conversionError() : QString();
		if (why.isEmpty())
			why = sinkError(sink_->error());
		why = outputDescription + QStringLiteral(": ") + why;
		// A backend can emit this from start/stop itself. Release it after that
		// stack unwinds; an intentional stop or restart invalidates this result.
		QMetaObject::invokeMethod(this, [this, generation, why]() {
			if (generation != playbackGeneration_)
				return;
			stop();
			reportFailure(why);
		}, Qt::QueuedConnection);
	});
	sink_->setVolume(qreal(std::clamp(volume_, 0.0, 1.0)));
	sink_->start(reader_.get());
	if (sink_->error() != QAudio::NoError && sink_->state() == QAudio::StoppedState) {
		const QString why = outputDescription + QStringLiteral(": ") + sinkError(sink_->error());
		stop();
		reportFailure(why);
	}
}

void AudioPreview::stop()
{
	++playbackGeneration_;
	if (sink_) {
		disconnect(sink_.get(), nullptr, this, nullptr);
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

void AudioPreview::reportFailure(const QString &why)
{
	if (why == lastError_)
		return;
	lastError_ = why;
	emit failed(why);
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
