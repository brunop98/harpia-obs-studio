#pragma once

// Which colour matrix a conversion between YUV and RGB uses.
//
// swscale converts with BT.601 coefficients unless told otherwise. Nearly
// every video Harpia sees is HD and BT.709 -- OBS records in 709, phones and
// cameras tag 709 -- so decoding with 601 shifted every colour a little (reds
// towards orange, greens towards yellow, skin tones redder), and encoding the
// composited timeline with 601 while the file SAYS 709 shifted them again on
// the way out. Both directions go through here now:
//
//   decode : the frame's own matrix and range (its tags; untagged HD is 709,
//            untagged SD is 601 -- the convention players use too);
//   encode : BT.709, limited range, which is what the encoder writes in the file.

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>

namespace harpia {

// SWS_CS_* for a frame, from its tag or, untagged, its size.
inline int swsSpaceOf(AVColorSpace cs, int height)
{
	switch (cs) {
	case AVCOL_SPC_BT709: return SWS_CS_ITU709;
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
	case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
	case AVCOL_SPC_BT2020_NCL:
	case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
	case AVCOL_SPC_FCC: return SWS_CS_FCC;
	default: break;
	}
	return height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
}

// Full ("JPEG", 0-255) range: the tag, or one of the yuvj formats that imply it.
inline bool isFullRange(AVColorRange range, AVPixelFormat fmt)
{
	if (range == AVCOL_RANGE_JPEG)
		return true;
	return fmt == AV_PIX_FMT_YUVJ420P || fmt == AV_PIX_FMT_YUVJ422P || fmt == AV_PIX_FMT_YUVJ444P ||
	       fmt == AV_PIX_FMT_YUVJ440P || fmt == AV_PIX_FMT_YUVJ411P;
}

inline bool isRgbFormat(AVPixelFormat fmt)
{
	const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(fmt);
	return d && (d->flags & AV_PIX_FMT_FLAG_RGB);
}

// A context converting a YUV frame (with these tags) to RGB: decode it with
// its own matrix and range. No-op for an RGB source.
inline void swsDecodeColors(SwsContext *ctx, AVPixelFormat srcFmt, AVColorSpace cs, AVColorRange range,
			    int height)
{
	if (!ctx || isRgbFormat(srcFmt))
		return;
	const int *coef = sws_getCoefficients(swsSpaceOf(cs, height));
	sws_setColorspaceDetails(ctx, coef, isFullRange(range, srcFmt) ? 1 : 0, coef, 1, 0, 1 << 16, 1 << 16);
}

inline void swsDecodeColors(SwsContext *ctx, const AVFrame *f)
{
	if (f)
		swsDecodeColors(ctx, AVPixelFormat(f->format), f->colorspace, f->color_range, f->height);
}

// A context converting RGB to YUV for the encoder: BT.709, limited range --
// what openVideoEncoder writes in the file.
inline void swsEncodeColors709(SwsContext *ctx)
{
	if (!ctx)
		return;
	const int *coef = sws_getCoefficients(SWS_CS_ITU709);
	sws_setColorspaceDetails(ctx, coef, 1, coef, 0, 0, 1 << 16, 1 << 16);
}

// A context converting RGB back into a YUV frame that carries these tags
// (the round trip around a shader pass): the frame's own matrix and range.
inline void swsEncodeColorsLike(SwsContext *ctx, AVPixelFormat dstFmt, AVColorSpace cs, AVColorRange range,
				int height)
{
	if (!ctx || isRgbFormat(dstFmt))
		return;
	const int *coef = sws_getCoefficients(swsSpaceOf(cs, height));
	sws_setColorspaceDetails(ctx, coef, 1, coef, isFullRange(range, dstFmt) ? 1 : 0, 0, 1 << 16, 1 << 16);
}

// Where plane `p` of a `fmt` frame starts for pixel (x, y): the chroma planes
// are subsampled by the format's own shifts, not always by two -- 4:4:4 has
// none, and a 4:2:0 offset there crops (or places) the colour in the wrong spot.
// Bytes per pixel in a plane come from the format too, so a packed frame
// (RGB24, RGBA, NV12's interleaved chroma) is offset by whole pixels.
inline int planeOffset(const AVFrame *f, int plane, int x, int y)
{
	const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
	const bool chroma = plane == 1 || plane == 2;
	const int sx = (d && chroma) ? d->log2_chroma_w : 0;
	const int sy = (d && chroma) ? d->log2_chroma_h : 0;
	int step = 1;
	if (d)
		for (int c = 0; c < d->nb_components; ++c)
			if (d->comp[c].plane == plane)
				step = std::max(step, d->comp[c].step);
	return (y >> sy) * f->linesize[plane] + (x >> sx) * step;
}

// Rows in plane `p` of a `fmt` frame of height h.
inline int planeRows(const AVFrame *f, int plane)
{
	const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
	const bool chroma = plane == 1 || plane == 2;
	const int sy = (d && chroma) ? d->log2_chroma_h : 0;
	return (f->height + (1 << sy) - 1) >> sy;
}

} // namespace harpia
