#pragma once

// The arithmetic behind Audio Only recording, with nothing libobs or FFmpeg in
// it so audiotap_test can drive all of it.
//
// Two things live here. AudioTapState is the bookkeeping the capture callback
// does on every buffer -- is this take paused, does this chunk fit, how long is
// the recording now. buildWavHeaderFloat32 is the header of the file the tap
// streams into, which has to be written twice: once with a placeholder length
// when the file is created, and once with the real length when it is closed.
// Building it in one pure function is what makes those two writes identical by
// construction rather than by two people remembering the same offsets.

#include <array>
#include <cstdint>
#include <cstring>

namespace harpia {

// Bookkeeping for the raw-audio tap. Every field is written from the libobs
// audio thread and read from the UI thread, so the owner holds a lock around
// offer() and copies out what it needs -- this struct does no locking itself,
// deliberately, because a mutex in a header is a mutex a test has to reason
// about.
struct AudioTapState {
	// Set while the recording is paused. Paused samples are DISCARDED rather
	// than queued: an audio-only file has no frames to hold a gap open, so
	// resuming has to butt straight onto what came before or the sound drifts
	// further behind the wall clock with every pause.
	bool paused = false;

	// Frames actually accepted -- which is what the file will be, and so what
	// the elapsed-time readout has to be derived from.
	uint64_t frames = 0;

	// Frames thrown away because the writer thread fell too far behind. Not
	// the same as paused frames: this one is a fault and gets logged.
	uint64_t dropped = 0;

	// How far ahead of the writer the queue may run before chunks are
	// dropped. 0 means unlimited. A cap exists at all because a stalled disk
	// must not turn into unbounded memory growth during a two-hour take.
	uint64_t queueCapFrames = 0;

	// Decide what to do with a buffer of `n` frames, given the writer still
	// has `queued` frames to get through. Returns how many frames to enqueue:
	// `n` to take it, 0 to drop it.
	//
	// A chunk is taken or dropped WHOLE. Taking half of one would leave the
	// file frame-aligned but split a buffer at an arbitrary point, and the
	// only thing that buys is a slightly smaller gap in audio that already
	// has a gap in it.
	uint64_t offer(uint64_t n, uint64_t queued)
	{
		if (paused || n == 0)
			return 0;
		if (queueCapFrames > 0 && queued + n > queueCapFrames) {
			dropped += n;
			return 0;
		}
		frames += n;
		return n;
	}

	// Length of the file so far. Derived from the frames that were kept, so a
	// pause shortens it -- which is correct: the file really is shorter.
	int64_t durationMs(int rate) const
	{
		if (rate <= 0)
			return 0;
		return int64_t(frames * 1000ull / uint64_t(rate));
	}
};

// A 32-bit float RIFF/WAVE header: 18-byte fmt chunk with the IEEE-float tag,
// then the `fact` chunk that a non-PCM WAV is required to carry, then `data`.
//
// The short 16-byte PCM-shaped header with the format tag switched to 3 is what
// most code writes and most decoders accept, but "most" is doing real work in
// that sentence -- this file is what the user is left holding if the encode
// step fails, so it is written to spec.
inline constexpr size_t kWavHeaderBytes = 58;

// Byte offsets of the three fields that are not known until the file is closed,
// exposed so the patch-on-close path cannot drift from the layout below.
inline constexpr size_t kWavRiffSizeOffset = 4;
inline constexpr size_t kWavFactFramesOffset = 46;
inline constexpr size_t kWavDataSizeOffset = 54;

inline std::array<uint8_t, kWavHeaderBytes> buildWavHeaderFloat32(int rate, int channels,
								  uint64_t dataBytes)
{
	std::array<uint8_t, kWavHeaderBytes> h{};
	size_t at = 0;
	const auto tag = [&](const char *s) {
		std::memcpy(h.data() + at, s, 4);
		at += 4;
	};
	const auto u32 = [&](uint32_t v) {
		h[at++] = uint8_t(v & 0xff);
		h[at++] = uint8_t((v >> 8) & 0xff);
		h[at++] = uint8_t((v >> 16) & 0xff);
		h[at++] = uint8_t((v >> 24) & 0xff);
	};
	const auto u16 = [&](uint16_t v) {
		h[at++] = uint8_t(v & 0xff);
		h[at++] = uint8_t((v >> 8) & 0xff);
	};

	const uint32_t ch = uint32_t(channels > 0 ? channels : 1);
	const uint32_t sr = uint32_t(rate > 0 ? rate : 48000);
	const uint32_t blockAlign = ch * 4;

	// A RIFF length field is 32 bits, so past 4 GB the container simply cannot
	// describe itself. Saturate rather than wrap: a truncated-looking length is
	// recoverable by a decoder that reads to EOF, a wrapped one is not.
	const uint64_t maxData = 0xFFFFFFFFull - (kWavHeaderBytes - 8);
	const uint32_t data = uint32_t(dataBytes > maxData ? maxData : dataBytes);

	tag("RIFF");
	u32(uint32_t(kWavHeaderBytes - 8) + data);
	tag("WAVE");
	tag("fmt ");
	u32(18);
	u16(3); // WAVE_FORMAT_IEEE_FLOAT
	u16(uint16_t(ch));
	u32(sr);
	u32(sr * blockAlign); // byte rate
	u16(uint16_t(blockAlign));
	u16(32); // bits per sample
	u16(0);  // cbSize: no extension
	tag("fact");
	u32(4);
	u32(uint32_t(data / (blockAlign ? blockAlign : 1))); // frames
	tag("data");
	u32(data);
	return h;
}

} // namespace harpia
