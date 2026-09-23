#pragma once

// The audio a media clip plays, as the transcriber wants it: 16 kHz, mono,
// 16-bit WAV, cut to the clip's source range, in pieces no longer than the
// API's upload allows.
//
// Why re-encode at all: the editor's own decode (VoiceoverMixer::decodeToWav)
// gives 48 kHz stereo, which is 192 KB a second -- a ten-minute clip is over
// the 25 MB limit and most of those bytes carry nothing speech needs. At
// 16 kHz mono it is 32 KB a second, so one piece holds about thirteen
// minutes; pieces are cut at ten to leave room.

#include <QString>
#include <QVector>

namespace harpia {

struct SpeechAudioChunk {
	QString wavPath;   // a temporary file the caller deletes
	qint64 offsetMs;   // this chunk's start in SOURCE time (word times add this)
	qint64 lengthMs;
};

class AudioForSpeech {
public:
	static constexpr int kRate = 16000;
	static constexpr qint64 kChunkMs = 10 * 60 * 1000;

	// Decode `sourcePath`, keep [srcStartMs, srcEndMs), write chunks into
	// `workDir`. Empty with *err set on failure. `speed` is NOT applied: the
	// words are wanted in source time, and buildSubtitleClips maps them.
	static QVector<SpeechAudioChunk> prepare(const QString &sourcePath, qint64 srcStartMs,
						  qint64 srcEndMs, const QString &workDir, QString *err);

	// Exposed for tests: turn 48 kHz interleaved stereo 16-bit PCM into 16 kHz
	// mono. Each output sample averages the six input samples (3 frames x 2
	// channels) it stands for, which is a box filter -- crude, but the
	// transcriber does not care about the top octave of what it removes.
	static QVector<qint16> downmixTo16k(const qint16 *stereo48k, qint64 frames);

	// Write a 16 kHz mono 16-bit WAV. For tests and for prepare().
	static bool writeWav16k(const QString &path, const QVector<qint16> &mono);
};

} // namespace harpia
