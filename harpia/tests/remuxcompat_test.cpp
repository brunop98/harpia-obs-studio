// Native Apple playback uses hvc1 for HEVC MP4/MOV. A container-only change
// must preserve the recorded frames, AAC packets and A/V timing, and must not
// leak the HEVC tag into Matroska or H.264 output.
#include "core/Remuxer.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/common.h>
}

using namespace harpia;
static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}

static bool run(const QString &program, const QStringList &arguments, QByteArray *output = nullptr)
{
	QProcess process;
	process.start(program, arguments);
	if (!process.waitForStarted(5000) || !process.waitForFinished(60000) ||
	    process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
		std::printf("     process failed: %s\n", process.readAllStandardError().constData());
		return false;
	}
	if (output)
		*output = process.readAllStandardOutput();
	return true;
}

struct Facts {
	bool valid = false;
	AVCodecID videoCodec = AV_CODEC_ID_NONE;
	unsigned videoTag = 0;
	int videoPackets = 0;
	int audioPackets = 0;
	qint64 videoStart = std::numeric_limits<qint64>::max();
	qint64 videoEnd = std::numeric_limits<qint64>::min();
	qint64 audioStart = std::numeric_limits<qint64>::max();
	qint64 audioEnd = std::numeric_limits<qint64>::min();
	QByteArray audioHash;
};

static Facts inspect(const QString &path)
{
	Facts facts;
	AVFormatContext *input = nullptr;
	const QByteArray encoded = path.toUtf8();
	if (avformat_open_input(&input, encoded.constData(), nullptr, nullptr) < 0)
		return facts;
	if (avformat_find_stream_info(input, nullptr) < 0) {
		avformat_close_input(&input);
		return facts;
	}
	int video = -1, audio = -1;
	for (unsigned i = 0; i < input->nb_streams; ++i) {
		const AVCodecParameters *codec = input->streams[i]->codecpar;
		if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			video = int(i);
			facts.videoCodec = codec->codec_id;
			facts.videoTag = codec->codec_tag;
		} else if (codec->codec_id == AV_CODEC_ID_AAC) {
			audio = int(i);
		}
	}
	AVPacket *packet = av_packet_alloc();
	QCryptographicHash audioHash(QCryptographicHash::Sha256);
	if (packet) {
		while (av_read_frame(input, packet) >= 0) {
			const bool picture = packet->stream_index == video;
			const bool sound = packet->stream_index == audio;
			if (picture || sound) {
				const AVRational timebase = input->streams[packet->stream_index]->time_base;
				const int64_t pts = packet->pts == AV_NOPTS_VALUE ? packet->dts : packet->pts;
				if (pts != AV_NOPTS_VALUE) {
					const qint64 start = av_rescale_q(pts, timebase, AV_TIME_BASE_Q);
					const qint64 end = av_rescale_q(pts + packet->duration, timebase, AV_TIME_BASE_Q);
					if (picture) {
						facts.videoStart = std::min(facts.videoStart, start);
						facts.videoEnd = std::max(facts.videoEnd, end);
					} else {
						facts.audioStart = std::min(facts.audioStart, start);
						facts.audioEnd = std::max(facts.audioEnd, end);
					}
				}
				if (picture)
					++facts.videoPackets;
				else {
					++facts.audioPackets;
					audioHash.addData(QByteArrayView(reinterpret_cast<const char *>(packet->data), packet->size));
				}
			}
			av_packet_unref(packet);
		}
	}
	av_packet_free(&packet);
	avformat_close_input(&input);
	facts.audioHash = audioHash.result();
	facts.valid = facts.videoPackets > 0 && facts.audioPackets > 0 &&
		facts.videoEnd > facts.videoStart && facts.audioEnd > facts.audioStart;
	return facts;
}

static QByteArray decodedFrames(const QString &ffmpeg, const QString &path)
{
	QByteArray output;
	if (!run(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"), path,
		QStringLiteral("-map"), QStringLiteral("0:v:0"), QStringLiteral("-f"), QStringLiteral("framemd5"),
		QStringLiteral("-")}, &output))
		return {};
	QByteArray frames;
	for (const QByteArray &line : output.split('\n'))
		if (!line.isEmpty() && !line.startsWith('#'))
			frames += line.split(',').last().trimmed() + '\n';
	return frames;
}

static bool timingMatches(const Facts &source, const Facts &copy)
{
	return std::abs(source.videoStart - copy.videoStart) <= 2000 &&
		std::abs(source.videoEnd - copy.videoEnd) <= 2000 &&
		std::abs(source.audioStart - copy.audioStart) <= 2000 &&
		std::abs(source.audioEnd - copy.audioEnd) <= 2000;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
	QByteArray encoders;
	if (ffmpeg.isEmpty() || !run(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-encoders")}, &encoders) ||
	    !encoders.contains(" libx265 ")) {
		std::printf("SKIP: remuxcompat requires the FFmpeg CLI with libx265 to create its HEVC fixture.\n");
		return 77;
	}
	QTemporaryDir temp;
	if (!temp.isValid())
		return 2;
	const QString input = temp.filePath(QStringLiteral("recording source.mkv"));
	const QStringList inputs = {
		QStringLiteral("-y"), QStringLiteral("-v"), QStringLiteral("error"),
		QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
		QStringLiteral("testsrc2=size=160x90:rate=30:duration=1"),
		QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
		QStringLiteral("sine=frequency=440:sample_rate=48000:duration=1")};
	check(run(ffmpeg, inputs + QStringList{QStringLiteral("-c:v"), QStringLiteral("libx265"),
		QStringLiteral("-preset"), QStringLiteral("ultrafast"), QStringLiteral("-x265-params"),
		QStringLiteral("log-level=error:pools=1:frame-threads=1:bframes=0"), QStringLiteral("-pix_fmt"),
		QStringLiteral("yuv420p"), QStringLiteral("-c:a"), QStringLiteral("aac"), QStringLiteral("-shortest"), input}),
		"a real one-second HEVC/AAC Matroska recording is generated");
	const Facts source = inspect(input);
	const QByteArray frames = decodedFrames(ffmpeg, input);
	check(source.valid && source.videoCodec == AV_CODEC_ID_HEVC && source.videoPackets == 30 && !frames.isEmpty(),
		"the fixture contains thirty decoded HEVC frames and AAC audio");
	if (!source.valid || frames.isEmpty())
		return 1;
	for (const QString &extension : {QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv")}) {
		const QString output = temp.filePath(QStringLiteral("copy.") + extension);
		check(Remuxer::remux(input.toUtf8().constData(), output.toUtf8().constData()),
			qPrintable(QStringLiteral("HEVC remux to %1 succeeds").arg(extension)));
		const Facts copy = inspect(output);
		const unsigned expectedTag = extension == QLatin1String("mkv") ? source.videoTag : MKTAG('h', 'v', 'c', '1');
		check(copy.valid && copy.videoCodec == AV_CODEC_ID_HEVC && copy.videoTag == expectedTag,
			qPrintable(QStringLiteral("%1 uses its correct HEVC sample entry").arg(extension)));
		check(copy.videoPackets == source.videoPackets && copy.audioPackets == source.audioPackets &&
			copy.audioHash == source.audioHash,
			qPrintable(QStringLiteral("%1 preserves every video packet and the original AAC packet bytes").arg(extension)));
		check(timingMatches(source, copy), qPrintable(QStringLiteral("%1 preserves audio/video start and end times").arg(extension)));
		check(decodedFrames(ffmpeg, output) == frames,
			qPrintable(QStringLiteral("%1 decodes to the original video frames").arg(extension)));
	}
	check(QFileInfo::exists(input), "remuxing retains the original recording");
	if (encoders.contains(" libx264 ")) {
		const QString avc = temp.filePath(QStringLiteral("avc.mkv"));
		const QString output = temp.filePath(QStringLiteral("avc.mp4"));
		check(run(ffmpeg, inputs + QStringList{QStringLiteral("-c:v"), QStringLiteral("libx264"),
			QStringLiteral("-preset"), QStringLiteral("ultrafast"), QStringLiteral("-bf"), QStringLiteral("0"),
			QStringLiteral("-c:a"), QStringLiteral("aac"), QStringLiteral("-shortest"), avc}) &&
			Remuxer::remux(avc.toUtf8().constData(), output.toUtf8().constData()),
			"the existing H.264/AAC remux path still succeeds");
		const Facts copy = inspect(output);
		check(copy.valid && copy.videoCodec == AV_CODEC_ID_H264 && copy.videoTag != MKTAG('h', 'v', 'c', '1'),
			"H.264 keeps its own sample entry rather than receiving an HEVC tag");
	} else {
		std::printf("SKIP: H.264 comparison requires the FFmpeg CLI with libx264.\n");
	}
	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
