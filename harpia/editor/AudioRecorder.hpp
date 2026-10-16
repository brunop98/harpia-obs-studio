#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>

class QAudioSource;
class QIODevice;
class QFile;
class QTimer;

namespace harpia {

class SystemAudioCapture;

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
//
// The device id may also name the computer's own sound instead of a
// microphone (system_audio::sourceId(): everything Windows plays, or one app).
// Those takes come from SystemAudioCapture, always stereo at the rate Windows
// plays, and are otherwise the same: the same WAV, the same level signal.
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

	bool isRecording() const { return source_ != nullptr || sys_ != nullptr; }

	// The last take started was the computer's sound, not a microphone.
	bool isSystemSource() const { return systemTake_; }
	// ...and Harpia's own sound was kept out of it (Windows 11), so the
	// timeline may play out loud while it records.
	bool systemExcludesSelf() const { return systemExcludesSelf_; }

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
	void onSystemAudio();

private:
	bool openTakeFile(const QString &dir); // the temp WAV, header placeholder written
	void writeWavHeaderPlaceholder();
	void patchWavHeader();
	void emitLevel(const char *data, qint64 bytes);

	QString deviceId_;
	int sampleRate_ = 48000;
	int channels_ = 1;

	QAudioSource *source_ = nullptr; // owns the capture; null when idle
	SystemAudioCapture *sys_ = nullptr; // or this, for the computer's sound
	QTimer *sysTimer_ = nullptr;        // collects what its thread captured
	bool systemTake_ = false;
	bool systemExcludesSelf_ = false;
	bool sysFailureShown_ = false;
	QIODevice *pull_ = nullptr;      // QAudioSource::start() stream (not owned)
	QFile *file_ = nullptr;          // the WAV being written
	QString path_;
	qint64 dataBytes_ = 0; // PCM payload size, for the header + duration
};

} // namespace harpia
