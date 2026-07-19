#include "Remuxer.hpp"

#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace harpia {

bool Remuxer::remux(const std::string &inPath, const std::string &outPath)
{
	AVFormatContext *ifmt = nullptr;
	AVFormatContext *ofmt = nullptr;
	AVPacket *pkt = nullptr;
	bool headerWritten = false;
	bool ok = false;

	if (avformat_open_input(&ifmt, inPath.c_str(), nullptr, nullptr) < 0)
		return false;

	do {
		if (avformat_find_stream_info(ifmt, nullptr) < 0)
			break;

		avformat_alloc_output_context2(&ofmt, nullptr, nullptr, outPath.c_str());
		if (!ofmt)
			break;

		// Map input streams to output streams (video/audio only), copying codec
		// parameters — no decode/encode.
		std::vector<int> streamMap(ifmt->nb_streams, -1);
		int outIdx = 0;
		bool mapOk = true;
		for (unsigned i = 0; i < ifmt->nb_streams; ++i) {
			AVStream *in = ifmt->streams[i];
			const AVMediaType type = in->codecpar->codec_type;
			if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO)
				continue;

			AVStream *out = avformat_new_stream(ofmt, nullptr);
			if (!out) {
				mapOk = false;
				break;
			}
			if (avcodec_parameters_copy(out->codecpar, in->codecpar) < 0) {
				mapOk = false;
				break;
			}
			out->codecpar->codec_tag = 0; // let the muxer pick a valid tag
			streamMap[i] = outIdx++;
		}
		if (!mapOk || outIdx == 0)
			break;

		if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
			if (avio_open(&ofmt->pb, outPath.c_str(), AVIO_FLAG_WRITE) < 0)
				break;
		}

		// +faststart moves the MP4 index to the front so files play/scrub while
		// still downloading; negligible cost on a stream copy.
		AVDictionary *opts = nullptr;
		av_dict_set(&opts, "movflags", "+faststart", 0);
		int hdr = avformat_write_header(ofmt, &opts);
		av_dict_free(&opts);
		if (hdr < 0)
			break;
		headerWritten = true;

		pkt = av_packet_alloc();
		if (!pkt)
			break;

		bool readOk = true;
		while (av_read_frame(ifmt, pkt) >= 0) {
			const int si = pkt->stream_index;
			const int oi = (si >= 0 && (size_t)si < streamMap.size()) ? streamMap[si] : -1;
			if (oi < 0) {
				av_packet_unref(pkt);
				continue;
			}
			AVStream *in = ifmt->streams[si];
			AVStream *out = ofmt->streams[oi];
			pkt->stream_index = oi;
			av_packet_rescale_ts(pkt, in->time_base, out->time_base);
			pkt->pos = -1;
			if (av_interleaved_write_frame(ofmt, pkt) < 0) {
				readOk = false;
				av_packet_unref(pkt);
				break;
			}
			av_packet_unref(pkt);
		}
		ok = readOk;
	} while (false);

	if (pkt)
		av_packet_free(&pkt);
	if (ofmt && headerWritten)
		av_write_trailer(ofmt);
	if (ofmt && !(ofmt->oformat->flags & AVFMT_NOFILE) && ofmt->pb)
		avio_closep(&ofmt->pb);
	if (ofmt)
		avformat_free_context(ofmt);
	avformat_close_input(&ifmt);
	return ok;
}

} // namespace harpia
