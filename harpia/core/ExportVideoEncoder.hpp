#pragma once

#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace harpia::export_video {

enum class Encoding { Automatic, Software, AppleHardware };

struct Config {
	Encoding encoding = Encoding::Automatic;
	int width = 0;
	int height = 0;
	AVRational timeBase{1, 30};
	AVRational frameRate{30, 1};
	AVRational sampleAspectRatio{0, 1};
	int gopSize = 60;
	AVPixelFormat softwareFormat = AV_PIX_FMT_YUV420P;
	AVColorSpace colorSpace = AVCOL_SPC_UNSPECIFIED;
	AVColorPrimaries colorPrimaries = AVCOL_PRI_UNSPECIFIED;
	AVColorTransferCharacteristic colorTransfer = AVCOL_TRC_UNSPECIFIED;
	AVColorRange colorRange = AVCOL_RANGE_UNSPECIFIED;
	bool globalHeader = false;
	int crf = 23;
	const char *preset = "medium";
};

struct Result {
	bool actualHardware = false;
	// Names the encoder actually opened, followed by any fallback explanation.
	QString description;
	QString note;
	QString fallbackReason;
};

// Metadata shared by H.264 and other export codecs. Does not set pixel format,
// quality controls, codec-specific flags, or any caller-owned frame timestamps.
void applyCommon(AVCodecContext *context, const Config &config);

// Opens a fresh session for this export. No capability result is cached: a
// native encoder can be unavailable while another application is using it.
// Empty string means success; the caller then owns *out (avcodec_free_context).
// *out must initially be null. On failure it stays null and Result is reset;
// passing an already-open context is rejected without changing that context.
QString openH264(const Config &config, AVCodecContext **out, Result *result = nullptr);

} // namespace harpia::export_video
