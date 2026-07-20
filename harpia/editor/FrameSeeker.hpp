#pragma once

#include <QImage>
#include <QString>

#include <cstdint>

extern "C" {
struct AVFormatContext;
struct AVCodecContext;
struct SwsContext;
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
	QImage frameAt(qint64 ms, int maxW, int maxH);

private:
	AVFormatContext *fmt_ = nullptr;
	AVCodecContext *dec_ = nullptr;
	SwsContext *sws_ = nullptr;
	int swsW_ = 0;
	int swsH_ = 0;
	int vIdx_ = -1;

	qint64 durationMs_ = 0;
	int width_ = 0;
	int height_ = 0;
	double fps_ = 0.0;
};

} // namespace harpia
