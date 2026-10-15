#include "HdrToSdr.hpp"

#include "SwsColor.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

namespace harpia::hdr {

Kind kindOf(AVColorTransferCharacteristic trc, AVColorPrimaries primaries)
{
	Kind k;
	if (trc == AVCOL_TRC_ARIB_STD_B67)
		k.transfer = Transfer::Hlg;
	else if (trc == AVCOL_TRC_SMPTE2084)
		k.transfer = Transfer::Pq;
	k.bt2020 = primaries == AVCOL_PRI_BT2020;
	return k;
}

Kind kindOf(const AVFrame *f)
{
	return f ? kindOf(f->color_trc, f->color_primaries) : Kind{};
}

namespace {

// ITU-R BT.2100 HLG inverse OETF: signal -> scene light, [0, 1].
double hlgToScene(double v)
{
	const double a = 0.17883277, b = 1.0 - 4.0 * a, c = 0.5 - a * std::log(4.0 * a);
	return v <= 0.5 ? v * v / 3.0 : (std::exp((v - c) / a) + b) / 12.0;
}

// SMPTE ST 2084 (PQ) EOTF: signal -> light, 1.0 = 10000 nits.
double pqToLinear(double v)
{
	const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
	const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
	const double p = std::pow(std::max(v, 0.0), 1.0 / m2);
	return std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

} // namespace

ToneMapper::ToneMapper(const Kind &k) : kind_(k)
{
	toLinear_.resize(65536);
	for (int i = 0; i < 65536; ++i) {
		const double v = i / 65535.0;
		double l = 0;
		switch (k.transfer) {
		case Transfer::Hlg: l = hlgToScene(v); break;
		case Transfer::Pq: l = pqToLinear(v); break;
		case Transfer::Sdr: l = std::pow(v, 2.4); break; // BT.1886
		}
		toLinear_[size_t(i)] = float(l);
	}
	if (k.transfer == Transfer::Hlg) {
		// The OOTF's Ys^(gamma - 1), gamma 1.2 for a 1000-nit display.
		ootf_.resize(4097);
		for (int i = 0; i <= 4096; ++i)
			ootf_[size_t(i)] = float(std::pow(i / 4096.0, 0.2));
	}
	encode_.resize(65536);
	for (int i = 0; i < 65536; ++i)
		encode_[size_t(i)] = uint8_t(std::lround(255.0 * std::pow(i / 65535.0, 1.0 / 2.4)));

	const float w = kHlgPeakNits / kSdrWhiteNits;
	switch (k.transfer) {
	case Transfer::Hlg:
		scale_ = kHlgPeakNits / kSdrWhiteNits;
		white2_ = w * w;
		break;
	case Transfer::Pq:
		scale_ = 10000.f / kSdrWhiteNits;
		white2_ = w * w;
		break;
	case Transfer::Sdr:
		scale_ = 1.f;
		white2_ = 0.f;
		break;
	}
	if (k.bt2020) {
		// BT.2020 -> BT.709, linear light (libobs color.effect rec2020_to_rec709).
		const float m[9] = {1.6604910f,  -0.5876411f, -0.0728499f, -0.1245505f, 1.1328999f,
				    -0.0083494f, -0.0181508f, -0.1005789f, 1.1187297f};
		std::copy(m, m + 9, m_);
	}
}

void ToneMapper::pixel(const uint16_t in[3], uint8_t out[3]) const
{
	float r = toLinear_[in[0]], g = toLinear_[in[1]], b = toLinear_[in[2]];
	if (kind_.transfer == Transfer::Hlg) {
		const float ys = std::clamp(0.2627f * r + 0.678f * g + 0.0593f * b, 0.f, 1.f) * 4096.f;
		const int i = std::min(int(ys), 4095);
		const float f = ootf_[size_t(i)] + (ootf_[size_t(i) + 1] - ootf_[size_t(i)]) * (ys - float(i));
		r *= f;
		g *= f;
		b *= f;
	}
	r *= scale_;
	g *= scale_;
	b *= scale_;
	const float c[3] = {m_[0] * r + m_[1] * g + m_[2] * b, m_[3] * r + m_[4] * g + m_[5] * b,
			    m_[6] * r + m_[7] * g + m_[8] * b};
	for (int k = 0; k < 3; ++k) {
		float x = std::max(c[k], 0.f);
		if (white2_ > 0.f)
			x = x * (1.f + x / white2_) / (1.f + x); // Reinhard with a white point
		x = std::min(x, 1.f);
		out[k] = encode_[size_t(x * 65535.f + 0.5f)];
	}
}

void ToneMapper::rows(const uint16_t *in, int inStrideBytes, uint8_t *out, int outStrideBytes, int w, int y0,
		      int y1, int bpp) const
{
	for (int y = y0; y < y1; ++y) {
		const uint16_t *s = reinterpret_cast<const uint16_t *>(reinterpret_cast<const uint8_t *>(in) +
									size_t(y) * size_t(inStrideBytes));
		uint8_t *d = out + size_t(y) * size_t(outStrideBytes);
		for (int x = 0; x < w; ++x, s += 3, d += bpp) {
			pixel(s, d);
			if (bpp == 4)
				d[3] = 255;
		}
	}
}

void ToneMapper::map(const uint16_t *in, int inStrideBytes, uint8_t *out, int outStrideBytes, int w, int h,
		     int bpp) const
{
	const unsigned cores = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
	const int parts = (int64_t(w) * h >= 512 * 512) ? int(cores) : 1;
	if (parts <= 1) {
		rows(in, inStrideBytes, out, outStrideBytes, w, 0, h, bpp);
		return;
	}
	std::vector<std::thread> pool;
	for (int p = 0; p < parts; ++p) {
		const int y0 = h * p / parts, y1 = h * (p + 1) / parts;
		pool.emplace_back([=]() { rows(in, inStrideBytes, out, outStrideBytes, w, y0, y1, bpp); });
	}
	for (std::thread &t : pool)
		t.join();
}

Converter::~Converter()
{
	if (toRgb48_)
		sws_freeContext(toRgb48_);
	if (toYuv_)
		sws_freeContext(toYuv_);
}

bool Converter::toRgb(const uint8_t *const data[4], const int linesize[4], AVPixelFormat fmt, int sw, int sh,
		      AVColorSpace cs, AVColorRange range, const Kind &kind, int dw, int dh, AVPixelFormat dstFmt,
		      uint8_t *dst, int dstStride, int swsFlags)
{
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || !dst)
		return false;
	const int key[6] = {sw, sh, int(fmt), dw, dh, swsFlags};
	if (!toRgb48_ || !std::equal(key, key + 6, key_)) {
		if (toRgb48_)
			sws_freeContext(toRgb48_);
		toRgb48_ = sws_getContext(sw, sh, fmt, dw, dh, AV_PIX_FMT_RGB48, swsFlags, nullptr, nullptr, nullptr);
		std::copy(key, key + 6, key_);
	}
	if (!toRgb48_)
		return false;
	// The frame's own matrix and range, every time: cheap, and a stream can
	// change its tags without changing its size.
	swsDecodeColors(toRgb48_, fmt, cs, range, sh);
	if (!tm_ || tm_->kind().transfer != kind.transfer || tm_->kind().bt2020 != kind.bt2020)
		tm_ = std::make_unique<ToneMapper>(kind);
	rgb48_.resize(size_t(dw) * size_t(dh) * 3);
	uint8_t *mid[4] = {reinterpret_cast<uint8_t *>(rgb48_.data()), nullptr, nullptr, nullptr};
	int midStride[4] = {dw * 6, 0, 0, 0};
	sws_scale(toRgb48_, data, linesize, 0, sh, mid, midStride);
	const int bpp = dstFmt == AV_PIX_FMT_RGB24 ? 3 : 4;
	tm_->map(rgb48_.data(), dw * 6, dst, dstStride, dw, dh, bpp);
	return true;
}

bool Converter::toRgb(const AVFrame *f, int dw, int dh, AVPixelFormat dstFmt, uint8_t *dst, int dstStride,
		      int swsFlags)
{
	if (!f)
		return false;
	return toRgb(f->data, f->linesize, AVPixelFormat(f->format), f->width, f->height, f->colorspace,
		     f->color_range, kindOf(f), dw, dh, dstFmt, dst, dstStride, swsFlags);
}

bool Converter::toYuv(const AVFrame *f, uint8_t *const dstData[4], const int dstLinesize[4], AVPixelFormat dstFmt,
		      int dw, int dh, int swsFlags)
{
	if (!f || dw <= 0 || dh <= 0)
		return false;
	rgba_.resize(size_t(dw) * size_t(dh) * 4);
	if (!toRgb(f, dw, dh, AV_PIX_FMT_RGBA, rgba_.data(), dw * 4, swsFlags))
		return false;
	const int key[3] = {dw, dh, int(dstFmt)};
	if (!toYuv_ || !std::equal(key, key + 3, yuvKey_)) {
		if (toYuv_)
			sws_freeContext(toYuv_);
		toYuv_ = sws_getContext(dw, dh, AV_PIX_FMT_RGBA, dw, dh, dstFmt, SWS_BILINEAR, nullptr, nullptr,
					nullptr);
		if (toYuv_)
			swsEncodeColors709(toYuv_);
		std::copy(key, key + 3, yuvKey_);
	}
	if (!toYuv_)
		return false;
	const uint8_t *src[4] = {rgba_.data(), nullptr, nullptr, nullptr};
	const int srcStride[4] = {dw * 4, 0, 0, 0};
	sws_scale(toYuv_, src, srcStride, 0, dh, dstData, dstLinesize);
	return true;
}

bool Converter::toYuv(const AVFrame *f, AVFrame *dst, int swsFlags)
{
	if (!dst)
		return false;
	if (!toYuv(f, dst->data, dst->linesize, AVPixelFormat(dst->format), dst->width, dst->height, swsFlags))
		return false;
	dst->colorspace = AVCOL_SPC_BT709;
	dst->color_primaries = AVCOL_PRI_BT709;
	dst->color_trc = AVCOL_TRC_BT709;
	dst->color_range = AVCOL_RANGE_MPEG;
	return true;
}

} // namespace harpia::hdr
