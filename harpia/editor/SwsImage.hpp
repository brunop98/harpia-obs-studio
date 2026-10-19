#pragma once

// Room for swscale to overrun.
//
// sws_scale writes PAST the end of its destination in some conversions. Its
// unscaled YUV -> RGB converters work in blocks of 16 pixels and finish the
// last row's block regardless: for an RGBA row whose width is 8 to 15 pixels
// over a multiple of 16, that is up to 32 bytes beyond the last pixel. Every
// width from 1080 (a vertical 1080x1920 video) to the 360 of a phone clip is
// one of them, about a quarter of all widths. Inside the image that is
// harmless -- the next row is written over it -- but after the LAST row it
// lands in whatever the heap put there, which is how showing a 9:16 clip
// at its own size corrupted the heap and crashed the editor later, somewhere
// else entirely.
//
// So nothing sws_scale writes into is allocated at its exact size: an AVFrame
// from av_frame_get_buffer is already padded by FFmpeg, and the rest use these.

#include <QImage>

#include <cstdlib>

namespace harpia {

// Generous: the worst seen is 32 bytes (RGBA) and 96 (RGB48), and other
// converters and CPUs may block differently.
constexpr size_t kSwsTailPad = 256;

// QImage(w, h, format), but with kSwsTailPad spare bytes after its last row,
// for sws_scale to write into. Same width, height, format and bytesPerLine;
// only the allocation is longer. The image owns the buffer and frees it with
// its last copy. Null on a bad size or a failed allocation, like QImage.
inline QImage swsTargetImage(int w, int h, QImage::Format format)
{
	if (w <= 0 || h <= 0)
		return {};
	const int bpp = QImage::toPixelFormat(format).bitsPerPixel();
	const qsizetype bytesPerLine = ((qsizetype(w) * bpp + 31) / 32) * 4; // as QImage aligns rows
	void *data = std::malloc(size_t(bytesPerLine) * size_t(h) + kSwsTailPad);
	if (!data)
		return {};
	return QImage(static_cast<uchar *>(data), w, h, bytesPerLine, format,
		      [](void *p) { std::free(p); }, data);
}

} // namespace harpia
