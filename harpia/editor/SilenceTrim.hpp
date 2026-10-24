#pragma once

// Pause detection works in output time. Applying its reviewed ranges edits the
// timeline model only; the media files are never rewritten.

#include "timeline/TimelineSlice.hpp"

#include <QString>

#include <atomic>
#include <vector>

namespace harpia {
namespace silence_trim {

struct Options {
	double thresholdDb = -40.0;
	qint64 minSilenceMs = 500;
	qint64 keepPauseMs = 150; // total quiet time kept around an internal pause
	bool includeEdges = false;
};

// Interleaved float PCM. Ten-millisecond windows use the loudest channel's
// RMS, with a peak guard for brief transients. Entirely quiet/invalid input
// yields no cuts rather than removing a whole recording.
QVector<TlSpan> detect(const std::vector<float> &pcm, int sampleRate, int channels,
		       const Options &options = {}, std::atomic<bool> *cancel = nullptr);

// Analyze only [fromMs,toMs) without copying the PCM. Returned ranges retain
// their positions in the complete buffer, and edge handling applies to this
// analysis window rather than unrelated gaps outside it.
QVector<TlSpan> detectRange(const std::vector<float> &pcm, int sampleRate, int channels,
			    qint64 fromMs, qint64 toMs, const Options &options = {},
			    std::atomic<bool> *cancel = nullptr);

// Clamp to the timeline and union overlapping/touching ranges.
QVector<TlSpan> normalize(const QVector<TlSpan> &ranges, qint64 durationMs);

// Position after removing normalized ranges; a point inside a removal maps to
// the join. Useful for moving the playhead along with the edited picture.
qint64 mapTime(qint64 oldMs, const QVector<TlSpan> &normalizedRanges);

struct EditResult {
	TimelineModel model;
	QVector<TlSpan> removed;
	QString error;
	qint64 removedMs = 0;
	bool ok() const { return error.isEmpty(); }
};

// Remove the same spans from every lane so footage, sound and captions remain
// synchronized. An error returns the untouched model. Locked affected lanes,
// transition boundaries and unsupported time-driven animations are diagnosed
// instead of being moved or silently restarted.
EditResult apply(const TimelineModel &model, const QVector<TlSpan> &ranges);

} // namespace silence_trim
} // namespace harpia
