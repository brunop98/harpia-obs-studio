#pragma once

// HDR video -- an iPhone's HLG, an HDR10 (PQ) file -- shown and exported as
// SDR, the way OBS puts an HDR source on an SDR canvas.
//
// Without this an HDR frame was converted like any other: the right YUV
// matrix (BT.2020), but its brightness curve (HLG or PQ) read as if it were
// the SDR one and its wide BT.2020 colours as BT.709. The picture came out
// flat and pale, with every highlight clipped to white -- and an export kept
// the HDR labels on 8-bit SDR pixels.
//
// The conversion is libobs's own (libobs/data/color.effect, obs-source.c):
//   HLG  -> light with a 1000-nit peak (OOTF exponent 0.2), PQ -> nits;
//   scaled so SDR white is 300 nits (OBS's default SDR white level);
//   BT.2020 -> BT.709 primaries in linear light;
//   Reinhard per channel -- here with a white point at the 1000-nit peak, so
//   the brightest highlight still reaches white (plain Reinhard stops at 90%);
//   encoded with the BT.1886 1/2.4 power, as SDR video is.
// SDR video with BT.2020 primaries gets the primaries step only.
//
// The frame is converted by swscale to 16-bit RGB with its own matrix and
// range first, so 10-bit sources keep their precision until the end.

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <memory>
#include <vector>

namespace harpia::hdr {

enum class Transfer { Sdr, Hlg, Pq };

struct Kind {
	Transfer transfer = Transfer::Sdr;
	bool bt2020 = false; // BT.2020 primaries
	bool needsConversion() const { return transfer != Transfer::Sdr || bt2020; }
};

Kind kindOf(AVColorTransferCharacteristic trc, AVColorPrimaries primaries);
Kind kindOf(const AVFrame *f);

constexpr float kSdrWhiteNits = 300.f; // OBS's default SDR white level
constexpr float kHlgPeakNits = 1000.f; // OBS's HDR nominal peak; BT.2408's comfortable maximum

// The lookup tables for one Kind, and the per-pixel work.
class ToneMapper {
public:
	explicit ToneMapper(const Kind &k);
	const Kind &kind() const { return kind_; }
	// One pixel: 16-bit RGB signal (0..65535) -> 8-bit SDR BT.709 RGB.
	void pixel(const uint16_t in[3], uint8_t out[3]) const;
	// Rows of packed RGB48 (native endian) -> rows of 3 (RGB) or 4 (RGBA, alpha
	// 255) bytes a pixel. Large images are split across threads.
	void map(const uint16_t *in, int inStrideBytes, uint8_t *out, int outStrideBytes, int w, int h,
		 int outBytesPerPixel) const;

private:
	void rows(const uint16_t *in, int inStrideBytes, uint8_t *out, int outStrideBytes, int w, int y0, int y1,
		  int bpp) const;
	Kind kind_;
	std::vector<float> toLinear_;  // 65536: signal -> linear (HLG: scene light; PQ: x 10000 nits)
	std::vector<float> ootf_;      // HLG: Ys -> Ys^0.2, 4097 entries over [0, 1]
	std::vector<uint8_t> encode_;  // 65536: SDR linear [0, 1] -> 8-bit
	float scale_ = 1.f;            // linear -> relative to SDR white
	float white2_ = 0.f;           // Reinhard white point squared (0: no tone curve)
	float m_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

// Frames to 8-bit SDR pixels, keeping its swscale contexts and buffers.
class Converter {
public:
	Converter() = default;
	Converter(const Converter &) = delete;
	Converter &operator=(const Converter &) = delete;
	~Converter();

	// A region of a frame (data/linesize may point into a crop): to packed
	// RGB24 or RGBA/RGB0 rows at dw x dh.
	bool toRgb(const uint8_t *const data[4], const int linesize[4], AVPixelFormat fmt, int sw, int sh,
		   AVColorSpace cs, AVColorRange range, const Kind &kind, int dw, int dh, AVPixelFormat dstFmt,
		   uint8_t *dst, int dstStride, int swsFlags = SWS_BILINEAR);
	bool toRgb(const AVFrame *f, int dw, int dh, AVPixelFormat dstFmt, uint8_t *dst, int dstStride,
		   int swsFlags = SWS_BILINEAR);
	// Into `dst`'s planes (any YUV format, its own size, or the `dstData`
	// planes at dw x dh when given): BT.709, limited range. Tags `dst` so.
	bool toYuv(const AVFrame *f, AVFrame *dst, int swsFlags = SWS_BILINEAR);
	bool toYuv(const AVFrame *f, uint8_t *const dstData[4], const int dstLinesize[4], AVPixelFormat dstFmt,
		   int dw, int dh, int swsFlags = SWS_BILINEAR);

private:
	SwsContext *toRgb48_ = nullptr;
	SwsContext *toYuv_ = nullptr;
	int key_[6] = {-1, -1, -1, -1, -1, -1};
	int yuvKey_[3] = {-1, -1, -1};
	std::unique_ptr<ToneMapper> tm_;
	std::vector<uint16_t> rgb48_;
	std::vector<uint8_t> rgba_;
};

} // namespace harpia::hdr
