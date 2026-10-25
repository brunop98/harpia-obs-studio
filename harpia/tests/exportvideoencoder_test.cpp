// Native VT sessions require a Mac. HARPIA_EXPORT_ENCODER_TEST_APPLE can be
// defined for this target (and its helper .cpp) on Linux to exercise the real
// missing-encoder fallback path; that is not an Apple hardware test.
#include "core/ExportVideoEncoder.hpp"

#include <QCoreApplication>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

extern "C" {
#include <libavutil/log.h>
}

using namespace harpia::export_video;
namespace {
int failed = 0;
void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	failed += !condition;
}

struct ContextFree {
	void operator()(AVCodecContext *value) const { avcodec_free_context(&value); }
};
struct FrameFree {
	void operator()(AVFrame *value) const { av_frame_free(&value); }
};
struct PacketFree {
	void operator()(AVPacket *value) const { av_packet_free(&value); }
};
using Context = std::unique_ptr<AVCodecContext, ContextFree>;
using Frame = std::unique_ptr<AVFrame, FrameFree>;
using Packet = std::unique_ptr<AVPacket, PacketFree>;

Config fixture()
{
	Config config;
	config.encoding = Encoding::Software;
	config.width = 64;
	config.height = 48;
	config.timeBase = {1, 90000};
	config.frameRate = {30000, 1001};
	config.sampleAspectRatio = {4, 3};
	config.gopSize = 3;
	config.globalHeader = true;
	config.preset = "veryfast";
	config.colorSpace = AVCOL_SPC_SMPTE170M;
	config.colorPrimaries = AVCOL_PRI_SMPTE170M;
	config.colorTransfer = AVCOL_TRC_SMPTE170M;
	config.colorRange = AVCOL_RANGE_JPEG;
	return config;
}

uint8_t sample(int plane, int x, int y, int index)
{
	return uint8_t((x * 3 + y * 5 + index * 7 + plane * 67) & 255);
}

bool pixelsMatch(const AVFrame *frame, AVPixelFormat expected, int index)
{
	// FFmpeg may expose full-range H.264 as yuvj* without changing samples.
	const bool formatMatches = frame->format == expected ||
		(expected == AV_PIX_FMT_YUV420P && frame->format == AV_PIX_FMT_YUVJ420P) ||
		(expected == AV_PIX_FMT_YUV444P && frame->format == AV_PIX_FMT_YUVJ444P);
	if (!formatMatches)
		return false;
	for (int plane = 0; plane < 3; ++plane) {
		const int shift = plane > 0 && expected == AV_PIX_FMT_YUV420P ? 1 : 0;
		for (int y = 0; y < (frame->height >> shift); ++y)
			for (int x = 0; x < (frame->width >> shift); ++x)
				if (frame->data[plane][y * frame->linesize[plane] + x] !=
				    sample(plane, x, y, index))
					return false;
	}
	return true;
}

void roundTrip(Config config, const char *label)
{
	std::printf("\n-- %s --\n", label);
	AVCodecContext *opened = nullptr;
	Result result;
	const QString error = openH264(config, &opened, &result);
	Context encoder(opened);
	check(error.isEmpty() && encoder, qPrintable(QStringLiteral("opened encoder: %1").arg(error)));
	if (!encoder)
		return;
	check(!result.actualHardware && result.description.contains(QStringLiteral("libx264")),
	      "reports the software encoder actually opened");
	check(encoder->pix_fmt == config.softwareFormat, "preserves the requested chroma format");
	check(av_cmp_q(encoder->time_base, config.timeBase) == 0 &&
	      av_cmp_q(encoder->framerate, config.frameRate) == 0 && encoder->gop_size == config.gopSize,
	      "preserves rational timing and keyframe interval");
	check(av_cmp_q(encoder->sample_aspect_ratio, config.sampleAspectRatio) == 0 &&
	      encoder->colorspace == config.colorSpace && encoder->color_primaries == config.colorPrimaries &&
	      encoder->color_trc == config.colorTransfer && encoder->color_range == config.colorRange,
	      "preserves full-range colour metadata and pixel aspect ratio");
	check(bool(encoder->flags & AV_CODEC_FLAG_GLOBAL_HEADER) == config.globalHeader &&
	      (!config.globalHeader || encoder->extradata_size > 0),
	      "honours the container's global-header requirement");
	Frame source(av_frame_alloc());
	Packet packet(av_packet_alloc());
	if (!source || !packet) {
		check(false, "allocated encoding buffers");
		return;
	}
	source->format = encoder->pix_fmt;
	source->width = encoder->width;
	source->height = encoder->height;
	source->colorspace = config.colorSpace;
	source->color_primaries = config.colorPrimaries;
	source->color_trc = config.colorTransfer;
	source->color_range = config.colorRange;
	source->sample_aspect_ratio = config.sampleAspectRatio;
	if (av_frame_get_buffer(source.get(), 32) < 0) {
		check(false, "allocated frame planes");
		return;
	}
	std::vector<Packet> packets;
	auto receivePackets = [&]() {
		for (;;) {
			const int status = avcodec_receive_packet(encoder.get(), packet.get());
			if (status == AVERROR(EAGAIN) || status == AVERROR_EOF)
				return true;
			if (status < 0)
				return false;
			Packet copy(av_packet_clone(packet.get()));
			av_packet_unref(packet.get());
			if (!copy)
				return false;
			packets.push_back(std::move(copy));
		}
	};
	constexpr int frameCount = 8;
	bool encodeOkay = true;
	for (int index = 0; index < frameCount && encodeOkay; ++index) {
		if (av_frame_make_writable(source.get()) < 0) {
			encodeOkay = false;
			break;
		}
		for (int plane = 0; plane < 3; ++plane) {
			const int shift = plane > 0 && config.softwareFormat == AV_PIX_FMT_YUV420P ? 1 : 0;
			for (int y = 0; y < (config.height >> shift); ++y)
				for (int x = 0; x < (config.width >> shift); ++x)
					source->data[plane][y * source->linesize[plane] + x] = sample(plane, x, y, index);
		}
		source->pts = 9009 + index * 3003;
		encodeOkay = avcodec_send_frame(encoder.get(), source.get()) >= 0 && receivePackets();
	}
	const size_t beforeFlush = packets.size();
	encodeOkay = encodeOkay && avcodec_send_frame(encoder.get(), nullptr) >= 0 && receivePackets();
	check(encodeOkay && packets.size() == frameCount, "flush emits every submitted frame");
	check(packets.size() > beforeFlush, "delayed frames survive the encoder flush");
	if (!encodeOkay || packets.size() != frameCount)
		return;
	bool packetTimes = true;
	int keyframes = 0;
	int64_t lastDts = AV_NOPTS_VALUE;
	for (const Packet &p : packets) {
		packetTimes = packetTimes && p->pts >= 9009 && (p->pts - 9009) % 3003 == 0 &&
			      p->pts < 9009 + frameCount * 3003 && p->dts != AV_NOPTS_VALUE &&
			      (lastDts == AV_NOPTS_VALUE || p->dts > lastDts);
		lastDts = p->dts;
		keyframes += bool(p->flags & AV_PKT_FLAG_KEY);
	}
	check(packetTimes, "packets retain caller PTS and monotonically increasing DTS");
	check(keyframes >= 3, "the requested GOP reaches encoded keyframes");
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	Context decoder(codec ? avcodec_alloc_context3(codec) : nullptr);
	AVCodecParameters *parameters = avcodec_parameters_alloc();
	const bool decoderReady = decoder && parameters &&
		avcodec_parameters_from_context(parameters, encoder.get()) >= 0 &&
		avcodec_parameters_to_context(decoder.get(), parameters) >= 0 &&
		avcodec_open2(decoder.get(), codec, nullptr) >= 0;
	avcodec_parameters_free(&parameters);
	check(decoderReady, "encoded headers open a real H.264 decoder");
	if (!decoderReady)
		return;
	decoder->pkt_timebase = config.timeBase;
	Frame decoded(av_frame_alloc());
	int received = 0;
	bool decodeOkay = bool(decoded), pixelOkay = true, metadataOkay = true, presentationOkay = true;
	auto receiveFrames = [&]() {
		for (;;) {
			const int status = avcodec_receive_frame(decoder.get(), decoded.get());
			if (status == AVERROR(EAGAIN) || status == AVERROR_EOF)
				return true;
			if (status < 0)
				return false;
			presentationOkay = presentationOkay && decoded->pts == 9009 + received * 3003;
			if (config.crf == 0)
				pixelOkay = pixelOkay && pixelsMatch(decoded.get(), config.softwareFormat, received);
			metadataOkay = metadataOkay && decoded->width == config.width &&
				decoded->height == config.height && decoded->colorspace == config.colorSpace &&
				decoded->color_primaries == config.colorPrimaries && decoded->color_trc == config.colorTransfer &&
				decoded->color_range == config.colorRange &&
				av_cmp_q(decoded->sample_aspect_ratio, config.sampleAspectRatio) == 0;
			++received;
			av_frame_unref(decoded.get());
		}
	};
	for (const Packet &p : packets)
		decodeOkay = decodeOkay && avcodec_send_packet(decoder.get(), p.get()) >= 0 && receiveFrames();
	decodeOkay = decodeOkay && avcodec_send_packet(decoder.get(), nullptr) >= 0 && receiveFrames();
	check(decodeOkay && received == frameCount, "real decoder receives all frames including its flush");
	check(presentationOkay, "decoded presentation timestamps preserve the caller's rational cadence");
	check(metadataOkay, "decoded bitstream preserves size, colour range, colour tags, and aspect ratio");
	if (config.crf == 0)
		check(pixelOkay, "CRF zero roundtrips every Y, U, and V sample without a lossless downgrade");
}
} // namespace

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	av_log_set_level(AV_LOG_ERROR);
	Config config = fixture();
	Result result;
	AVCodecContext *context = nullptr;

	check(!openH264(config, nullptr, &result).isEmpty(), "rejects a missing output pointer");
	Config invalid = config;
	invalid.width = 0;
	result.actualHardware = true;
	result.description = QStringLiteral("stale");
	check(!openH264(invalid, &context, &result).isEmpty() && !context &&
	      !result.actualHardware && result.description.isEmpty(), "invalid dimensions fail cleanly and reset stale status");
	invalid = config;
	invalid.timeBase = {0, 1};
	check(!openH264(invalid, &context).isEmpty() && !context, "rejects a nonpositive time base");
	invalid = config;
	invalid.width = 63;
	check(!openH264(invalid, &context).isEmpty() && !context, "rejects odd dimensions for chroma-subsampled export");
	invalid = config;
	invalid.softwareFormat = AV_PIX_FMT_RGB24;
	check(!openH264(invalid, &context).isEmpty() && !context, "rejects an unsupported input format rather than changing it");
	invalid = config;
	invalid.preset = "missing-preset";
	check(!openH264(invalid, &context).isEmpty() && !context, "a failed codec session releases its output context");

	AVCodecContext *metadata = avcodec_alloc_context3(nullptr);
	if (metadata) {
		metadata->flags = AV_CODEC_FLAG_QSCALE | AV_CODEC_FLAG_GLOBAL_HEADER;
		Config common = config;
		common.globalHeader = false;
		applyCommon(metadata, common);
		check(!(metadata->flags & AV_CODEC_FLAG_GLOBAL_HEADER) && (metadata->flags & AV_CODEC_FLAG_QSCALE) &&
		      metadata->color_range == AVCOL_RANGE_JPEG && metadata->sample_aspect_ratio.num == 4,
		      "common metadata applies to other codecs without erasing their quality flag");
		Config defaults;
		applyCommon(metadata, defaults);
		check(metadata->colorspace == AVCOL_SPC_UNSPECIFIED && metadata->color_range == AVCOL_RANGE_UNSPECIFIED &&
		      metadata->sample_aspect_ratio.num == 0, "unset colour metadata and aspect ratio remain unset");
		avcodec_free_context(&metadata);
	} else {
		check(false, "allocated metadata fixture");
	}
	applyCommon(nullptr, config);

	config.encoding = Encoding::Automatic;
	config.crf = 0;
	QString error = openH264(config, &context, &result);
	check(error.isEmpty() && context && !result.actualHardware && result.note.contains(QStringLiteral("Lossless")),
	      "automatic lossless export selects software before any native attempt");
	if (context) {
		AVCodecContext *existing = context;
		check(!openH264(config, &context).isEmpty() && context == existing,
		      "rejects an already-owned context without leaking or replacing it");
	}
	avcodec_free_context(&context);
	config.encoding = Encoding::AppleHardware;
	check(openH264(config, &context).contains(QStringLiteral("Lossless")) && !context,
	      "explicit hardware explains that lossless needs software");
	config.crf = 23;
	config.softwareFormat = AV_PIX_FMT_YUV444P;
	check(openH264(config, &context).contains(QStringLiteral("4:4:4")) && !context,
	      "explicit hardware explains that 4:4:4 needs software");
	config.encoding = Encoding::Automatic;
	error = openH264(config, &context, &result);
	check(error.isEmpty() && context && !result.actualHardware &&
	      context->pix_fmt == AV_PIX_FMT_YUV444P && result.note.contains(QStringLiteral("4:4:4")),
	      "automatic 4:4:4 keeps the requested format using software");
	avcodec_free_context(&context);

#if !defined(__APPLE__)
	config = fixture();
	config.encoding = Encoding::AppleHardware;
	error = openH264(config, &context, &result);
	check(!error.isEmpty() && !context && !result.actualHardware,
	      "explicit Apple hardware fails clearly when the native encoder cannot run");
#if !defined(HARPIA_EXPORT_ENCODER_TEST_APPLE)
	check(error.contains(QStringLiteral("macOS")), "unsupported-platform error identifies the required platform");
#endif
	config.encoding = Encoding::Automatic;
	error = openH264(config, &context, &result);
	check(error.isEmpty() && context && !result.actualHardware, "automatic export still opens a real software session");
#if defined(HARPIA_EXPORT_ENCODER_TEST_APPLE)
	check(!result.fallbackReason.isEmpty() && result.description.contains(result.fallbackReason),
	      "a failed native attempt is visible in the actual software encoder description");
	check(context && context->pix_fmt == config.softwareFormat && !(context->flags & AV_CODEC_FLAG_QSCALE),
	      "the failed native session cannot leak its NV12 or rate-control settings into software");
#else
	check(result.fallbackReason.isEmpty(), "automatic export does not claim a native attempt on another platform");
#endif
	avcodec_free_context(&context);
#endif
	config = fixture();
	roundTrip(config, "software 4:2:0 full-range export");
	config.crf = 0;
	roundTrip(config, "lossless 4:2:0 full-range export");
	config.softwareFormat = AV_PIX_FMT_YUV444P;
	config.globalHeader = false;
	config.colorRange = AVCOL_RANGE_MPEG;
	config.colorSpace = AVCOL_SPC_BT709;
	config.colorPrimaries = AVCOL_PRI_BT709;
	config.colorTransfer = AVCOL_TRC_BT709;
	roundTrip(config, "lossless 4:4:4 limited-range export with local headers");
	std::printf("\n%d failures\n", failed);
	return failed ? 1 : 0;
}
