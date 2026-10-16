#pragma once

// Cutting the quiet ends off a recorded take.
//
// Grabbing a sound from Windows means pressing Record, switching to the
// browser and pressing play -- and, at the other end, waiting for the video to
// stop before switching back. Both waits are recorded. This takes them off, so
// the sound starts where the take is put.
//
// Works on the 16-bit PCM WAVs AudioRecorder writes, in place. A short margin
// is kept either side so the first consonant and the last echo survive.

#include <QString>
#include <QtGlobal>

namespace harpia {

struct WavTrim {
	bool ok = false;     // the file was read (and rewritten, if anything was cut)
	bool silent = false; // nothing above the threshold anywhere: left as it was
	qint64 leadMs = 0;   // cut from the start
	qint64 tailMs = 0;   // cut from the end
	qint64 durationMs = 0; // what is left
};

WavTrim trimWavSilence(const QString &path, double thresholdDb = -60.0, int keepLeadMs = 40,
		       int keepTailMs = 250);

} // namespace harpia
