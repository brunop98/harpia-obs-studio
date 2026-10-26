#pragma once

#include <QString>
#include <QVector>

#include <atomic>
#include <vector>

namespace harpia::beat_detection {

struct Options {
	// Lower values keep stronger attacks; higher values admit quieter ones.
	double sensitivity = 1.0;
};

struct Result {
	// Milliseconds from the start of the analyzed buffer or requested source
	// interval. No guessed markers are inserted through silence.
	QVector<qint64> beatsMs;
	double bpm = 0.0;
	double confidence = 0.0;
	QString error;
	bool canceled = false;
};

// Detect a stable musical pulse in interleaved float PCM. This is an estimate
// based on rhythmic attacks, rather than a transcription of musical meter.
Result detect(const std::vector<float> &pcm, int sampleRate, int channels,
	      const Options &options = {}, std::atomic<bool> *cancel = nullptr);

// Analyze only [sourceStartMs, sourceEndMs). Decoding is local, seeks to the
// selected range, and can be canceled. Times retain initial audio delays and
// gaps relative to the requested start; trim and playback-speed mapping are
// the caller's responsibility.
Result analyzeFile(const QString &path, qint64 sourceStartMs, qint64 sourceEndMs,
		   const Options &options = {}, std::atomic<bool> *cancel = nullptr);

} // namespace harpia::beat_detection
