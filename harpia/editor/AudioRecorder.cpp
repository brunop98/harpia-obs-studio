#include "AudioRecorder.hpp"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QMediaDevices>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace harpia {

namespace {
// Little-endian writers for the WAV header.
void putU32(char *p, quint32 v)
{
	p[0] = char(v & 0xff);
	p[1] = char((v >> 8) & 0xff);
	p[2] = char((v >> 16) & 0xff);
	p[3] = char((v >> 24) & 0xff);
}
void putU16(char *p, quint16 v)
{
	p[0] = char(v & 0xff);
	p[1] = char((v >> 8) & 0xff);
}
} // namespace

AudioRecorder::AudioRecorder(QObject *parent) : QObject(parent) {}

AudioRecorder::~AudioRecorder()
{
	if (isRecording())
		stop();
}

QVector<AudioInputDevice> AudioRecorder::inputDevices()
{
	QVector<AudioInputDevice> out;
	const QAudioDevice def = QMediaDevices::defaultAudioInput();
	for (const QAudioDevice &d : QMediaDevices::audioInputs()) {
		AudioInputDevice e;
		e.id = QString::fromUtf8(d.id());
		e.name = d.description();
		e.isDefault = (d.id() == def.id());
		if (e.isDefault)
			out.prepend(e); // default first
		else
			out.append(e);
	}
	return out;
}

void AudioRecorder::setDeviceId(const QString &id)
{
	if (!isRecording())
		deviceId_ = id;
}

void AudioRecorder::setSampleRate(int hz)
{
	if (!isRecording() && hz > 0)
		sampleRate_ = hz;
}

void AudioRecorder::setChannelCount(int ch)
{
	if (!isRecording() && (ch == 1 || ch == 2))
		channels_ = ch;
}

bool AudioRecorder::start(const QString &dir)
{
	if (isRecording())
		return false;

	// Resolve the requested device (fall back to the system default).
	QAudioDevice device = QMediaDevices::defaultAudioInput();
	if (!deviceId_.isEmpty()) {
		const QByteArray want = deviceId_.toUtf8();
		for (const QAudioDevice &d : QMediaDevices::audioInputs()) {
			if (d.id() == want) {
				device = d;
				break;
			}
		}
	}
	if (device.isNull()) {
		emit error(QStringLiteral("No microphone is available."));
		return false;
	}

	QAudioFormat fmt;
	fmt.setSampleRate(sampleRate_);
	fmt.setChannelCount(channels_);
	fmt.setSampleFormat(QAudioFormat::Int16);
	if (!device.isFormatSupported(fmt)) {
		// Fall back to the device's preferred format, keeping Int16 if possible.
		fmt = device.preferredFormat();
		fmt.setSampleFormat(QAudioFormat::Int16);
		sampleRate_ = fmt.sampleRate();
		channels_ = fmt.channelCount();
	}

	const QString folder = dir.isEmpty() ? QDir::tempPath() : dir;
	QDir().mkpath(folder);
	path_ = QDir(folder).filePath(
		QStringLiteral("harpia_voiceover_%1.wav")
			.arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss_zzz"))));

	file_ = new QFile(path_, this);
	if (!file_->open(QIODevice::WriteOnly)) {
		emit error(QStringLiteral("Could not create the voiceover file."));
		delete file_;
		file_ = nullptr;
		return false;
	}
	dataBytes_ = 0;
	writeWavHeaderPlaceholder();

	source_ = new QAudioSource(device, fmt, this);
	pull_ = source_->start(); // pull mode: read from this device on readyRead
	if (!pull_) {
		emit error(QStringLiteral("Could not start microphone capture."));
		delete source_;
		source_ = nullptr;
		file_->close();
		delete file_;
		file_ = nullptr;
		QFile::remove(path_);
		return false;
	}
	connect(pull_, &QIODevice::readyRead, this, &AudioRecorder::onReadyRead);
	return true;
}

QString AudioRecorder::stop()
{
	if (!isRecording())
		return QString();

	// Drain anything still buffered before tearing down.
	onReadyRead();
	source_->stop();
	disconnect(pull_, nullptr, this, nullptr);
	delete source_;
	source_ = nullptr;
	pull_ = nullptr;

	patchWavHeader();
	file_->close();
	delete file_;
	file_ = nullptr;

	const QString written = dataBytes_ > 0 ? path_ : QString();
	if (dataBytes_ == 0)
		QFile::remove(path_); // empty take — nothing captured
	return written;
}

qint64 AudioRecorder::capturedMs() const
{
	const int bytesPerSample = 2 * std::max(1, channels_);
	if (sampleRate_ <= 0 || bytesPerSample <= 0)
		return 0;
	return qint64(dataBytes_ / bytesPerSample) * 1000 / sampleRate_;
}

void AudioRecorder::onReadyRead()
{
	if (!pull_ || !file_)
		return;
	const QByteArray chunk = pull_->readAll();
	if (chunk.isEmpty())
		return;
	file_->write(chunk);
	dataBytes_ += chunk.size();
	emitLevel(chunk.constData(), chunk.size());
}

void AudioRecorder::emitLevel(const char *data, qint64 bytes)
{
	// Int16 samples → RMS + peak, normalized to 0..1.
	const qint64 n = bytes / 2;
	if (n <= 0)
		return;
	const auto *s = reinterpret_cast<const qint16 *>(data);
	double sumSq = 0.0;
	int peak = 0;
	for (qint64 i = 0; i < n; ++i) {
		const int v = s[i];
		sumSq += double(v) * double(v);
		peak = std::max(peak, std::abs(v));
	}
	const double rms = std::sqrt(sumSq / double(n)) / 32768.0;
	emit level(std::clamp(rms, 0.0, 1.0), std::clamp(peak / 32768.0, 0.0, 1.0));
}

void AudioRecorder::writeWavHeaderPlaceholder()
{
	// Canonical 44-byte PCM WAV header; sizes patched in patchWavHeader().
	char h[44];
	std::memset(h, 0, sizeof(h));
	std::memcpy(h + 0, "RIFF", 4);
	putU32(h + 4, 36); // ChunkSize = 36 + dataBytes (patched)
	std::memcpy(h + 8, "WAVE", 4);
	std::memcpy(h + 12, "fmt ", 4);
	putU32(h + 16, 16);                                  // Subchunk1Size (PCM)
	putU16(h + 20, 1);                                   // AudioFormat = PCM
	putU16(h + 22, quint16(channels_));                  // NumChannels
	putU32(h + 24, quint32(sampleRate_));                // SampleRate
	const quint32 byteRate = quint32(sampleRate_) * channels_ * 2;
	putU32(h + 28, byteRate);                            // ByteRate
	putU16(h + 32, quint16(channels_ * 2));              // BlockAlign
	putU16(h + 34, 16);                                  // BitsPerSample
	std::memcpy(h + 36, "data", 4);
	putU32(h + 40, 0); // Subchunk2Size = dataBytes (patched)
	file_->write(h, sizeof(h));
}

void AudioRecorder::patchWavHeader()
{
	if (!file_)
		return;
	char buf[4];
	file_->seek(4);
	putU32(buf, quint32(36 + dataBytes_));
	file_->write(buf, 4);
	file_->seek(40);
	putU32(buf, quint32(dataBytes_));
	file_->write(buf, 4);
	file_->flush();
}

} // namespace harpia
