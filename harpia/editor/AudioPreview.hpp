#pragma once

// Plays a mixed timeline through the speakers while the preview runs.
//
// The buffer is the whole timeline pre-mixed to interleaved stereo float at
// 48 kHz (see TimelineAudio), so seeking is just an index — no decoding happens
// on the audio path and the playhead can jump anywhere instantly.
//
// The device is also the CLOCK. positionMs() reports what has actually reached
// the speakers, which is what keeps the picture matched to the sound: a
// wall-clock timer and an audio device drift apart, and the drift is audible
// long before it is visible.

#include <QAudioFormat>
#include <QObject>

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

	// False when the machine has no usable audio output; every call below then
	// does nothing and the editor plays silently, as it did before.
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

	void setMuted(bool on);
	bool isMuted() const { return muted_; }
	void setVolume(double v); // 0..1

signals:
	void finished(); // the buffer ran out

private:
	class Reader; // QIODevice over the buffer

	std::vector<float> pcm_;
	std::unique_ptr<QAudioSink> sink_;
	std::unique_ptr<Reader> reader_;
	QAudioFormat format_;
	qint64 startMs_ = 0;
	bool muted_ = false;
	double volume_ = 1.0;
};

} // namespace harpia
