#pragma once

#include <QAudioFormat>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

namespace harpia {

// A bounded streaming view of the timeline's interleaved stereo float48k mix.
// The owner keeps pcm alive until the stream is destroyed. Reads are sequential
// on one audio thread; seek by constructing a new stream at an input frame.
// Past the mix it supplies correctly encoded silence, keeping the device clock
// running when the picture lasts longer than its audio.
class AudioOutputStream {
public:
	static constexpr int kInputRate = 48000;
	static constexpr int kInputChannels = 2;

	AudioOutputStream(const std::vector<float> &pcm, qint64 startFrame, const QAudioFormat &format);
	~AudioOutputStream();
	AudioOutputStream(const AudioOutputStream &) = delete;
	AudioOutputStream &operator=(const AudioOutputStream &) = delete;

	static QAudioFormat mixFormat();
	static bool canConvertTo(const QAudioFormat &format);
	// Device-independent negotiation, so the policy can also be checked for
	// devices that are not attached to the test machine.
	static QAudioFormat chooseFormat(const QAudioFormat &preferred,
		const std::function<bool(const QAudioFormat &)> &supported);

	bool isValid() const;
	QString error() const;
	qint64 read(char *data, qint64 maxBytes);
	bool atEndOfMix() const;
	// Conversion scratch only; never grows with the timeline's duration.
	qsizetype bufferedBytes() const;

private:
	struct Impl;
	std::unique_ptr<Impl> d_;
};

} // namespace harpia
