// The cover picture an export carries (editor/CoverArt): its size, the JPEG,
// and the picker that keeps frames as they go by and takes the middle one.
//
//   * Size: the video's shape, at most 1280 on the long side, even numbers.
//   * The JPEG decodes, at that size, and keeps the frame's colour (a BT.709
//     limited-range red comes back red, not shifted).
//   * The picker keeps a bounded number of frames however long the export,
//     and picks the one nearest the middle -- for a short export and a long
//     one, starting at 0 or not.
#include "editor/CoverArt.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// A flat yuv420p frame (limited range) of the given Y/U/V.
static AVFrame *flat(int w, int h, int y, int u, int v)
{
	AVFrame *f = av_frame_alloc();
	f->format = AV_PIX_FMT_YUV420P;
	f->width = w;
	f->height = h;
	av_frame_get_buffer(f, 0);
	for (int r = 0; r < h; ++r)
		std::memset(f->data[0] + r * f->linesize[0], y, size_t(w));
	for (int r = 0; r < h / 2; ++r) {
		std::memset(f->data[1] + r * f->linesize[1], u, size_t(w / 2));
		std::memset(f->data[2] + r * f->linesize[2], v, size_t(w / 2));
	}
	return f;
}

// Decode a JPEG; the first pixel's Y, Cb, Cr (full range) and the size.
static bool decode(const QByteArray &jpg, int &w, int &h, int &y, int &cb, int &cr)
{
	const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
	AVCodecContext *d = avcodec_alloc_context3(c);
	AVPacket *p = av_packet_alloc();
	AVFrame *f = av_frame_alloc();
	bool good = false;
	if (avcodec_open2(d, c, nullptr) >= 0 && av_new_packet(p, int(jpg.size())) >= 0) {
		std::memcpy(p->data, jpg.constData(), size_t(jpg.size()));
		if (avcodec_send_packet(d, p) >= 0 && avcodec_receive_frame(d, f) >= 0) {
			w = f->width;
			h = f->height;
			y = f->data[0][f->linesize[0] * (h / 2) + w / 2];
			cb = f->data[1][f->linesize[1] * (h / 4) + w / 4];
			cr = f->data[2][f->linesize[2] * (h / 4) + w / 4];
			good = true;
		}
	}
	av_frame_free(&f);
	av_packet_free(&p);
	avcodec_free_context(&d);
	return good;
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	std::printf("\n-- size --\n");
	ok(cover::sizeFor(QSize(1920, 1080)) == QSize(1280, 720), "1080p -> 1280 x 720");
	ok(cover::sizeFor(QSize(1080, 1920)) == QSize(720, 1280), "vertical -> 720 x 1280");
	ok(cover::sizeFor(QSize(640, 360)) == QSize(640, 360), "smaller: as it is");
	ok(cover::sizeFor(QSize(0, 0)).isEmpty(), "no video: none");

	std::printf("\n-- the JPEG --\n");
	{
		// BT.709 limited-range pure red: Y 63, Cb 102, Cr 240.
		AVFrame *red = flat(640, 360, 63, 102, 240);
		const QByteArray j = cover::jpeg(red, QSize(320, 180), AVCOL_SPC_BT709, AVCOL_RANGE_MPEG);
		int w = 0, h = 0, y = 0, cb = 0, cr = 0;
		ok(!j.isEmpty() && decode(j, w, h, y, cb, cr), "a JPEG that decodes");
		ok(w == 320 && h == 180, "at the asked size");
		std::printf("     %d bytes; Y %d Cb %d Cr %d\n", int(j.size()), y, cb, cr);
		// JFIF (BT.601 full range) red: Y 76, Cb 85, Cr 255.
		ok(std::abs(y - 76) <= 4 && std::abs(cb - 85) <= 6 && cr >= 245, "red stays red (as JFIF reads it)");
		av_frame_free(&red);
	}

	std::printf("\n-- picking the middle --\n");
	const auto run = [](double start, double seconds, double fps, int keep, double &picked, int &kept) {
		cover::Picker pk(QSize(64, 36), AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, keep);
		const int n = int(seconds * fps);
		for (int i = 0; i < n; ++i) {
			// Each frame's brightness says when it was: Y from 16 up.
			const double t = start + i / fps;
			AVFrame *f = flat(64, 36, 16 + int(200.0 * i / n), 128, 128);
			pk.offer(f, t);
			av_frame_free(&f);
		}
		kept = int(pk.times().size());
		int w, h, y, cb, cr;
		if (!decode(pk.pick(), w, h, y, cb, cr))
			return false;
		// Back from brightness to time (full-range Y of limited 16..235).
		const double lim = 16.0 + (y / 255.0) * 219.0;
		picked = start + (lim - 16.0) / 200.0 * seconds;
		return true;
	};
	double t = 0;
	int kept = 0;
	ok(run(0.0, 4.0, 30.0, 24, t, kept), "a 4 s export: a cover");
	std::printf("     picked %.2f s of 4, %d kept\n", t, kept);
	ok(std::abs(t - 2.0) < 0.3, "from the middle");
	ok(run(0.0, 120.0, 30.0, 24, t, kept), "a 2 minute export: a cover");
	std::printf("     picked %.2f s of 120, %d kept\n", t, kept);
	ok(std::abs(t - 60.0) < 4.0, "from the middle");
	ok(kept < 48, "only a few dozen frames kept, however long");
	ok(run(10.0, 6.0, 25.0, 24, t, kept), "frames that start at 10 s");
	ok(std::abs(t - 13.0) < 0.4, "the middle of THEM (13 s)");
	{
		cover::Picker none(QSize(64, 36), AVCOL_SPC_BT709, AVCOL_RANGE_MPEG);
		ok(none.pick().isEmpty(), "no frames: no cover");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
