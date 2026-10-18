// Every frame, once: a 60 fps project exported at 60 fps shows each of its
// source's frames in turn -- none skipped, none shown twice.
//
// The sources here carry their own frame number in the picture (eight black
// or white bands, one per bit), so a skipped or doubled frame is something a
// test can see. Solid-colour clips, which every other export test uses, look
// the same whichever frame is decoded.
//
// What went wrong: the export asks the decoder for each output frame at a
// time in whole milliseconds -- 0, 17, 33, 50, 67 at 60 fps -- and the decoder
// answered with the first frame AT OR AFTER that time. 17 ms is just past the
// frame at 16.7 ms, so it got the one at 33.3, and 33 ms got it again: a frame
// lost and a frame doubled, about one in three, which plays like a lower frame
// rate in a file that says 60. Now the frame NEAREST the time is shown.
//
// Also checked: the file's frame timestamps are evenly spaced (constant frame
// rate), rather than the 16/17 ms of the millisecond clock the frames are
// composed on.
#include "editor/ClipExporter.hpp"
#include "editor/FrameSeeker.hpp"
#include "editor/timeline/TimelineModel.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// A 64x64 clip whose frame N shows N in binary: band k (8 px wide) is white
// when bit k is set. `startAt` seconds of nothing before its first frame.
static bool makeCounter(const QString &path, const QString &rate, double seconds, double startAt = 0.0)
{
	QProcess ff;
	ff.start(QStringLiteral("ffmpeg"),
		 {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
		  QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
		  QStringLiteral("nullsrc=s=64x64:r=%1:d=%2,format=yuv420p,"
				 "geq=lum='16+219*mod(floor(N/pow(2\\,floor(X/8)))\\,2)':cb=128:cr=128")
			  .arg(rate)
			  .arg(seconds),
		  QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-g"), QStringLiteral("24"),
		  QStringLiteral("-crf"), QStringLiteral("8"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
		  QStringLiteral("-output_ts_offset"), QString::number(startAt), QStringLiteral("-y"), path});
	ff.waitForFinished(60000);
	return ff.exitCode() == 0 && QFileInfo::exists(path);
}

// The number a frame shows; -1 for no picture.
static int counterOf(const QImage &img)
{
	if (img.isNull() || img.width() < 64)
		return -1;
	const QImage rgb = img.convertToFormat(QImage::Format_RGB32);
	int n = 0;
	for (int k = 0; k < 8; ++k)
		if (qGray(rgb.pixel(8 * k + 4, rgb.height() / 2)) > 128)
			n |= 1 << k;
	return n;
}

static bool runExport(const QString &out, const ClipExporter::Options &o, QString *err,
		      const QString &in = QString())
{
	ClipExporter ex;
	bool done = false, good = false;
	QObject::connect(&ex, &ClipExporter::finished, [&](bool okay, bool, const QString &e) {
		good = okay;
		if (err)
			*err = e;
		done = true;
	});
	ex.run(in, out, o);
	QElapsedTimer t;
	t.start();
	while (!done && t.elapsed() < 120000) {
		QCoreApplication::processEvents();
		QThread::msleep(5);
	}
	return good && done;
}

static QStringList probe(const QString &file, const QString &entries)
{
	QProcess p;
	p.start(QStringLiteral("ffprobe"), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"),
					    QStringLiteral("v:0"), QStringLiteral("-show_entries"), entries,
					    QStringLiteral("-of"), QStringLiteral("csv=p=0"), file});
	p.waitForFinished(30000);
	return QString::fromUtf8(p.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

static TlClip video(int source, qint64 out, qint64 from, qint64 to)
{
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = source;
	c.srcStartMs = from;
	c.srcEndMs = to;
	c.outStartMs = out;
	return c;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	qputenv("QT_QPA_PLATFORM", "offscreen");
	QGuiApplication app(argc, argv);
	QTemporaryDir dir;
	const QString c60 = dir.filePath(QStringLiteral("count60.mp4"));
	const QString c30 = dir.filePath(QStringLiteral("count30.mp4"));
	const QString c5994 = dir.filePath(QStringLiteral("count5994.mp4"));
	ok(makeCounter(c60, QStringLiteral("60"), 1.6) && makeCounter(c30, QStringLiteral("30"), 1.6) &&
		   makeCounter(c5994, QStringLiteral("60000/1001"), 1.6),
	   "made 60, 30 and 59.94 fps sources that number their frames");

	std::printf("\n-- the decoder, asked frame by frame the way an export asks --\n");
	int lastSeeks = 0;
	const auto walk = [&lastSeeks](const QString &file, double fps, int frames) {
		FrameSeeker fs;
		QVector<int> got;
		if (!fs.open(file))
			return got;
		for (int i = 0; i < frames; ++i)
			got.append(counterOf(fs.frameAt(qint64(std::llround(i * 1000.0 / fps)), 64, 64)));
		lastSeeks = fs.seekCount();
		return got;
	};
	// Once at the start, and a few times at the very end of the file, where
	// the decoder has handed over everything it had.
	const auto rolled = [&lastSeeks](const char *what) {
		std::printf("     %d seeks\n", lastSeeks);
		ok(lastSeeks <= 5, what);
	};
	const auto exact = [](const QVector<int> &got, int step) {
		int wrong = 0;
		for (int i = 0; i < got.size(); ++i)
			if (got[i] != i * step) {
				if (wrong < 4)
					std::printf("     frame %d: got %d, wanted %d\n", i, got[i], i * step);
				++wrong;
			}
		return wrong;
	};
	{
		const QVector<int> got = walk(c60, 60.0, 96);
		ok(got.size() == 96 && exact(got, 1) == 0, "60 fps source at 60 fps: every frame, in order, once");
		rolled("decoded straight through, not seeked frame by frame");
	}
	{
		const QVector<int> got = walk(c30, 30.0, 48);
		ok(got.size() == 48 && exact(got, 1) == 0, "30 fps at 30 fps: every frame once");
	}
	{
		const QVector<int> got = walk(c5994, 60000.0 / 1001.0, 95);
		ok(got.size() == 95 && exact(got, 1) == 0, "59.94 fps at 59.94 fps: every frame once");
	}
	{
		const QVector<int> got = walk(c60, 30.0, 48);
		ok(got.size() == 48 && exact(got, 2) == 0, "60 fps source at 30 fps: every other frame, evenly");
	}
	{
		// More output frames than source frames: each source frame is held
		// for exactly two, none skipped -- not one, two, three as the
		// millisecond rounding falls.
		const QVector<int> got = walk(c30, 60.0, 96);
		bool steady = got.size() == 96;
		for (int i = 0; i < got.size(); ++i)
			steady = steady && got[i] == i / 2;
		ok(steady, "30 fps source at 60 fps: each frame held for two, evenly");
		rolled("and decoded straight through too");
	}
	{
		// Scrubbing lands on the frame on screen at that time: after a jump
		// back, between two frames, and at the end of the file.
		FrameSeeker fs;
		fs.open(c60);
		ok(counterOf(fs.frameAt(1000, 64, 64)) == 60 && counterOf(fs.frameAt(250, 64, 64)) == 15,
		   "a jump back shows the frame at that time");
		ok(counterOf(fs.frameAt(1012, 64, 64)) == 60 && counterOf(fs.frameAt(1016, 64, 64)) == 61 &&
			   counterOf(fs.frameAt(1015, 64, 64)) == 60,
		   "between two frames, the earlier one -- give or take a rounded millisecond");
		ok(counterOf(fs.frameAt(1590, 64, 64)) == 95 && counterOf(fs.frameAt(1700, 64, 64)) == 95 &&
			   counterOf(fs.frameAt(400, 64, 64)) == 24,
		   "past the end the last frame stays, and the decoder still seeks back after it");
	}
	{
		// A file whose first frame comes 0.2 s in: before it, it shows.
		const QString late = dir.filePath(QStringLiteral("late.mp4"));
		makeCounter(late, QStringLiteral("60"), 0.5, 0.2);
		FrameSeeker fs;
		bool firstFirst = fs.open(late);
		for (int i = 0; i < 12; ++i)
			firstFirst = firstFirst && counterOf(fs.frameAt(qint64(std::llround(i * 1000.0 / 60.0)), 64, 64)) == 0;
		ok(firstFirst, "before a late first frame, the first frame shows");
		ok(counterOf(fs.frameAt(250, 64, 64)) == 3 && fs.seekCount() <= 1,
		   "and the walk into it does not seek on every request");
	}

	std::printf("\n-- a two-track timeline, exported at 60 fps --\n");
	{
		// Bottom: the 60 fps clip, the whole 1.6 s. On top, 0.5 s to 1 s: the
		// 30 fps one.
		ClipExporter::Options o;
		o.format = ClipExporter::Format::Mp4;
		o.keepAudio = false;
		o.videoCrf = 8;
		o.effort = ClipExporter::Options::Effort::Fast;
		TlTrack top, bottom;
		top.kind = bottom.kind = TlTrack::Kind::Video;
		top.clips = {video(2, 500, 0, 500)};
		bottom.clips = {video(1, 0, 0, 1600)};
		o.timeline.tracks = {top, bottom};
		o.timelineSources = {{1, c60.toStdString()}, {2, c30.toStdString()}};
		o.canvasW = 64;
		o.canvasH = 64;
		o.timelineFps = 60.0;
		const QString out = dir.filePath(QStringLiteral("out60.mp4"));
		QString err;
		ok(runExport(out, o, &err), qPrintable(QStringLiteral("it exported (%1)").arg(err)));

		FrameSeeker fs;
		QVector<int> got;
		if (fs.open(out)) {
			fs.seekTo(0);
			qint64 ms = 0;
			for (QImage img = fs.nextFrame(&ms, 64, 64); !img.isNull(); img = fs.nextFrame(&ms, 64, 64))
				got.append(counterOf(img));
		}
		ok(got.size() == 96, qPrintable(QStringLiteral("96 frames for 1.6 s (%1)").arg(got.size())));
		int wrongBottom = 0;
		bool topSteady = true;
		int topLast = -1;
		for (int i = 0; i < got.size(); ++i) {
			if (i >= 30 && i < 60) { // the 30 fps clip on top: each frame held for two
				const int want = (i - 30) / 2;
				topSteady = topSteady && std::abs(got[i] - want) <= 1 && got[i] >= topLast;
				topLast = got[i];
			} else if (got[i] != i) {
				if (wrongBottom < 4)
					std::printf("     frame %d: got %d\n", i, got[i]);
				++wrongBottom;
			}
		}
		ok(!got.isEmpty() && wrongBottom == 0, "the 60 fps track: every frame, once");
		ok(!got.isEmpty() && topSteady && topLast >= 14, "the 30 fps track on top: in order, none skipped");
		QSet<int> distinct;
		for (int i = 0; i < got.size(); ++i)
			if (i < 30 || i >= 60)
				distinct.insert(got[i]);
		std::printf("     %d distinct frames where 66 were due\n", int(distinct.size()));

		const QStringList rate = probe(out, QStringLiteral("stream=avg_frame_rate"));
		std::printf("     the file says %s fps\n", qPrintable(rate.value(0)));
		ok(rate.value(0) == QStringLiteral("60/1"), "the file says 60 fps");
		QVector<double> pts;
		for (const QString &p : probe(out, QStringLiteral("frame=pts_time")))
			pts.append(p.trimmed().toDouble());
		std::sort(pts.begin(), pts.end());
		double shortest = 1e9, longest = 0;
		for (int i = 1; i < pts.size(); ++i) {
			shortest = std::min(shortest, pts[i] - pts[i - 1]);
			longest = std::max(longest, pts[i] - pts[i - 1]);
		}
		std::printf("     frames %.3f to %.3f ms apart\n", shortest * 1000.0, longest * 1000.0);
		ok(pts.size() == 96 && longest - shortest < 0.0005,
		   "every frame lasts the same: a constant frame rate");
	}

	std::printf("\n-- a lower frame rate, chosen in the export window --\n");
	{
		// Every frame of a file, in order, by its number.
		const auto framesOf = [](const QString &file) {
			FrameSeeker fs;
			QVector<int> got;
			if (fs.open(file)) {
				fs.seekTo(0);
				qint64 ms = 0;
				for (QImage img = fs.nextFrame(&ms, 64, 64); !img.isNull(); img = fs.nextFrame(&ms, 64, 64))
					got.append(counterOf(img));
			}
			return got;
		};
		const auto steady = [](const QString &file) {
			QVector<double> pts;
			for (const QString &p : probe(file, QStringLiteral("frame=pts_time")))
				pts.append(p.trimmed().toDouble());
			std::sort(pts.begin(), pts.end());
			double lo = 1e9, hi = 0;
			for (int i = 1; i < pts.size(); ++i) {
				lo = std::min(lo, pts[i] - pts[i - 1]);
				hi = std::max(hi, pts[i] - pts[i - 1]);
			}
			return pts.size() > 1 && hi - lo < 0.0005 && std::abs(lo - 1.0 / 30.0) < 0.0005;
		};
		const auto everyOther = [](const QVector<int> &got) {
			bool all = got.size() == 48;
			for (int i = 0; all && i < got.size(); ++i)
				all = got[i] == 2 * i;
			return all;
		};
		ClipExporter::Options base;
		base.format = ClipExporter::Format::Mp4;
		base.keepAudio = false;
		base.videoCrf = 8;
		base.effort = ClipExporter::Options::Effort::Fast;
		base.outFps = 30.0;
		QString err;

		const QString single = dir.filePath(QStringLiteral("single30.mp4"));
		ok(runExport(single, base, &err, c60), qPrintable(QStringLiteral("a 60 fps clip, exported at 30 (%1)").arg(err)));
		const QVector<int> a = framesOf(single);
		std::printf("     %d frames: %d %d %d %d ...\n", int(a.size()), a.value(0), a.value(1), a.value(2), a.value(3));
		ok(everyOther(a), "every other frame, in order: none doubled, none missing");
		ok(probe(single, QStringLiteral("stream=avg_frame_rate")).value(0) == QStringLiteral("30/1") && steady(single),
		   "the file says 30 fps, and is: frames 1/30 s apart");

		ClipExporter::Options cuts = base;
		cuts.cuts = {{0, 800, 1.0, 0}, {800, 1600, 1.0, 0}};
		const QString multi = dir.filePath(QStringLiteral("cuts30.mp4"));
		ok(runExport(multi, cuts, &err, c60), qPrintable(QStringLiteral("Multi-Cut at 30 (%1)").arg(err)));
		ok(everyOther(framesOf(multi)) && steady(multi), "Multi-Cut: the same, across the cut");

		ClipExporter::Options tl = base;
		TlTrack only;
		only.kind = TlTrack::Kind::Video;
		only.clips = {video(1, 0, 0, 1600)};
		tl.timeline.tracks = {only};
		tl.timelineSources = {{1, c60.toStdString()}};
		tl.canvasW = 64;
		tl.canvasH = 64;
		tl.timelineFps = 60.0;
		const QString timeline = dir.filePath(QStringLiteral("timeline30.mp4"));
		ok(runExport(timeline, tl, &err), qPrintable(QStringLiteral("a 60 fps timeline at 30 (%1)").arg(err)));
		ok(everyOther(framesOf(timeline)) && steady(timeline) &&
			   probe(timeline, QStringLiteral("stream=avg_frame_rate")).value(0) == QStringLiteral("30/1"),
		   "Full editing: composed at 30, every other frame, steady");
	}

	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
