#pragma once

// One libav audio encode, fed by a pull callback instead of a buffer.
//
// It lives in core rather than in the editor because two very different callers
// need the same thirty lines of encoder setup: "Extract Audio Only", which has
// the whole clip in memory, and Audio Only recording, which streams a file that
// may be hours long and must never hold more than a chunk of it at a time. A
// pull callback serves both -- the in-memory caller wraps its vector in three
// lines -- and means there is exactly one place where the sample format, the
// resampler and the trailer are got right.

#include <functional>
#include <string>

namespace harpia {

// Is this build carrying that encoder? Asked rather than assumed: "aac" is
// always present, "libmp3lame" often is not.
bool audioEncoderAvailable(const char *encoderName);

// Encode to `path`, taking interleaved float frames from `pull`.
//
// pull(dst, maxFrames) fills up to maxFrames frames (maxFrames * channels
// floats) and returns how many frames it wrote; returning 0 ends the file. It
// is called on the calling thread.
//
// The container is chosen from the path's extension, as libav does.
bool encodeAudioStream(const std::string &path, const char *encoderName,
		       const std::function<int(float *dst, int maxFrames)> &pull, int rate,
		       int channels, int bitrateKbps, std::string *err);

} // namespace harpia
