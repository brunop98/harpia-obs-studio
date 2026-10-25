#pragma once

// SCK's requested 48 kHz stereo stream is linear PCM. Keep the sample packing
// independent of the native callback so alignment, clipping and planar input
// can be verified without an Apple SDK.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace harpia::mac_audio {

struct PcmPlane {
	const void *data = nullptr;
	size_t bytes = 0;
};

enum class PcmSample { Float32, Float64, Signed16, Signed32 };

inline size_t bytesPerSample(PcmSample sample)
{
	switch (sample) {
	case PcmSample::Float32: case PcmSample::Signed32: return 4;
	case PcmSample::Float64: return 8;
	case PcmSample::Signed16: return 2;
	}
	return 0;
}

inline double pcmSample(const unsigned char *p, PcmSample sample)
{
	switch (sample) {
	case PcmSample::Float32: { float v; std::memcpy(&v, p, 4); return std::isfinite(v) ? v : 0.0; }
	case PcmSample::Float64: { double v; std::memcpy(&v, p, 8); return std::isfinite(v) ? v : 0.0; }
	case PcmSample::Signed16: { int16_t v; std::memcpy(&v, p, 2); return double(v) / 32768.0; }
	case PcmSample::Signed32: { int32_t v; std::memcpy(&v, p, 4); return double(v) / 2147483648.0; }
	}
	return 0.0;
}

inline int16_t signed16(double value)
{
	if (!std::isfinite(value))
		return 0;
	if (value <= -1.0)
		return std::numeric_limits<int16_t>::min();
	if (value >= 1.0)
		return std::numeric_limits<int16_t>::max();
	return int16_t(std::clamp<long>(std::lrint(value * 32768.0),
		std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()));
}

// All bounds are checked before touching output. Mono is duplicated; the
// native configuration requests stereo, so unexpected surround is rejected.
inline bool packStereo16(const PcmPlane *planes, size_t planeCount, size_t frames,
			 unsigned channels, bool planar, PcmSample sample,
			 int16_t *output, size_t outputSamples)
{
	const size_t bytes = bytesPerSample(sample);
	if (!planes || !output || !bytes || channels < 1 || channels > 2 ||
	    frames > std::numeric_limits<size_t>::max() / (bytes * channels) ||
	    frames > outputSamples / 2 || planeCount < (planar ? channels : 1))
		return false;
	for (unsigned channel = 0; channel < (planar ? channels : 1); ++channel)
		if (!planes[channel].data || planes[channel].bytes < frames * bytes * (planar ? 1 : channels))
			return false;
	for (size_t frame = 0; frame < frames; ++frame) {
		for (unsigned channel = 0; channel < 2; ++channel) {
			const unsigned input = channels == 1 ? 0 : channel;
			const auto *data = static_cast<const unsigned char *>(planes[planar ? input : 0].data);
			const size_t at = planar ? frame : frame * channels + input;
			output[frame * 2 + channel] = signed16(pcmSample(data + at * bytes, sample));
		}
	}
	return true;
}

} // namespace harpia::mac_audio
