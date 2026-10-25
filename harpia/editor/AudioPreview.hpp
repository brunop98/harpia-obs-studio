#pragma once

// Plays a mixed timeline through the speakers while the preview runs.
//
// The buffer is the whole timeline pre-mixed to interleaved stereo float at
// 48 kHz (see TimelineAudio), so seeking is just an index. Devices that need
// another PCM format are converted in small streaming chunks. No decoding or
// second copy of the whole mix is needed on the audio path.
//
// The device is also the CLOCK. positionMs() reports what has actually reached
// the speakers, which is what keeps the picture matched to the sound: a
// wall-clock timer and an audio device drift apart, and the drift is audible
// long before it is visible.

#include <QAudioFormat>
#include <QObject>
#include <QString>

#include <memory>
#include <vector>

class QAudioSink;
class QIODevice;

namespace harpia {

class AudioPreview : public QObject {
	Q_OBJECT
public:
	explicit AudioPreview(QObject *parent = nullptr);
	~AudioPreview() override;

	// False when the machine has no output device. A failed start also emits
	// failed() with a readable reason; callers can surface it without a modal.
	static bool available();

	// Hand over the mix. Any playback in progress stops.
	void setBuffer(std::vector<float> pcm);
	void clear();
	bool hasAudio() const;
	qint64 durationMs() const;

	void start(qint64 fromMs);
	void stop();
	bool isPlaying() const;

	// Where the sound actually is, in output-time. -1 when not playing.
	qint64 positionMs() const;
	QString lastError() const { return lastError_; }

	void setMuted(bool on);
	bool isMuted() const { return muted_; }
	void setVolume(double v); // 0..1

signals:
	void finished(); // the buffer ran out
	void failed(const QString &reason);

private:
	class Reader; // QIODevice over the buffer
	void reportFailure(const QString &why);

	std::vector<float> pcm_;
	std::unique_ptr<QAudioSink> sink_;
	std::unique_ptr<Reader> reader_;
	QAudioFormat format_;
	QString lastError_;
	quint64 playbackGeneration_ = 0;
	qint64 startMs_ = 0;
	bool muted_ = false;
	double volume_ = 1.0;
};

} // namespace harpia
