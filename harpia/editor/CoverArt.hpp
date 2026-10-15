#pragma once

// The picture inside an exported MP4/MOV that a file manager shows for it: a
// cover (the iTunes "covr" tag, written by FFmpeg for an attached picture),
// taken from the export's own finished frames -- captions, effects and all.
//
// Without one, Windows picks a frame of the video itself, and the versions of
// a Random text -- the same footage with different words -- all came out
// looking alike. Each file now carries a frame of its own render.
//
// Which frame: the middle of the export. The length is not known while the
// frames go by (four export paths, cuts, speeds), so the picker keeps frames
// at an even spacing as they pass, thinning them out (and doubling the spacing)
// whenever there are too many, and takes the one nearest the middle once the
// last frame is in.

#include <QByteArray>
#include <QPair>
#include <QSize>
#include <QVector>

extern "C" {
#include <libavutil/pixfmt.h>
}

struct AVFrame;

namespace harpia::cover {

// The cover's size for a video of `video` size: the same shape, at most this
// on its long side (YouTube's thumbnail size), even numbers.
constexpr int kMaxSide = 1280;
QSize sizeFor(QSize video);

// `f` as a JPEG at `size`. `cs` and `range` say what the frame's YUV means (the
// encoder's tags: what the file says); the JPEG is standard JFIF, BT.601 full
// range, as every viewer reads it. Empty on failure.
QByteArray jpeg(const AVFrame *f, QSize size, AVColorSpace cs, AVColorRange range);

class Picker {
public:
	Picker(QSize size, AVColorSpace cs, AVColorRange range, int keep = 24);

	// Every frame on its way to the encoder, at its time in seconds.
	void offer(const AVFrame *f, double t);
	// The JPEG nearest the middle of what was offered; empty if nothing was.
	QByteArray pick() const;
	// For tests: how many are kept, and their times.
	QVector<double> times() const;

private:
	QSize size_;
	AVColorSpace cs_;
	AVColorRange range_;
	int keep_;
	double step_ = 0.25;
	double next_ = 0.0;
	double first_ = -1.0, last_ = -1.0;
	QVector<QPair<double, QByteArray>> shots_;
};

} // namespace harpia::cover
