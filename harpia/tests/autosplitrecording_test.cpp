// Optional native integration test. Requires built libobs, its renderer,
// obs-ffmpeg, obs-x264 and the obs-ffmpeg-mux helper beside this executable.
// Usage: autosplitrecording_test <renderer> <ffmpeg-module> <ffmpeg-data>
//        <x264-module> <x264-data> [--time]
// --time also records a real one-minute part and verifies paused time is not
// counted. Without it, the test exercises size splitting and ordinary takes.
#include "core/RecordingController.hpp"
#include "core/ModeCapabilities.hpp"

#include <obs.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

using namespace harpia;
static int failures = 0;
static void check(bool condition, const char *description)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", description);
	if (!condition)
		++failures;
}

static bool waitFor(const std::function<bool()> &predicate, int timeoutMs)
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	do {
		if (predicate())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	} while (std::chrono::steady_clock::now() < end);
	return predicate();
}

struct MediaStats {
	int videoFrames = 0;
	int audioPackets = 0;
	double duration = 0;
	bool firstVideoKeyframe = false;
	bool monotonic = true;
	bool decodes = true;
};

static MediaStats inspect(const std::string &path)
{
	MediaStats stats;
	AVFormatContext *format = nullptr;
	if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0 ||
	    avformat_find_stream_info(format, nullptr) < 0) {
		stats.decodes = false;
		avformat_close_input(&format);
		return stats;
	}
	stats.duration = format->duration > 0 ? double(format->duration) / AV_TIME_BASE : 0;
	const int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	const int audio = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	AVCodecContext *decoder = nullptr;
	if (video >= 0) {
		const auto *codec = avcodec_find_decoder(format->streams[video]->codecpar->codec_id);
		decoder = avcodec_alloc_context3(codec);
		if (!decoder || avcodec_parameters_to_context(decoder, format->streams[video]->codecpar) < 0 ||
		    avcodec_open2(decoder, codec, nullptr) < 0)
			stats.decodes = false;
	} else {
		stats.decodes = false;
	}
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	std::vector<int64_t> dts(format->nb_streams, AV_NOPTS_VALUE);
	bool seenVideo = false;
	const auto drain = [&]() {
		if (!decoder || !stats.decodes)
			return;
		int result;
		while ((result = avcodec_receive_frame(decoder, frame)) == 0) {
			++stats.videoFrames;
			av_frame_unref(frame);
		}
		if (result != AVERROR(EAGAIN) && result != AVERROR_EOF)
			stats.decodes = false;
	};
	while (av_read_frame(format, packet) >= 0) {
		if (packet->dts != AV_NOPTS_VALUE) {
			auto &last = dts[size_t(packet->stream_index)];
			if (last != AV_NOPTS_VALUE && packet->dts < last)
				stats.monotonic = false;
			last = packet->dts;
		}
		if (packet->stream_index == audio)
			++stats.audioPackets;
		if (packet->stream_index == video) {
			if (!seenVideo)
				stats.firstVideoKeyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
			seenVideo = true;
			if (decoder && stats.decodes) {
				if (avcodec_send_packet(decoder, packet) < 0)
					stats.decodes = false;
				drain();
			}
		}
		av_packet_unref(packet);
	}
	if (decoder && stats.decodes) {
		avcodec_send_packet(decoder, nullptr);
		drain();
	}
	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&decoder);
	avformat_close_input(&format);
	return stats;
}

class TestSource {
public:
	TestSource()
	{
		obs_source_info info = {};
		info.id = "harpia_autosplit_test_input";
		info.type = OBS_SOURCE_TYPE_INPUT;
		info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO;
		info.get_name = [](void *) { return "Auto Split test"; };
		info.create = [](obs_data_t *, obs_source_t *s) -> void * { return s; };
		info.destroy = [](void *) {};
		obs_register_source(&info);
		source_ = obs_source_create(info.id, "auto-split-test-input", nullptr, nullptr);
		obs_set_output_source(0, source_);
		thread_ = std::thread([this] { feed(); });
	}
	~TestSource()
	{
		running_.store(false);
		thread_.join();
		obs_set_output_source(0, nullptr);
		obs_source_release(source_);
	}

private:
	void feed()
	{
		std::vector<uint8_t> pixels(320 * 180 * 4);
		std::vector<float> tone(1600 * 2);
		uint32_t random = 12345;
		uint64_t samples = 0;
		const uint64_t start = os_gettime_ns();
		int frameIndex = 0;
		while (running_.load()) {
			for (size_t at = 0; at < pixels.size(); at += 4) {
				random ^= random << 13;
				random ^= random >> 17;
				random ^= random << 5;
				pixels[at] = uint8_t(random);
				pixels[at + 1] = uint8_t(random >> 8);
				pixels[at + 2] = uint8_t(random >> 16);
				pixels[at + 3] = 255;
			}
			const uint64_t stamp = start + uint64_t(frameIndex) * 1000000000ULL / 30;
			obs_source_frame video = {};
			video.data[0] = pixels.data();
			video.linesize[0] = 320 * 4;
			video.width = 320;
			video.height = 180;
			video.timestamp = stamp;
			video.format = VIDEO_FORMAT_RGBA;
			obs_source_output_video(source_, &video);
			for (size_t i = 0; i < 1600; ++i) {
				const float value = float(0.2 * std::sin(6.283185307179586 * 440 * double(samples++) / 48000));
				tone[i * 2] = value;
				tone[i * 2 + 1] = value;
			}
			obs_source_audio audio = {};
			audio.data[0] = reinterpret_cast<const uint8_t *>(tone.data());
			audio.frames = 1600;
			audio.speakers = SPEAKERS_STEREO;
			audio.format = AUDIO_FORMAT_FLOAT;
			audio.samples_per_sec = 48000;
			audio.timestamp = stamp;
			obs_source_output_audio(source_, &audio);
			++frameIndex;
			const uint64_t next = start + uint64_t(frameIndex) * 1000000000ULL / 30;
			const uint64_t now = os_gettime_ns();
			if (next > now)
				std::this_thread::sleep_for(std::chrono::nanoseconds(next - now));
		}
	}
	obs_source_t *source_ = nullptr;
	std::atomic<bool> running_{true};
	std::thread thread_;
};

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc < 6) {
		std::fprintf(stderr, "Need renderer, ffmpeg module/data, x264 module/data; optional --time\n");
		return 2;
	}
	QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/harpia split %FPS-XXXXXX"));
	check(directory.isValid(), "a recording directory with spaces and literal percent tokens exists");
	if (!directory.isValid() || !obs_startup("en-US", directory.path().toUtf8().constData(), nullptr))
		return 2;
	obs_audio_info audio = {};
	audio.samples_per_sec = 48000;
	audio.speakers = SPEAKERS_STEREO;
	check(obs_reset_audio(&audio), "the native audio mixer starts");
	obs_video_info video = {};
	video.graphics_module = argv[1];
	video.fps_num = 30;
	video.fps_den = 1;
	video.base_width = video.output_width = 320;
	video.base_height = video.output_height = 180;
	video.output_format = VIDEO_FORMAT_NV12;
	video.colorspace = VIDEO_CS_709;
	video.range = VIDEO_RANGE_PARTIAL;
	video.scale_type = OBS_SCALE_BILINEAR;
	check(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "the native renderer starts");
	for (int at : {2, 4}) {
		obs_module_t *module = nullptr;
		check(obs_open_module(&module, argv[at], argv[at + 1]) == MODULE_SUCCESS &&
			      obs_init_module(module),
		      "the recording module loads");
	}
	if (failures) {
		obs_shutdown();
		return 1;
	}
	{
		TestSource input;
		RecordingController recorder;
		Preset preset;
		preset.format = RecordingFormat::MKV;
		preset.videoBitrateKbps = 8000;
		AutoSplitSettings split;
		split.enabled = true;
		split.mode = AutoSplitSettings::Mode::Size;
		split.sizeMB = 1;
		std::atomic<int> starts{0}, stops{0}, switches{0};
		recorder.onStarted = [&] { ++starts; };
		recorder.onFinished = [&](const std::string &) { ++stops; };
		recorder.onFileChanged = [&](const std::string &) { ++switches; };
		const QString existingPart = directory.filePath(QStringLiteral("part.mkv"));
		{
			QFile sentinel(existingPart);
			check(sentinel.open(QIODevice::WriteOnly) && sentinel.write("keep existing file") > 0,
			      "a preexisting internal filename is present before splitting");
		}
		const std::string first = directory.filePath(QStringLiteral("Original %FPS_part001.mkv")).toStdString();
		check(recorder.start(preset, first, split), "a size-limited recording starts");
		{
			obs_output_t *output = obs_get_output_by_name("harpia_file_output");
			obs_data_t *settings = obs_output_get_settings(output);
			check(obs_data_get_bool(settings, "split_file") &&
				      obs_data_get_int(settings, "max_size_mb") == 1 &&
				      obs_data_get_int(settings, "max_time_sec") == 0 &&
				      !obs_data_get_bool(settings, "allow_overwrite"),
			      "only the selected size threshold is active, with overwriting disabled");
			obs_data_release(settings);
			obs_output_release(output);
		}
		check(waitFor([&] { return switches.load() >= 2; }, 12000), "the muxer automatically creates at least three parts");
		check(starts.load() == 1 && stops.load() == 0 && recorder.isRecording(),
		      "splitting keeps the same running encoder and does not signal recording stop");
		check(recorder.pause(true), "the split recording can pause");
		const int pausedSwitches = switches.load();
		std::this_thread::sleep_for(std::chrono::milliseconds(800));
		check(switches.load() == pausedSwitches, "a paused take does not keep splitting files");
		check(!recorder.pause(false), "resuming leaves the recorder unpaused");
		std::this_thread::sleep_for(std::chrono::milliseconds(350));
		recorder.stop();
		check(waitFor([&] { return stops.load() == 1; }, 10000), "one final stop closes the full recording");
		const auto files = recorder.recordedFiles();
		check(files.size() >= 3 && files.front() == first, "the ordered snapshot includes the original and all split files");
		check(recorder.currentFilePath() == first, "the original recording path stays stable for existing callers");
		{
			QFile sentinel(existingPart);
			check(sentinel.open(QIODevice::ReadOnly) && sentinel.readAll() == "keep existing file",
			      "native filename rotation never overwrites an existing file");
		}
		int decodedFrames = 0;
		for (const auto &path : files) {
			const MediaStats stats = inspect(path);
			decodedFrames += stats.videoFrames;
			check(QFileInfo(QString::fromStdString(path)).absolutePath() == directory.path(),
			      "every part stays inside the original take directory");
			check(stats.decodes && stats.videoFrames > 0 && stats.audioPackets > 0,
			      "each part decodes with both video frames and audio packets");
			check(stats.firstVideoKeyframe && stats.monotonic, "each part begins on a keyframe with valid timestamps");
		}
		int splitFrameCounter = 0;
		{
			obs_output_t *output = obs_get_output_by_name("harpia_file_output");
			splitFrameCounter = obs_output_get_total_frames(output);
			std::printf("  split decoded frames: %d; OBS frame counter: %d\n", decodedFrames,
				    splitFrameCounter);
			obs_output_release(output);
		}

		AutoSplitSettings disabled;
		const auto ordinary = directory.filePath(QStringLiteral("ordinary.mkv")).toStdString();
		check(recorder.start(preset, ordinary, disabled), "a subsequent unsplit recording starts");
		std::this_thread::sleep_for(std::chrono::milliseconds(700));
		recorder.stop();
		check(waitFor([&] { return stops.load() == 2; }, 10000), "the unsplit take stops normally");
		check(recorder.recordedFiles() == std::vector<std::string>{ordinary},
		      "a new take clears old segment paths and disabled splitting produces one file");
		const MediaStats single = inspect(ordinary);
		{
			obs_output_t *output = obs_get_output_by_name("harpia_file_output");
			const int ordinaryFrameCounter = obs_output_get_total_frames(output);
			std::printf("  unsplit decoded frames: %d; OBS frame counter: %d\n", single.videoFrames,
				    ordinaryFrameCounter);
			// OBS counts a packet before the muxer's requested-stop cutoff can
			// reject it. Compare with an ordinary take instead of pretending
			// that offered-packet counter describes muxed frames exactly.
			check(splitFrameCounter - decodedFrames == ordinaryFrameCounter - single.videoFrames,
			      "splitting loses no additional frames compared with the ordinary stop path");
			obs_output_release(output);
		}
		check(single.decodes && single.videoFrames > 0 && single.audioPackets > 0,
		      "the ordinary take still decodes with sound and video");

		preset.format = RecordingFormat::GIF;
		check(!recorder.start(preset, first, split) && !recorder.lastStopError().empty(),
		      "GIF splitting is rejected with an actionable reason");
		preset.format = RecordingFormat::MP4;
		preset.captureMode = int(RecordMode::AudioOnly);
		check(!recorder.start(preset, first, split) && !recorder.lastStopError().empty(),
		      "Audio Only splitting is rejected with an actionable reason");
		check(recorder.recordedFiles().empty(), "a failed start never leaves stale files from the previous take");

		if (argc > 6 && std::strcmp(argv[6], "--time") == 0) {
			preset.format = RecordingFormat::MKV;
			preset.captureMode = int(RecordMode::Monitor);
			split.mode = AutoSplitSettings::Mode::Time;
			split.minutes = 1;
			const auto timed = directory.filePath(QStringLiteral("timed_part001.mkv")).toStdString();
			const int before = switches.load();
			check(recorder.start(preset, timed, split), "a one-minute recording limit starts");
			check(recorder.pause(true), "the time-limited take pauses");
			std::this_thread::sleep_for(std::chrono::seconds(3));
			check(!recorder.pause(false), "the time-limited take resumes");
			check(waitFor([&] { return switches.load() > before; }, 65000), "the time limit rotates the file automatically");
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			recorder.stop();
			check(waitFor([&] { return stops.load() == 3; }, 10000), "the time-limited take closes");
			const auto parts = recorder.recordedFiles();
			const MediaStats firstPart = inspect(parts.front());
			check(parts.size() == 2 && firstPart.duration >= 59.8 && firstPart.duration <= 62.5,
			      "one-minute parts count recorded media time rather than paused wall time");
		}
	}
	obs_shutdown();
	return failures ? 1 : 0;
}
