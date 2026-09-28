// What an export puts in its pixels: the right colour, in the right place,
// with the detail the source had.
//
//  * colour matrix: an HD source tagged BT.709 comes out the same colour after
//    a timeline export (it used to be decoded AND re-encoded with BT.601);
//  * 4:4:4 crop: a crop of a 4:4:4 export takes its colour from the cropped
//    spot, not from where a 4:2:0 offset would have put it;
//  * odd-sized primary: a multi-source export has no black strip down the edge
//    when the first source's size is odd;
//  * zoom: a clip zoomed in on the timeline keeps the source's fine detail
//    rather than being decoded at canvas size and enlarged.
//
// Makes its own media with ffmpeg in the work dir; reads the results back with
// ffmpeg, which honours the colour tags the way a player does.
#include "editor/ClipExporter.hpp"
#include "editor/timeline/TimelineModel.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
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

static bool ffmpeg(const QStringList &args)
{
	QProcess p;
	p.start(QStringLiteral("ffmpeg"), QStringList{"-y", "-loglevel", "error"} + args);
	p.waitForFinished(120000);
	if (p.exitCode() != 0)
		std::printf("     ffmpeg: %s\n", p.readAllStandardError().constData());
	return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

struct Rgb {
	int w = 0, h = 0;
	QByteArray px;
	const uchar *at(int x, int y) const { return (const uchar *)px.constData() + (size_t(y) * w + x) * 3; }
};

static QSize sizeOf(const QString &f)
{
	QProcess p;
	p.start(QStringLiteral("ffprobe"), {"-v", "error", "-select_streams", "v:0", "-show_entries",
					    "stream=width,height", "-of", "csv=p=0:s=x", f});
	p.waitForFinished(20000);
	const QList<QByteArray> v = p.readAllStandardOutput().trimmed().split('x');
	return v.size() == 2 ? QSize(v[0].toInt(), v[1].toInt()) : QSize();
}

// One frame at `sec`, as RGB24 the way a player would show it.
static Rgb frameOf(const QString &f, double sec)
{
	Rgb r;
	const QSize s = sizeOf(f);
	r.w = s.width();
	r.h = s.height();
	QProcess p;
	p.start(QStringLiteral("ffmpeg"), {"-v", "error", "-ss", QString::number(sec), "-i", f, "-frames:v", "1",
					   "-f", "rawvideo", "-pix_fmt", "rgb24", "-"});
	p.waitForFinished(60000);
	r.px = p.readAllStandardOutput();
	if (r.px.size() != r.w * r.h * 3)
		r.w = r.h = 0;
	return r;
}

static bool runExport(const QString &in, const QString &out, ClipExporter::Options o, QString *err)
{
	ClipExporter ex;
	bool done = false, good = false;
	QObject::connect(&ex, &ClipExporter::finished, [&](bool g, bool c, const QString &e) {
		done = true;
		good = g && !c;
		if (err)
			*err = e;
	});
	ex.run(in, out, o);
	QElapsedTimer t;
	t.start();
	while (!done && t.elapsed() < 180000) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
		QThread::msleep(20);
	}
	if (!good && err)
		std::printf("     err: %s\n", qPrintable(*err));
	return good;
}

static int maxDiff(const uchar *a, const uchar *b)
{
	return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	const QString work = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
	QTemporaryDir dir;
	QString err;

	// ---- media ------------------------------------------------------------
	const QString hd = work + "/col_709.mp4";     // 1280x720 orange, tagged 709
	const QString half = work + "/col_444.mp4";   // 640x360 4:4:4, red | blue
	const QString odd = work + "/col_odd.mp4";    // 321x181 4:4:4 red (odd size)
	const QString other = work + "/col_green.mp4"; // 322x182 green (not quite 16:9)
	const QString still = work + "/col_still.png"; // 1280x720 orange picture
	const QString lines = work + "/col_lines.mp4"; // 1280x720 2px stripes
	bool media = true;
	if (!QFile::exists(hd))
		media &= ffmpeg({"-f", "lavfi", "-i", "color=c=0xD06020:s=1280x720:r=30:d=2", "-vf",
				 "scale=out_color_matrix=bt709:out_range=tv", "-c:v", "libx264", "-qp", "0",
				 "-pix_fmt", "yuv420p", "-colorspace", "bt709", "-color_primaries", "bt709",
				 "-color_trc", "bt709", hd});
	if (!QFile::exists(half))
		media &= ffmpeg({"-f", "lavfi", "-i", "color=c=red:s=640x360:r=30:d=2", "-f", "lavfi", "-i",
				 "color=c=blue:s=320x360:r=30:d=2", "-filter_complex", "[0][1]overlay=320:0",
				 "-c:v", "libx264", "-qp", "0", "-pix_fmt", "yuv444p", half});
	if (!QFile::exists(odd))
		media &= ffmpeg({"-f", "lavfi", "-i", "color=c=red:s=320x180:r=30:d=2", "-vf", "scale=321:181",
				 "-c:v", "libx264",
				 "-qp", "0", "-pix_fmt", "yuv444p", odd});
	if (!QFile::exists(other))
		media &= ffmpeg({"-f", "lavfi", "-i", "color=c=green:s=322x182:r=30:d=2", "-c:v", "libx264",
				 "-qp", "0", "-pix_fmt", "yuv420p", other});
	if (!QFile::exists(still))
		media &= ffmpeg({"-f", "lavfi", "-i", "color=c=0xD06020:s=1280x720", "-frames:v", "1", still});
	if (!QFile::exists(lines))
		media &= ffmpeg({"-f", "lavfi", "-i", "color=c=black:s=1280x720:r=30:d=2", "-vf",
				 "geq=lum='if(lt(mod(X\\,4)\\,2)\\,235\\,16)':cb=128:cr=128", "-c:v", "libx264",
				 "-qp", "0", "-pix_fmt", "yuv420p", lines});
	if (!media) {
		std::printf("could not make the test media\n");
		return 2;
	}

	std::printf("\n-- colour matrix (timeline export of a BT.709 source) --\n");
	{
		ClipExporter::Options o;
		o.format = ClipExporter::Format::Mp4;
		TlTrack t;
		t.kind = TlTrack::Kind::Video;
		TlClip a;
		a.type = TlClip::Type::Video;
		a.sourceId = 1;
		a.srcStartMs = 0;
		a.srcEndMs = 1000;
		t.clips.append(a);
		o.timeline.tracks.append(t);
		o.timelineSources = {{1, hd.toStdString()}};
		o.canvasW = 1280;
		o.canvasH = 720;
		o.timelineFps = 30.0;
		const QString f = dir.path() + "/tl709.mp4";
		ok(runExport(hd, f, o, &err), "exports");
		const Rgb src = frameOf(hd, 0.5), got = frameOf(f, 0.5);
		if (src.w && got.w) {
			const uchar *a0 = src.at(src.w / 2, src.h / 2), *b0 = got.at(got.w / 2, got.h / 2);
			std::printf("     source %d,%d,%d  export %d,%d,%d\n", a0[0], a0[1], a0[2], b0[0], b0[1], b0[2]);
			ok(maxDiff(a0, b0) <= 4, "same colour after export (within 4/255)");
		} else {
			ok(false, "frames readable");
		}
	}

	std::printf("\n-- colour matrix (timeline export of a still picture) --\n");
	{
		// A picture is RGB: its colour reaches the file only through the
		// encode conversion, which must use the matrix the file is tagged with.
		ClipExporter::Options o;
		o.format = ClipExporter::Format::Mp4;
		TlTrack t;
		t.kind = TlTrack::Kind::Video;
		TlClip a;
		a.type = TlClip::Type::Image;
		a.sourceId = 1;
		a.srcStartMs = 0;
		a.srcEndMs = 1000;
		t.clips.append(a);
		o.timeline.tracks.append(t);
		o.timelineSources = {{1, still.toStdString()}};
		o.canvasW = 1280;
		o.canvasH = 720;
		o.timelineFps = 30.0;
		const QString f = dir.path() + "/tlstill.mp4";
		ok(runExport(still, f, o, &err), "exports");
		const Rgb got = frameOf(f, 0.5);
		if (got.w) {
			const uchar want[3] = {0xD0, 0x60, 0x20};
			const uchar *b0 = got.at(got.w / 2, got.h / 2);
			std::printf("     picture 208,96,32  export %d,%d,%d\n", b0[0], b0[1], b0[2]);
			ok(maxDiff(want, b0) <= 4, "same colour after export (within 4/255)");
		} else {
			ok(false, "frame readable");
		}
	}

	std::printf("\n-- 4:4:4 crop takes the cropped spot's colour --\n");
	{
		ClipExporter::Options o;
		o.format = ClipExporter::Format::Mp4;
		o.chroma444 = true;
		o.startMs = 0;
		o.endMs = 1000;
		o.crop = true;
		o.cropX = 320; // the blue half; a 4:2:0 offset would read chroma at x=160 (red)
		o.cropY = 0;
		o.cropW = 320;
		o.cropH = 360;
		const QString f = dir.path() + "/crop444.mp4";
		ok(runExport(half, f, o, &err), "exports");
		const Rgb got = frameOf(f, 0.5);
		if (got.w) {
			const uchar *c = got.at(got.w / 2, got.h / 2);
			std::printf("     centre %d,%d,%d\n", c[0], c[1], c[2]);
			ok(c[2] > 200 && c[0] < 40, "centre is blue");
			const uchar *e = got.at(2, got.h / 2);
			ok(e[2] > 200 && e[0] < 40, "left edge is blue too");
		} else {
			ok(false, "frame readable");
		}
	}

	std::printf("\n-- multi-source with an odd-sized primary: no edge strip --\n");
	{
		ClipExporter::Options o;
		o.format = ClipExporter::Format::Mp4;
		o.inputs = {odd.toStdString(), other.toStdString()};
		ClipExporter::Cut c1;
		c1.startMs = 0;
		c1.endMs = 1000;
		c1.source = 0;
		ClipExporter::Cut c2 = c1;
		c2.source = 1;
		o.cuts = {c1, c2};
		const QString f = dir.path() + "/multiodd.mp4";
		ok(runExport(odd, f, o, &err), "exports");
		for (double at : {0.5, 1.5}) {
			const Rgb got = frameOf(f, at);
			if (!got.w) {
				ok(false, "frame readable");
				continue;
			}
			// Every edge pixel the colour of the middle: no black (or, before
			// the 4:4:4 fix, magenta) strip where the scaled picture fell short.
			const uchar *mid = got.at(got.w / 2, got.h / 2);
			int worst = 0;
			for (int y = 0; y < got.h; y += 3)
				for (int x : {0, 1, got.w - 2, got.w - 1})
					worst = std::max(worst, maxDiff(got.at(x, y), mid));
			for (int x = 0; x < got.w; x += 3)
				for (int y : {0, 1, got.h - 2, got.h - 1})
					worst = std::max(worst, maxDiff(got.at(x, y), mid));
			std::printf("     %.1fs: %dx%d, centre %d,%d,%d, worst edge difference %d\n", at, got.w, got.h,
				    mid[0], mid[1], mid[2], worst);
			ok(worst <= 40, at < 1.0 ? "first source reaches every edge" : "second source reaches every edge");
			// And the right colour: these SD sources are BT.601, and the file
			// has to say so (it used to claim BT.709: red 255,23,0).
			const uchar want[2][3] = {{255, 0, 0}, {0, 128, 0}};
			ok(maxDiff(mid, want[at < 1.0 ? 0 : 1]) <= 6, "true colour (within 6/255)");
		}
	}

	std::printf("\n-- timeline zoom keeps the source's detail --\n");
	{
		ClipExporter::Options o;
		o.format = ClipExporter::Format::Mp4;
		TlTrack t;
		t.kind = TlTrack::Kind::Video;
		TlClip a;
		a.type = TlClip::Type::Video;
		a.sourceId = 1;
		a.srcStartMs = 0;
		a.srcEndMs = 1000;
		a.scale = 4.0; // 1280 wide source on a 320 canvas: 1:1 source pixels
		t.clips.append(a);
		o.timeline.tracks.append(t);
		o.timelineSources = {{1, lines.toStdString()}};
		o.canvasW = 320;
		o.canvasH = 180;
		o.timelineFps = 30.0;
		const QString f = dir.path() + "/zoom.mp4";
		ok(runExport(lines, f, o, &err), "exports");
		const Rgb got = frameOf(f, 0.5);
		if (got.w) {
			// 2-px stripes: shown 1:1 the row swings between near-black and
			// near-white; decoded at canvas size and enlarged it is flat grey.
			int lo = 255, hi = 0;
			for (int x = got.w / 4; x < got.w * 3 / 4; ++x) {
				const int g = got.at(x, got.h / 2)[1];
				lo = std::min(lo, g);
				hi = std::max(hi, g);
			}
			std::printf("     centre row green range %d..%d\n", lo, hi);
			ok(hi - lo > 120, "stripes still resolved (contrast > 120)");
		} else {
			ok(false, "frame readable");
		}
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
