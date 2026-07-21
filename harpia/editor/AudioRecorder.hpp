#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>

class QAudioSource;
class QIODevice;
class QFile;

namespace harpia {

// One selectable microphone/input device (id is opaque, name is human-facing).
struct AudioInputDevice {
	QString id;
	QString name;
	bool isDefault = false;
};

// Editor voiceover capture engine: records a single take from a chosen
// microphone (Qt Multimedia / QAudioSource) into a temp 16-bit PCM WAV file,
// while streaming live input levels for a meter. Mono, 48 kHz by default (the
// natural choice for narration; changeable before start()).
//
// The editor is otherwise a libav module with no audio capture — this is the
// one place that touches the OS mic. Playback/mixing of the recorded takes is
// handled elsewhere (the voiceover track + ClipExporter).
class AudioRecorder : public QObject {
	Q_OBJECT
public:
	explicit AudioRecorder(QObject *parent = nullptr);
	~AudioRecorder() override;

	// Available input devices, default first. Safe to call anytime.
	static QVector<AudioInputDevice> inputDevices();

	// Choose the capture device by id (empty = system default). Applied on the
	// next start(); ignored while recording.
	void setDeviceId(const QString &id);
	QString deviceId() const { return deviceId_; }

	// Capture format (must be set before start()).
	void setSampleRate(int hz);       // default 48000
	void setChannelCount(int ch);     // default 1 (mono)
	int sampleRate() const { return sampleRate_; }
	int channelCount() const { return channels_; }

	bool isRecording() const { return source_ != nullptr; }

	// Begin capturing to a fresh temp WAV (created under `dir`, or the system
	// temp dir if empty). Returns false and emits error() on failure.
	bool start(const QString &dir = QString());

	// Stop capturing and finalize the WAV header. Returns the written file path
	// (empty if nothing was captured / on failure). Ownership of the file is the
	// caller's — delete it when the take is discarded.
	QString stop();

	// Duration captured so far, in milliseconds.
	qint64 capturedMs() const;

signals:
	// Live input level for a meter, both 0..1 (linear). Emitted as audio arrives.
	void level(qreal rms, qreal peak);
	void error(const QString &message);

private slots:
	void onReadyRead();

private:
	void writeWavHeaderPlaceholder();
	void patchWavHeader();
	void emitLevel(const char *data, qint64 bytes);

	QString deviceId_;
	int sampleRate_ = 48000;
	int channels_ = 1;

	QAudioSource *source_ = nullptr; // owns the capture; null when idle
	QIODevice *pull_ = nullptr;      // QAudioSource::start() stream (not owned)
	QFile *file_ = nullptr;          // the WAV being written
	QString path_;
	qint64 dataBytes_ = 0; // PCM payload size, for the header + duration
};

} // namespace harpia
