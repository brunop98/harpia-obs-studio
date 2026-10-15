// HDR video to SDR (editor/HdrToSdr): the conversion libobs uses for an HDR
// source on an SDR canvas, here for the editor's preview and exports.
//
//   * Which frames need it: HLG, PQ, or BT.2020 primaries; ordinary BT.709
//     video does not.
//   * HLG greys: black stays black, the 75% reference white lands a little
//     below white, the 1000-nit peak reaches white, and mid-tones are not
//     brighter than showing the signal as if it were SDR (the old, wrong
//     display) -- the complaint was "too bright".
//   * PQ: 300 nits (OBS's SDR white) and the 1000-nit peak.
//   * Colours: BT.2020 green is converted (not clipped to a pale green), and a
//     grey stays grey.
//   * A real 10-bit HLG HEVC file (an iPhone's kind), decoded by FFmpeg: the
//     converter's picture matches the per-pixel maths, and its YUV output is
//     tagged BT.709.
#include "editor/HdrToSdr.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace harpia::hdr;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static int grey(const ToneMapper &tm, double signal)
{
	const uint16_t v = uint16_t(signal * 65535.0 + 0.5);
	const uint16_t in[3] = {v, v, v};
	uint8_t out[3];
	tm.pixel(in, out);
	return out[1];
}

static bool near(int a, int b, int tol)
{
	return std::abs(a - b) <= tol;
}

// First decoded video frame of `path`, or null.
static AVFrame *firstFrame(const std::string &path)
{
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
		return nullptr;
	avformat_find_stream_info(fmt, nullptr);
	const int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	AVFrame *frame = nullptr;
	if (vi >= 0) {
		const AVCodec *codec = avcodec_find_decoder(fmt->streams[vi]->codecpar->codec_id);
		AVCodecContext *dec = avcodec_alloc_context3(codec);
		avcodec_parameters_to_context(dec, fmt->streams[vi]->codecpar);
		if (avcodec_open2(dec, codec, nullptr) >= 0) {
			AVPacket *pkt = av_packet_alloc();
			AVFrame *f = av_frame_alloc();
			while (!frame && av_read_frame(fmt, pkt) >= 0) {
				if (pkt->stream_index == vi && avcodec_send_packet(dec, pkt) >= 0 &&
				    avcodec_receive_frame(dec, f) >= 0)
					frame = f;
				av_packet_unref(pkt);
			}
			if (!frame) {
				avcodec_send_packet(dec, nullptr);
				if (avcodec_receive_frame(dec, f) >= 0)
					frame = f;
				else
					av_frame_free(&f);
			}
			av_packet_free(&pkt);
		}
		avcodec_free_context(&dec);
	}
	avformat_close_input(&fmt);
	return frame;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const std::string work = argc > 1 ? argv[1] : ".";

	std::printf("\n-- which frames --\n");
	ok(kindOf(AVCOL_TRC_ARIB_STD_B67, AVCOL_PRI_BT2020).transfer == Transfer::Hlg &&
		   kindOf(AVCOL_TRC_ARIB_STD_B67, AVCOL_PRI_BT2020).needsConversion(),
	   "HLG (an iPhone's HDR) needs it");
	ok(kindOf(AVCOL_TRC_SMPTE2084, AVCOL_PRI_BT2020).transfer == Transfer::Pq, "PQ (HDR10) needs it");
	ok(kindOf(AVCOL_TRC_BT709, AVCOL_PRI_BT2020).needsConversion(), "SDR in BT.2020 colours: the colours");
	ok(!kindOf(AVCOL_TRC_BT709, AVCOL_PRI_BT709).needsConversion() &&
		   !kindOf(AVCOL_TRC_UNSPECIFIED, AVCOL_PRI_UNSPECIFIED).needsConversion(),
	   "ordinary or untagged video: no");

	std::printf("\n-- HLG greys --\n");
	const ToneMapper hlg(Kind{Transfer::Hlg, true});
	ok(grey(hlg, 0.0) == 0, "black stays black");
	ok(near(grey(hlg, 0.75), 179, 3), "75% (HDR reference white) a little below white");
	ok(grey(hlg, 1.0) == 255, "the 1000-nit peak reaches white");
	bool notBrighter = true, rising = true;
	int last = -1;
	for (int i = 1; i <= 20; ++i) {
		const double s = i / 20.0;
		const int g = grey(hlg, s);
		if (s < 0.95 && g > int(s * 255.0 + 0.5) + 2)
			notBrighter = false;
		if (g < last)
			rising = false;
		last = g;
	}
	ok(notBrighter, "no grey is brighter than the old as-if-SDR display");
	ok(rising, "and brighter signal is never darker");

	std::printf("\n-- PQ --\n");
	const ToneMapper pq(Kind{Transfer::Pq, true});
	// Signal for a light level (inverse EOTF), computed here independently.
	auto pqSignal = [](double nits) {
		const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
		const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
		const double y = std::pow(nits / 10000.0, m1);
		return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
	};
	ok(near(grey(pq, pqSignal(300)), 198, 3), "300 nits (SDR white): 198");
	ok(grey(pq, pqSignal(1000)) == 255 && grey(pq, pqSignal(4000)) == 255, "1000 nits and over: white");
	ok(grey(pq, 0.0) == 0, "black stays black");

	std::printf("\n-- colours --\n");
	{
		// A strong BT.2020 green at HLG reference level, and a grey.
		const uint16_t green[3] = {0, uint16_t(0.75 * 65535), 0};
		uint8_t out[3];
		hlg.pixel(green, out);
		ok(out[1] > 150 && out[0] == 0 && out[2] == 0, "BT.2020 green: a saturated green, no red or blue");
		const uint16_t mid[3] = {30000, 30000, 30000};
		hlg.pixel(mid, out);
		ok(out[0] == out[1] && out[1] == out[2], "a grey stays grey");
		const ToneMapper sdr2020(Kind{Transfer::Sdr, true});
		const uint16_t white[3] = {65535, 65535, 65535};
		sdr2020.pixel(white, out);
		ok(out[0] == 255 && out[1] == 255 && out[2] == 255, "SDR in BT.2020: white is white (no tone curve)");
	}

	std::printf("\n-- a real 10-bit HLG HEVC file --\n");
	const std::string clip = work + "/hlg_ramp.mp4";
	AVFrame *f = firstFrame(clip);
	if (!f) {
		std::printf("  (skipped: %s not made -- no libx265 in this FFmpeg)\n", clip.c_str());
	} else {
		ok(kindOf(f).transfer == Transfer::Hlg && kindOf(f).bt2020, "decoded: tagged HLG, BT.2020");
		Converter conv;
		const int W = f->width, H = f->height;
		std::vector<uint8_t> rgb(size_t(W) * H * 3);
		ok(conv.toRgb(f, W, H, AV_PIX_FMT_RGB24, rgb.data(), W * 3), "converted to RGB");
		// Ten bars of HLG signal 0, .1, .25, .38, .5, .6, .75, .85, .95, 1.
		const double levels[10] = {0, .1, .25, .38, .5, .6, .75, .85, .95, 1};
		bool match = true;
		for (int i = 0; i < 10; ++i) {
			const int x = int((i + 0.5) * W / 10);
			const int got = rgb[(size_t(H / 2) * W + x) * 3 + 1];
			const int want = grey(hlg, levels[i]);
			if (!near(got, want, 6)) {
				std::printf("    bar %d: %d, expected about %d\n", i, got, want);
				match = false;
			}
		}
		ok(match, "each grey bar as the per-pixel maths says (within 10-bit rounding)");

		AVFrame *yuv = av_frame_alloc();
		yuv->format = AV_PIX_FMT_YUV420P;
		yuv->width = W;
		yuv->height = H;
		av_frame_get_buffer(yuv, 0);
		ok(conv.toYuv(f, yuv) && yuv->color_trc == AVCOL_TRC_BT709 && yuv->color_primaries == AVCOL_PRI_BT709 &&
			   yuv->colorspace == AVCOL_SPC_BT709 && yuv->color_range == AVCOL_RANGE_MPEG,
		   "to the encoder: SDR YUV, tagged BT.709");
		// Limited-range luma of the reference-white bar, back from the YUV.
		const int x = int(6.5 * W / 10);
		const int y = yuv->data[0][size_t(H / 2) * size_t(yuv->linesize[0]) + size_t(x)];
		const int expectY = 16 + int(std::lround(grey(hlg, 0.75) * 219.0 / 255.0));
		ok(near(y, expectY, 4), "its luma matches the RGB picture");
		av_frame_free(&yuv);
		av_frame_free(&f);
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
