#include "ExportVideoEncoder.hpp"

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <memory>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

namespace harpia::export_video {
namespace {

struct ContextDeleter {
	void operator()(AVCodecContext *context) const { avcodec_free_context(&context); }
};
using Context = std::unique_ptr<AVCodecContext, ContextDeleter>;

QString ffmpegError(int code)
{
	char message[AV_ERROR_MAX_STRING_SIZE]{};
	av_strerror(code, message, sizeof(message));
	return QString::fromUtf8(message);
}

int boundedCrf(const Config &config)
{
	return std::clamp(config.crf, 0, 51);
}

QString hardwareIncompatibility(const Config &config)
{
	if (boundedCrf(config) == 0)
		return QStringLiteral("Lossless H.264 requires software encoding.");
	if (config.softwareFormat != AV_PIX_FMT_YUV420P)
		return QStringLiteral("H.264 with 4:4:4 colour requires software encoding.");
	return {};
}

void describe(Result *result, bool hardware, const QString &note, const QString &fallback)
{
	if (!result)
		return;
	result->actualHardware = hardware;
	result->note = note;
	result->fallbackReason = fallback;
	result->description = hardware ? QStringLiteral("Apple hardware H.264 (VideoToolbox)")
				       : QStringLiteral("Software H.264 (libx264)");
	if (!note.isEmpty())
		result->description += QStringLiteral(" — ") + note;
	if (!fallback.isEmpty())
		result->description += QStringLiteral(" — ") + fallback;
}

QString openSoftware(const Config &config, AVCodecContext **out)
{
	const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
	if (!codec)
		return QStringLiteral("The software H.264 encoder (libx264) is not available in this build.");
	Context context(avcodec_alloc_context3(codec));
	if (!context)
		return QStringLiteral("Could not allocate the software H.264 encoder.");
	applyCommon(context.get(), config);
	context->pix_fmt = config.softwareFormat;
	const int crf = boundedCrf(config);
	const char *profile = crf == 0 || config.softwareFormat == AV_PIX_FMT_YUV444P ? "high444" : "high";
	int error = av_opt_set(context->priv_data, "preset", config.preset ? config.preset : "medium", 0);
	if (error >= 0)
		error = av_opt_set(context->priv_data, "profile", profile, 0);
	if (error >= 0)
		error = av_opt_set_int(context->priv_data, "crf", crf, 0);
	if (error >= 0)
		error = avcodec_open2(context.get(), codec, nullptr);
	if (error < 0)
		return QStringLiteral("Could not open the software H.264 encoder (libx264): %1.")
			.arg(ffmpegError(error));
	*out = context.release();
	return {};
}

#if defined(__APPLE__) || defined(HARPIA_EXPORT_ENCODER_TEST_APPLE)
// VideoToolbox quality is a 0..1 quality factor, not x264's CRF. FFmpeg's
// global_quality/FF_QP2LAMBDA is a percentage. Only Apple Silicon supports
// that mode; on Intel use an estimated bitrate, with no software VT fallback.
#if defined(__aarch64__) || defined(__arm64__)
int hardwareQuality(const Config &config)
{
	return std::clamp(int(std::lround(100.0 - (boundedCrf(config) - 1) * 99.0 / 50.0)), 1, 100);
}
#endif

int64_t hardwareBitrate(const Config &config)
{
	const double fps = config.frameRate.num > 0 && config.frameRate.den > 0
			   ? double(config.frameRate.num) / config.frameRate.den
			   : 30.0;
	// Approximate quality for Intel's bitrate mode. This is a target rather
	// than a file-size promise or an equivalence between the two encoders.
	const double bits = double(config.width) * config.height * std::clamp(fps, 1.0, 240.0) *
			    0.13 * std::exp2((23.0 - boundedCrf(config)) / 6.0);
	return int64_t(std::llround(std::clamp(bits, 250000.0, 200000000.0)));
}

QString openHardwareSession(const Config &config, bool quality, AVCodecContext **out)
{
	const AVCodec *codec = avcodec_find_encoder_by_name("h264_videotoolbox");
	if (!codec)
		return QStringLiteral("The Apple H.264 encoder (h264_videotoolbox) is not available in this build.");
	Context context(avcodec_alloc_context3(codec));
	if (!context)
		return QStringLiteral("Could not allocate the Apple H.264 encoder.");
	applyCommon(context.get(), config);
	context->pix_fmt = AV_PIX_FMT_NV12;
	context->max_b_frames = 0;
	if (quality) {
		context->flags |= AV_CODEC_FLAG_QSCALE;
#if defined(__aarch64__) || defined(__arm64__)
		context->global_quality = hardwareQuality(config) * FF_QP2LAMBDA;
#endif
		context->bit_rate = 0;
	} else {
		context->flags &= ~AV_CODEC_FLAG_QSCALE;
		context->global_quality = 0;
		context->bit_rate = hardwareBitrate(config);
	}
	// allow_sw=0 asks VT to REQUIRE hardware. A successful software VT
	// session would otherwise be mistaken for hardware acceleration.
	int error = av_opt_set_int(context->priv_data, "allow_sw", 0, 0);
	if (error >= 0 && av_opt_find(context->priv_data, "require_sw", nullptr, 0, 0))
		error = av_opt_set_int(context->priv_data, "require_sw", 0, 0);
	if (error >= 0)
		error = av_opt_set_int(context->priv_data, "realtime", 0, 0);
	if (error >= 0)
		error = av_opt_set(context->priv_data, "profile", "high", 0);
	if (error >= 0)
		error = avcodec_open2(context.get(), codec, nullptr);
	if (error < 0)
		return QStringLiteral("Could not open Apple hardware H.264: %1.").arg(ffmpegError(error));
	*out = context.release();
	return {};
}

QString openHardware(const Config &config, AVCodecContext **out, QString *note)
{
#if defined(__aarch64__) || defined(__arm64__)
	const QString qualityError = openHardwareSession(config, true, out);
	if (qualityError.isEmpty()) {
		*note = QStringLiteral("Hardware quality %1/100; quality differs from x264 CRF.")
				.arg(hardwareQuality(config));
		return {};
	}
#endif
	// FFmpeg explicitly rejects quality mode on Intel. A fresh ABR context
	// also handles Apple Silicon sessions that do not support that mode.
	const QString bitrateError = openHardwareSession(config, false, out);
	if (bitrateError.isEmpty()) {
		*note = QStringLiteral("Hardware target bitrate %1 Mbps; quality differs from x264 CRF.")
				.arg(double(hardwareBitrate(config)) / 1000000.0, 0, 'f', 1);
#if defined(__aarch64__) || defined(__arm64__)
		*note += QStringLiteral(" Hardware quality mode was unavailable: %1").arg(qualityError);
#endif
		return {};
	}
#if defined(__aarch64__) || defined(__arm64__)
	return qualityError + QStringLiteral(" Bitrate retry: ") + bitrateError;
#else
	return bitrateError;
#endif
}
#endif

} // namespace

void applyCommon(AVCodecContext *context, const Config &config)
{
	if (!context)
		return;
	context->width = config.width;
	context->height = config.height;
	context->time_base = config.timeBase;
	context->framerate = config.frameRate;
	context->sample_aspect_ratio = config.sampleAspectRatio;
	context->gop_size = config.gopSize;
	context->colorspace = config.colorSpace;
	context->color_primaries = config.colorPrimaries;
	context->color_trc = config.colorTransfer;
	context->color_range = config.colorRange;
	if (config.globalHeader)
		context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	else
		context->flags &= ~AV_CODEC_FLAG_GLOBAL_HEADER;
}

QString openH264(const Config &config, AVCodecContext **out, Result *result)
{
	if (result)
		*result = {};
	if (!out)
		return QStringLiteral("No output video encoder was provided.");
	if (*out)
		return QStringLiteral("The output video encoder is already open.");
	if (config.width <= 0 || config.height <= 0)
		return QStringLiteral("The export video dimensions must be positive.");
	if (config.timeBase.num <= 0 || config.timeBase.den <= 0)
		return QStringLiteral("The export video time base must be positive.");
	if (config.softwareFormat != AV_PIX_FMT_YUV420P && config.softwareFormat != AV_PIX_FMT_YUV444P)
		return QStringLiteral("The H.264 export pixel format must be YUV 4:2:0 or YUV 4:4:4.");
	if (config.softwareFormat == AV_PIX_FMT_YUV420P && ((config.width & 1) || (config.height & 1)))
		return QStringLiteral("YUV 4:2:0 export dimensions must be even.");

	QString note;
	QString fallback;
	if (config.encoding != Encoding::Software) {
		note = hardwareIncompatibility(config);
		if (!note.isEmpty() && config.encoding == Encoding::AppleHardware)
			return note + QStringLiteral(" Select Software in the export encoder settings.");
		if (note.isEmpty()) {
#if defined(__APPLE__) || defined(HARPIA_EXPORT_ENCODER_TEST_APPLE)
			const QString error = openHardware(config, out, &note);
			if (error.isEmpty()) {
				describe(result, true, note, {});
				return {};
			}
			if (config.encoding == Encoding::AppleHardware)
				return error + QStringLiteral(" Select Software or Automatic to use libx264.");
			fallback = QStringLiteral("Apple hardware was unavailable; using software. %1").arg(error);
#else
			if (config.encoding == Encoding::AppleHardware)
				return QStringLiteral("Apple hardware H.264 encoding is available only on macOS. Select Software or Automatic.");
#endif
		}
	}
	const QString error = openSoftware(config, out);
	if (!error.isEmpty())
		return fallback.isEmpty() ? error : fallback + QStringLiteral(" ") + error;
	describe(result, false, note, fallback);
	return {};
}

} // namespace harpia::export_video
