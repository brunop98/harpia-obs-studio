// How big the exported file will be, roughly.
//
// This is an ESTIMATE and the UI says so. Both encoders here are
// constant-quality, not constant-bitrate: x264 at a given CRF spends whatever
// bits the picture needs, so a static screen recording and a handheld shot of
// foliage at identical settings differ by more than an order of magnitude. No
// arithmetic can close that gap without encoding the thing.
//
// What the estimate IS good for is the question people actually ask before
// exporting: "is this going to be 5 MB or 500 MB?", and "does halving the
// resolution help?". For those it only has to be right to within a factor of
// about two and, more importantly, it has to MOVE the right way when a setting
// changes. Both properties are pinned by exportestimate_test.
//
// The coefficients are fitted to real encodes from this build's encoders rather
// than taken from a table, and the test re-encodes and compares so they cannot
// silently drift away from the encoder they describe.
#pragma once

#include "ClipExporter.hpp"

#include <QString>

namespace harpia {

struct ExportEstimateInput {
	ClipExporter::Format format = ClipExporter::Format::Mp4;
	int width = 1920;
	int height = 1080;
	double fps = 30.0;
	double seconds = 0.0;
	int videoCrf = 23;   // video formats
	bool keepAudio = true;
	int gifColors = 256; // GIF
	bool gifDither = true;
};

// Bytes. 0 when the input cannot produce a file (no duration, no pixels).
qint64 estimateExportBytes(const ExportEstimateInput &in);

// "24.8 MB" / "812 KB". Deliberately decimal MB, matching what file managers
// and upload limits mean by the word.
QString humanFileSize(qint64 bytes);

} // namespace harpia
