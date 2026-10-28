// The end-of-video marker bounds the real encoded picture and audio. An
// explicit range export can still use material retained beyond that marker.
#include "editor/ClipExporter.hpp"
#include "editor/FrameSeeker.hpp"
#include "editor/VoiceoverMixer.hpp"
#include "editor/timeline/TimelineSlice.hpp"
#include "editor/tts/TtsAudio.hpp"

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdio>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

using namespace harpia;

static int failures = 0;
static void ok(bool condition, const char *what)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		++failures;
}

struct FileInfo {
	bool valid = false;
	int videoPackets = 0;
	double durationMs = 0.0;
	double audioEndMs = -1.0;
};

static FileInfo inspect(const QString &path)
{
	FileInfo info;
	AVFormatContext *format = nullptr;
	if (avformat_open_input(&format, path.toUtf8().constData(), nullptr, nullptr) < 0 ||
	    avformat_find_stream_info(format, nullptr) < 0) {
		avformat_close_input(&format);
		return info;
	}
	const int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	const int audio = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	info.valid = video >= 0;
	info.durationMs = format->duration > 0 ? double(format->duration) * 1000.0 / AV_TIME_BASE : 0.0;
	AVPacket *packet = av_packet_alloc();
	if (!packet) {
		avformat_close_input(&format);
		return {};
	}
	while (av_read_frame(format, packet) >= 0) {
		if (packet->stream_index == video)
			++info.videoPackets;
		if (packet->stream_index == audio) {
			const int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
			if (pts != AV_NOPTS_VALUE) {
				const AVStream *stream = format->streams[audio];
				const double endMs = double(pts + std::max<int64_t>(0, packet->duration)) *
						     av_q2d(stream->time_base) * 1000.0;
				info.audioEndMs = std::max(info.audioEndMs, endMs);
			}
		}
		av_packet_unref(packet);
	}
	av_packet_free(&packet);
	avformat_close_input(&format);
	return info;
}

static bool runExport(const QString &path, const ClipExporter::Options &options)
{
	ClipExporter exporter;
	bool finished = false, success = false;
	QObject::connect(&exporter, &ClipExporter::finished,
			[&](bool good, bool canceled, const QString &error) {
		finished = true;
		success = good && !canceled;
		if (!success)
			std::printf("     export failed: %s\n", qPrintable(error));
	});
	exporter.run(QString(), path, options);
	return finished && success;
}

static TimelineModel model(qint64 clipMs, bool lateAudio)
{
	TlTrack pictures, audio;
	pictures.kind = TlTrack::Kind::Video;
	audio.kind = TlTrack::Kind::Audio;
	TlClip image;
	image.type = TlClip::Type::Image;
	image.sourceId = 1;
	image.srcEndMs = clipMs;
	pictures.clips = {image};
	TlClip sound;
	sound.type = TlClip::Type::Video;
	sound.sourceId = 2;
	sound.srcEndMs = clipMs;
	sound.fadeInMs = sound.fadeOutMs = 0;
	audio.clips = {sound};
	if (lateAudio) {
		TlClip later = sound;
		later.outStartMs = 1800;
		later.srcEndMs = 600;
		later.transition.enabled = false;
		audio.clips.append(later);
	}
	TimelineModel result;
	result.tracks = {pictures, audio};
	return result;
}

static ClipExporter::Options optionsFor(const TimelineModel &timeline, const QString &image, const QString &tone)
{
	ClipExporter::Options options;
	options.format = ClipExporter::Format::Mp4;
	options.effort = ClipExporter::Options::Effort::Fast;
	options.videoCrf = 16;
	options.canvasW = options.canvasH = 64;
	options.timelineFps = 10.0;
	options.timeline = timeline;
	options.timelineSources = {{1, image.toStdString()}, {2, tone.toStdString()}};
	return options;
}

static tts_audio::Pcm decodeAudio(const QString &path, const QString &wav)
{
	tts_audio::Pcm pcm;
	if (!VoiceoverMixer::decodeToWav(path, wav) || !tts_audio::readWav16(wav, pcm))
		pcm.samples.clear();
	return pcm;
}

static double rms(const tts_audio::Pcm &pcm, qint64 fromMs, qint64 toMs)
{
	const qint64 begin = std::clamp<qint64>(fromMs * pcm.rate / 1000 * pcm.channels, 0, pcm.samples.size());
	const qint64 end = std::clamp<qint64>(toMs * pcm.rate / 1000 * pcm.channels, begin, pcm.samples.size());
	if (begin == end)
		return 0.0;
	double sum = 0.0;
	for (qint64 i = begin; i < end; ++i)
		sum += double(pcm.samples[i]) * pcm.samples[i];
	return std::sqrt(sum / double(end - begin));
}

static QColor colorAt(const QString &path, qint64 atMs)
{
	FrameSeeker seeker;
	if (!seeker.open(path))
		return {};
	const QImage image = seeker.frameAt(atMs, 64, 64);
	return image.isNull() ? QColor() : image.pixelColor(image.width() / 2, image.height() / 2);
}

static bool isRed(const QColor &color)
{
	return color.isValid() && color.red() > 170 && color.green() < 70 && color.blue() < 70;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv);
	av_log_set_level(AV_LOG_ERROR);
	QTemporaryDir scratch;
	if (!scratch.isValid())
		return 2;
	const QString imagePath = scratch.filePath(QStringLiteral("red.png"));
	const QString tonePath = scratch.filePath(QStringLiteral("tone.wav"));
	QImage image(64, 64, QImage::Format_RGB32);
	image.fill(QColor(230, 25, 15));
	tts_audio::Pcm tone;
	tone.samples.resize(48000 * 2 * 3);
	constexpr double pi = 3.14159265358979323846;
	for (int frame = 0; frame < 48000 * 3; ++frame) {
		const qint16 sample = qint16(std::lround(11000.0 * std::sin(2.0 * pi * 440.0 * frame / 48000.0)));
		tone.samples[frame * 2] = tone.samples[frame * 2 + 1] = sample;
	}
	if (!image.save(imagePath) || !tts_audio::writeWav16(tonePath, tone)) {
		ok(false, "created self-contained image and audio sources");
		return 1;
	}

	std::printf("\n-- an explicit video end bounds picture and sound --\n");
	TimelineModel original = model(2400, true);
	original.tracks[1].clips[0].fadeOutMs = 1000; // its real fade starts after the marked output ends
	original.videoEndMs = 1000;
	const QString shortened = scratch.filePath(QStringLiteral("shortened.mp4"));
	ok(runExport(shortened, optionsFor(original, imagePath, tonePath)), "exported the marked one-second video");
	const FileInfo shortInfo = inspect(shortened);
	ok(shortInfo.valid && shortInfo.videoPackets == 10, "the output contains exactly ten video frames");
	ok(shortInfo.durationMs >= 990 && shortInfo.durationMs <= 1050 && shortInfo.audioEndMs > 900 &&
	   shortInfo.audioEndMs <= 1050, "neither the mux nor the audio extends to the later clips");
	ok(isRed(colorAt(shortened, 900)), "the last in-range picture remains visible");
	const auto shortPcm = decodeAudio(shortened, scratch.filePath(QStringLiteral("short.wav")));
	ok(!shortPcm.samples.isEmpty() && shortPcm.durationMs() <= 1050 && rms(shortPcm, 850, 950) > 5000,
	   "the sound reaches the boundary without an artificial fade or a long audio tail");
	ok(original.contentDurationMs() == 2400 && original.tracks[1].clips.size() == 2,
	   "exporting keeps the clips beyond the end marker in the project");

	std::printf("\n-- extending the end preserves the trailing gap --\n");
	TimelineModel extended = model(600, false);
	extended.videoEndMs = 1600;
	const QString padded = scratch.filePath(QStringLiteral("padded.mp4"));
	ok(runExport(padded, optionsFor(extended, imagePath, tonePath)), "exported an end marker beyond the last clip");
	const FileInfo paddedInfo = inspect(padded);
	ok(paddedInfo.valid && paddedInfo.videoPackets == 16 && paddedInfo.durationMs >= 1590 &&
	   paddedInfo.durationMs <= 1650, "the output keeps the full 1.6-second marked duration");
	const QColor gap = colorAt(padded, 1200);
	ok(isRed(colorAt(padded, 300)) && gap.isValid() && qGray(gap.rgb()) < 20,
	   "the clip is followed by black frames through the marked end");
	const auto paddedPcm = decodeAudio(padded, scratch.filePath(QStringLiteral("padded.wav")));
	ok(paddedInfo.audioEndMs >= 1550 && paddedPcm.durationMs() >= 1550 &&
	   rms(paddedPcm, 100, 400) > 5000 && rms(paddedPcm, 1000, 1400) < 30,
	   "the audio track fills the trailing gap with silence");

	std::printf("\n-- a chosen range can export retained post-end material --\n");
	const TimelineModel selection = sliceTimeline(original, 1400, 2200);
	ok(selection.durationMs() == 800 && original.videoEndMs == 1000,
	   "the selection owns its end without moving the project's end marker");
	const QString selected = scratch.filePath(QStringLiteral("selection.mp4"));
	ok(runExport(selected, optionsFor(selection, imagePath, tonePath)), "exported a selection beyond the project end");
	const FileInfo selectedInfo = inspect(selected);
	ok(selectedInfo.valid && selectedInfo.videoPackets == 8 && selectedInfo.durationMs >= 790 &&
	   selectedInfo.durationMs <= 850 && isRed(colorAt(selected, 600)),
	   "the selected later picture is rebased into an eight-frame video");
	const auto selectedPcm = decodeAudio(selected, scratch.filePath(QStringLiteral("selected.wav")));
	ok(rms(selectedPcm, 100, 250) > 5000 &&
	   rms(selectedPcm, 600, 750) > rms(selectedPcm, 100, 250) * 1.3,
	   "the later audio clip survives and enters at its rebased selection time");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
