#pragma once

#include "HdrToSdr.hpp"

#include <QImage>
#include <QString>

#include <cstdint>

extern "C" {
struct AVFormatContext;
struct AVCodecContext;
struct SwsContext;
struct AVPacket;
struct AVFrame;
}

namespace harpia {

// Opens a video file and decodes the frame at an arbitrary timestamp, returning
// it as a QImage scaled to fit a preview box. Used by the trim editor to show
// the frame under a timeline handle in real time. Single-threaded — one seeker
// per user. Ownership may be handed from a preparation worker to the GUI once
// that worker has finished; never call it concurrently.
class FrameSeeker {
public:
	// Exact source-time span represented by a decoded frame. The rounding
	// slack is the same allowance frameAt uses for whole-millisecond requests.
	// Unknown spans deliberately match no other request.
	struct FrameInterval {
		double fromMs = 0.0;
		double untilMs = 0.0;
		double slackMs = 0.0;
		bool known = false;
		bool contains(qint64 ms) const
		{
			const double at = double(ms) + slackMs;
			return known && at >= fromMs && at < untilMs;
		}
	};

	FrameSeeker() = default;
	~FrameSeeker();

	// fastThumbnails: for filmstrips, where "a frame from about here" is enough.
	// Skips the deblocking filter and every non-reference frame, which is what
	// makes seeking through long GOPs expensive — invisible at thumbnail size,
	// and it can land a few frames off the request. Never use it for the
	// preview, which must show the exact frame asked for.
	// decoderThreads = 0 uses the CPU default; comparison grids use 1 so each
	// additional video does not create another full decoder thread pool.
	bool open(const QString &path, bool fastThumbnails = false, int decoderThreads = 0);
	bool isOpen() const { return fmt_ != nullptr; }
	void close();

	qint64 durationMs() const { return durationMs_; }
	int width() const { return width_; }
	int height() const { return height_; }
	double fps() const { return fps_; }
	// libav's short name for the video codec ("h264", "vp9", ...). Empty if
	// unknown. Used to decide whether the file is worth proxying for preview.
	QString codecName() const { return codecName_; }
	// The source timestamp of the last frame decoded, or -1 right after a seek.
	// Sequential playback needs this: nextFrameAt cannot hold position -- it
	// always advances at least one frame -- so a caller pacing itself against a
	// clock has to know when NOT to ask. See nextFrameAt.
	qint64 positionMs() const { return posMs_; }

	// Decode the frame on screen at `ms` -- the last one at or before it, with a
	// millisecond of slack for callers that round -- and return it as an
	// ARGB32 image scaled to fit within maxW x maxH (aspect preserved). Empty
	// QImage on failure.
	// Optimized for live handle-scrubbing and for an export's frame-by-frame
	// walk: requests within the frame on screen return a cached image, and
	// requests ahead roll the decoder forward without seeking — only
	// backward/far jumps pay for a full seek.
	QImage frameAt(qint64 ms, int maxW, int maxH, FrameInterval *interval = nullptr);
	// How many times frameAt has had to seek. A linear walk -- an export --
	// should seek once and then roll forward; the tests hold it to that.
	int seekCount() const { return seeks_; }

	// Sequential playback: seek once, then pull frames in order (efficient — no
	// per-frame seeking). nextFrame returns the next decoded frame and its
	// timestamp (ms) via outMs; empty QImage at end of stream.
	bool seekTo(qint64 ms);
	QImage nextFrame(qint64 *outMs, int maxW, int maxH);

	// Playback catch-up: decode forward until reaching targetMs (or maxFrames /
	// end of stream) and convert ONLY the final frame — skipped frames don't
	// pay the RGBA conversion. Empty QImage at end of stream.
	QImage nextFrameAt(qint64 targetMs, qint64 *outMs, int maxW, int maxH, int maxFrames);

private:
	QImage toImage(AVFrame *f, int maxW, int maxH);
	// Decode the next video frame into seqFrame_ (caller unrefs); false at EOF.
	bool decodeNextInto(qint64 *ptsMs);
	// The next frame in order -- the one frameAt put back first, else the
	// decoder's next -- into `into`; false at the end of the stream.
	bool pullFrame(AVFrame *into, AVPacket *pkt);
	void dropCurrent(); // forget frameAt's frame on screen

	AVFormatContext *fmt_ = nullptr;
	AVCodecContext *dec_ = nullptr;
	SwsContext *sws_ = nullptr;
	int swsW_ = 0;
	int swsH_ = 0;
	int swsFmt_ = -1; // the pixel format sws_ was made for (a stream can change it)
	hdr::Converter hdr_; // HDR (HLG/PQ) or BT.2020 frames: to SDR BT.709 (HdrToSdr.hpp)
	int vIdx_ = -1;

	// Sequential-decode scratch (for nextFrame).
	AVPacket *seqPkt_ = nullptr;
	AVFrame *seqFrame_ = nullptr;
	bool flushed_ = false;

	// frameAt scratch — reused across calls (a drag calls frameAt ~50x/s).
	AVPacket *rndPkt_ = nullptr;
	AVFrame *rndFrame_ = nullptr;
	AVFrame *rndBest_ = nullptr;

	// Scrub acceleration: the position (pts of the last frame handed out, -1 =
	// unknown, e.g. right after a seek) and the last image returned by frameAt
	// (same-frame requests are answered from cache).
	qint64 posMs_ = -1;
	QImage cacheImg_;
	int cacheW_ = 0;
	int cacheH_ = 0;
	// frameAt's frame on screen, kept so a request at another size or a little
	// later can still be answered with it, and the frame after it, decoded to
	// learn that it IS after and put back for the next call.
	AVFrame *cur_ = nullptr;
	AVFrame *ahead_ = nullptr;
	bool haveCur_ = false;
	bool curIsFirst_ = false; // the file's first frame, shown for a time before it
	bool curIsLast_ = false; // decoder exhausted after this frame
	bool haveAhead_ = false;
	double curPtsMs_ = 0.0;
	double aheadPtsMs_ = 0.0;
	// The spacing of the last two frames frameAt decoded in a row (0 = not
	// seen yet), and the time of the last one (-1 = none since a seek).
	double frameDurMs_ = 0.0;
	double lastPtsMs_ = -1.0;
	int seeks_ = 0;

	qint64 durationMs_ = 0;
	int width_ = 0;
	int height_ = 0;
	double fps_ = 0.0;
	QString codecName_;
};

} // namespace harpia
