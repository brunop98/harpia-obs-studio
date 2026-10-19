// swscale writes past the end of its destination; the images it writes into
// have room for it.
//
// Its unscaled YUV -> RGB converters finish the last row's 16-pixel block
// whatever the width, so an image allocated at its exact size had up to 32
// bytes written after it. A 9:16 clip shown at its own size in the editor's
// Vertical layout corrupted the heap that way, and the crash came later,
// freeing something unrelated. Every 1:1 conversion, every width up to 1928,
// two common formats: the overrun must stay inside swsTargetImage's spare
// bytes. Then the real thing: frames of a 360-wide clip at their own size.
//
//   swsoverrun_test <workdir>   (run_avsync.sh generates av_tall.mp4)
#include "editor/FrameSeeker.hpp"
#include "editor/SwsImage.hpp"

#include <QCoreApplication>

#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *what)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", what);
	if (!c)
		++failures;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	std::printf("\n-- every width, at its own size --\n");
	{
		const QImage plain(1080, 4, QImage::Format_RGBA8888);
		const QImage padded = swsTargetImage(1080, 4, QImage::Format_RGBA8888);
		ok(!padded.isNull() && padded.bytesPerLine() == plain.bytesPerLine() &&
			   padded.size() == plain.size() && padded.format() == plain.format(),
		   "laid out exactly like a plain QImage");
		const QImage rgb = swsTargetImage(333, 3, QImage::Format_RGB888);
		ok(rgb.bytesPerLine() == QImage(333, 3, QImage::Format_RGB888).bytesPerLine(),
		   "rows aligned as QImage aligns them (RGB888, odd width)");
	}
	int worst = 0, worstW = 0, overran = 0, tried = 0;
	for (AVPixelFormat fmt : {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12}) {
		const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(fmt);
		for (int w = 16; w <= 1928; w += 2) {
			const int h = 6;
			AVFrame *f = av_frame_alloc();
			f->width = w;
			f->height = h;
			f->format = fmt;
			if (av_frame_get_buffer(f, 32) < 0)
				return 1;
			for (int p = 0; p < 4 && f->data[p]; ++p)
				std::memset(f->data[p], 90 + 30 * p,
					    size_t(f->linesize[p]) * (p == 0 ? h : AV_CEIL_RSHIFT(h, d->log2_chroma_h)));
			QImage img = swsTargetImage(w, h, QImage::Format_RGBA8888);
			uchar *tail = img.bits() + img.bytesPerLine() * h;
			std::memset(tail, 0xAB, kSwsTailPad);
			SwsContext *c = sws_getContext(w, h, fmt, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr,
						       nullptr);
			uint8_t *dst[4] = {img.bits(), nullptr, nullptr, nullptr};
			int ds[4] = {int(img.bytesPerLine()), 0, 0, 0};
			sws_scale(c, f->data, f->linesize, 0, h, dst, ds);
			sws_freeContext(c);
			av_frame_free(&f);
			int over = 0;
			for (size_t i = 0; i < kSwsTailPad; ++i)
				if (tail[i] != 0xAB)
					over = int(i + 1);
			++tried;
			if (over) {
				++overran;
				if (over > worst) {
					worst = over;
					worstW = w;
				}
			}
		}
	}
	std::printf("     %d of %d conversions wrote past the last row; the most, %d bytes (width %d)\n", overran,
		    tried, worst, worstW);
	ok(worst < int(kSwsTailPad), "every overrun fits in the spare bytes");

	std::printf("\n-- a 9:16 clip, 360 wide, at its own size --\n");
	const QString tall = QString::fromLocal8Bit(argc > 1 ? argv[1] : ".") + QStringLiteral("/av_tall.mp4");
	FrameSeeker seeker;
	ok(seeker.open(tall), "the clip opens");
	int good = 0;
	for (int ms = 0; ms < 3000; ms += 100) {
		const QImage frame = seeker.frameAt(ms, 0, 0);
		if (frame.size() == QSize(360, 640) && frame.pixel(180, 320) != 0)
			++good;
	}
	ok(good == 30, "thirty frames, each 360x640 and drawn");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
