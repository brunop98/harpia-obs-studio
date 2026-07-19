#pragma once

#include <string>

namespace harpia {

// Losslessly re-containers a media file (stream copy, no re-encode) — used to
// turn a fast-finalizing .mkv recording into a .mp4 after the recording stops.
// Safe to call on a worker thread.
class Remuxer {
public:
	// Read `inPath` and write its streams, copied packet-for-packet, into
	// `outPath` (container chosen from the output extension). Returns true on
	// success. Does not delete the input.
	static bool remux(const std::string &inPath, const std::string &outPath);
};

} // namespace harpia
