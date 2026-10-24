#include "ShareSplitter.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <limits>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

namespace harpia::share {
namespace {

struct Media {
	AVFormatContext *input = nullptr;
	AVFormatContext *output = nullptr;
	AVPacket *packet = av_packet_alloc();
	~Media()
	{
		av_packet_free(&packet);
		if (output) {
			if (output->pb)
				avio_closep(&output->pb);
			avformat_free_context(output);
		}
		avformat_close_input(&input);
	}
	bool open(const QString &path)
	{
		return packet && avformat_open_input(&input, path.toUtf8().constData(), nullptr, nullptr) >= 0 &&
		       avformat_find_stream_info(input, nullptr) >= 0;
	}
};

struct Boundary {
	int64_t pts = 0, dts = 0;
	qint64 bytes = 0;
};

struct Outputs {
	QStringList files;
	bool keep = false;
	~Outputs()
	{
		if (!keep)
			for (const QString &path : files)
				QFile::remove(path);
	}
};

bool writePart(const QString &input, const QString &path, const Boundary &start, const Boundary *end,
	       bool first, const std::atomic<bool> &cancel)
{
	Media m;
	if (!m.open(input))
		return false;
	const int video = av_find_best_stream(m.input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	const int audio = av_find_best_stream(m.input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (video < 0 || avformat_alloc_output_context2(&m.output, nullptr, "mp4", path.toUtf8().constData()) < 0 ||
	    !m.output)
		return false;
	const AVRational videoBase = m.input->streams[video]->time_base;
	if (!first && av_seek_frame(m.input, video, start.dts, AVSEEK_FLAG_BACKWARD) < 0)
		return false;
	struct StreamMapping {
		int output = -1;
		int64_t lo = 0, hi = 0, offset = 0;
	};
	std::vector<StreamMapping> mappings(m.input->nb_streams);
	for (int si : {video, audio}) {
		if (si < 0)
			continue;
		AVStream *in = m.input->streams[si];
		AVStream *out = avformat_new_stream(m.output, nullptr);
		if (!out || avcodec_parameters_copy(out->codecpar, in->codecpar) < 0)
			return false;
		out->codecpar->codec_tag = 0;
		out->time_base = in->time_base;
		StreamMapping &mapping = mappings[si];
		mapping.output = out->index;
		// Stream clocks and this part's boundaries stay fixed throughout the
		// remux. Rescale once per stream instead of for every packet.
		mapping.offset = av_rescale_q(start.pts, videoBase, in->time_base);
		mapping.lo = si == video ? start.dts : mapping.offset;
		mapping.hi = !end ? std::numeric_limits<int64_t>::max()
				 : (si == video ? end->dts : av_rescale_q(end->pts, videoBase, in->time_base));
	}
	m.output->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
	if (avio_open(&m.output->pb, path.toUtf8().constData(), AVIO_FLAG_WRITE) < 0)
		return false;
	AVDictionary *options = nullptr;
	av_dict_set(&options, "movflags", "+faststart", 0);
	const int header = avformat_write_header(m.output, &options);
	av_dict_free(&options);
	if (header < 0)
		return false;
	bool videoDone = false, audioDone = audio < 0, wroteVideo = false;
	int read = 0;
	while (!cancel.load() && (read = av_read_frame(m.input, m.packet)) >= 0) {
		AVPacket *packet = m.packet;
		const int si = packet->stream_index;
		if (si != video && si != audio) {
			av_packet_unref(packet);
			continue;
		}
		AVStream *in = m.input->streams[si];
		const StreamMapping &mapping = mappings[si];
		const int64_t clock = si == video ? packet->dts : packet->pts;
		if (clock == AV_NOPTS_VALUE)
			return false;
		// Video stays in its closed GOP; audio follows the presentation-time
		// boundary. A common offset preserves their sync, including B-frames.
		if (clock >= mapping.hi) {
			(si == video ? videoDone : audioDone) = true;
			av_packet_unref(packet);
			if (videoDone && audioDone)
				break;
			continue;
		}
		if (clock < mapping.lo && !(first && si == audio)) {
			av_packet_unref(packet);
			continue;
		}
		if (si == video && !wroteVideo) {
			if (!(packet->flags & AV_PKT_FLAG_KEY))
				return false;
			wroteVideo = true;
		}
		if (packet->pts != AV_NOPTS_VALUE)
			packet->pts -= mapping.offset;
		if (packet->dts != AV_NOPTS_VALUE)
			packet->dts -= mapping.offset;
		AVStream *out = m.output->streams[mapping.output];
		av_packet_rescale_ts(packet, in->time_base, out->time_base);
		packet->stream_index = out->index;
		packet->pos = -1;
		if (av_interleaved_write_frame(m.output, packet) < 0)
			return false;
		av_packet_unref(packet);
	}
	if (cancel.load() || !wroteVideo || (read < 0 && read != AVERROR_EOF))
		return false;
	if (av_write_trailer(m.output) < 0)
		return false;
	return avio_closep(&m.output->pb) >= 0;
}

} // namespace

SplitResult splitMp4(const QString &input, const QString &directory, qint64 maxBytes,
		     const std::atomic<bool> &cancel, const std::function<void(int, qint64)> &progress)
{
	Outputs outputs;
	const auto fail = [&](const QString &message, bool shorterGop = false) {
		return SplitResult{false, cancel.load(), shorterGop, message, {}};
	};
	if (maxBytes <= 0 || !QDir(directory).exists())
		return fail(QStringLiteral("Could not prepare the Discord parts folder."));
	Media index;
	if (!index.open(input))
		return fail(QStringLiteral("Could not read the sharing copy for splitting."));
	const int video = av_find_best_stream(index.input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	const int audio = av_find_best_stream(index.input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (video < 0)
		return fail(QStringLiteral("The sharing copy has no video."));
	std::vector<Boundary> boundaries;
	qint64 bytes = 0;
	int read = 0;
	while (!cancel.load() && (read = av_read_frame(index.input, index.packet)) >= 0) {
		const AVPacket *packet = index.packet;
		if (packet->stream_index == video && (packet->flags & AV_PKT_FLAG_KEY)) {
			if (packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE)
				return fail(QStringLiteral("The sharing copy has invalid video timestamps."));
			boundaries.push_back({packet->pts, packet->dts, bytes});
		}
		if (packet->stream_index == video || packet->stream_index == audio)
			bytes += packet->size;
		av_packet_unref(index.packet);
	}
	if (cancel.load())
		return fail(QString());
	if (read != AVERROR_EOF || boundaries.empty())
		return fail(QStringLiteral("Could not index the sharing copy."));
	// Reserve room for MP4 headers/sample tables. The final on-disk size is
	// still authoritative; an oversized part is retried with fewer GOPs.
	const qint64 reserve = std::min<qint64>(1'000'000, maxBytes / 20);
	const qint64 payloadBudget = maxBytes - reserve;
	const size_t count = boundaries.size();
	qint64 writtenBytes = 0;
	for (size_t start = 0; start < count;) {
		if (cancel.load())
			return fail(QString());
		size_t end = start + 1;
		while (end < count) {
			const qint64 nextBytes = end + 1 < count ? boundaries[end + 1].bytes : bytes;
			if (nextBytes - boundaries[start].bytes > payloadBudget)
				break;
			++end;
		}
		const QString path = QDir(directory).filePath(
			QStringLiteral("part%1.mp4").arg(outputs.files.size() + 1, 3, 10, QLatin1Char('0')));
		while (true) {
			// Reserve this name exclusively. Never truncate an existing file.
			QFile file(path);
			if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
				return fail(QStringLiteral("A Discord part already exists or cannot be created."));
			file.close();
			if (!outputs.files.contains(path))
				outputs.files.append(path);
			const Boundary *last = end < count ? &boundaries[end] : nullptr;
			if (!writePart(input, path, boundaries[start], last, start == 0, cancel))
				return fail(cancel.load() ? QString() : QStringLiteral("Could not write a Discord video part."));
			const qint64 size = QFileInfo(path).size();
			if (size <= maxBytes) {
				writtenBytes += size;
				break;
			}
			QFile::remove(path);
			if (end == start + 1)
				return fail(QStringLiteral("A video frame group exceeds the part size limit."), true);
			end = start + (end - start) / 2;
		}
		start = end;
		if (progress)
			progress(int(start * 100 / count), writtenBytes);
	}
	if (cancel.load())
		return fail(QString());
	outputs.keep = true;
	return {true, false, false, {}, outputs.files};
}

} // namespace harpia::share
