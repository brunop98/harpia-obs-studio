#pragma once

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
// per user (call from the same thread).
class FrameSeeker {
public:
	FrameSeeker() = default;
	~FrameSeeker();

	bool open(const QString &path);
	bool isOpen() const { return fmt_ != nullptr; }
	void close();

	qint64 durationMs() const { return durationMs_; }
	int width() const { return width_; }
	int height() const { return height_; }
	double fps() const { return fps_; }

	// Decode the frame nearest to `ms` and return it as an ARGB32 image scaled to
	// fit within maxW x maxH (aspect preserved). Empty QImage on failure.
	// Optimized for live handle-scrubbing: requests within the currently shown
	// frame return a cached image, and requests slightly ahead roll the decoder
	// forward without seeking — only backward/far jumps pay for a full seek.
	QImage frameAt(qint64 ms, int maxW, int maxH);

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

	AVFormatContext *fmt_ = nullptr;
	AVCodecContext *dec_ = nullptr;
	SwsContext *sws_ = nullptr;
	int swsW_ = 0;
	int swsH_ = 0;
	int vIdx_ = -1;

	// Sequential-decode scratch (for nextFrame).
	AVPacket *seqPkt_ = nullptr;
	AVFrame *seqFrame_ = nullptr;
	bool flushed_ = false;

	// Scrub acceleration: the decoder's current position (pts of the last frame
	// consumed, -1 = unknown, e.g. right after a seek) and the last image
	// returned by frameAt (same-frame requests are answered from cache).
	qint64 posMs_ = -1;
	QImage cacheImg_;
	qint64 cacheMs_ = -1;
	int cacheW_ = 0;
	int cacheH_ = 0;

	qint64 durationMs_ = 0;
	int width_ = 0;
	int height_ = 0;
	double fps_ = 0.0;
};

} // namespace harpia
