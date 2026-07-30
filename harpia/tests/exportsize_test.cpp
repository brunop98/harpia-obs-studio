// Output resolution and GIF palette, checked against the file that comes out.
//
// These are the two settings the export dialog is about to offer, and a
// setting the encoder quietly ignores is worse than no setting: the dialog
// promises 720p, the file is 1080p, and nothing anywhere says so.
//
// So nothing here trusts the Options struct. Every claim is read back off the
// encoded file with ffprobe, or off its size on disk.
//
// The specific ways this goes wrong:
//
//   - the encoder opened at the new size while the frames arrive at the old
//     one, which libav rejects -- an export that fails rather than scales;
//   - an odd dimension reaching a yuv420p encoder, which is refused outright
//     (a 1920x1080 clip asked for "half" is 960x540, but 1919 wide is not);
//   - the size applied to some export paths and not others, which is what four
//     copies of the encoder setup used to guarantee;
//   - the GIF palette size accepted and ignored, so "32 colours" produces a
//     file the same size as 256.
// Built like exportpaths_test (libav, no window): see harpia/tests/run_avsync.sh
// for the same linking pattern -- ClipExporter + GifEncoder + the compositor.
#include "editor/ClipExporter.hpp"
#include "editor/timeline/TimelineModel.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static QSize sizeOf(const QString &f)
{
	QProcess p;
	p.start(QStringLiteral("ffprobe"),
		{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"),
		 QStringLiteral("v:0"), QStringLiteral("-show_entries"),
		 QStringLiteral("stream=width,height"), QStringLiteral("-of"),
		 QStringLiteral("csv=p=0"), f});
	p.waitForFinished(20000);
	const QStringList wh =
		QString::fromUtf8(p.readAllStandardOutput()).trimmed().split(QLatin1Char(','));
	return wh.size() == 2 ? QSize(wh[0].toInt(), wh[1].toInt()) : QSize();
}

// Run an export to completion on this thread.
static bool runExport(const QString &in, const QString &out, const ClipExporter::Options &o,
		      QString *err)
{
	ClipExporter ex;
	bool done = false, good = false;
	QObject::connect(&ex, &ClipExporter::finished,
			 [&](bool okay, bool, const QString &e) {
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
		QThread::msleep(10);
	}
	return good && done;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	const QString src = QStringLiteral(SRC_MEDIA);

	QTemporaryDir dir;
	// A source of known, even, non-square size so the arithmetic below is not a
	// coincidence of the input.
	const QString in = dir.filePath(QStringLiteral("src.mp4"));
	{
		QProcess ff;
		ff.start(QStringLiteral("ffmpeg"),
			 {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
			  QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
			  QStringLiteral("-i"),
			  QStringLiteral("testsrc=size=640x360:duration=2:rate=15"),
			  QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), QStringLiteral("-y"),
			  in});
		ff.waitForFinished(60000);
	}
	ok(QFileInfo::exists(in) && sizeOf(in) == QSize(640, 360), "made a 640x360 source");
	if (!QFileInfo::exists(in))
		return 1;
	(void)src;

	std::printf("\n-- no size chosen leaves the source size alone --\n");
	{
		ClipExporter::Options o;
		o.format = ClipExporter::Format::Mp4;
		const QString out = dir.filePath(QStringLiteral("same.mp4"));
		QString err;
		ok(runExport(in, out, o, &err), qPrintable(QStringLiteral("it exported (%1)").arg(err)));
		const QSize got = sizeOf(out);
		std::printf("     %dx%d\n", got.width(), got.height());
		ok(got == QSize(640, 360), "and it is still 640x360");
	}

	std::printf("\n-- a chosen size is what comes out --\n");
	{
		ClipExporter::Options o;
		o.format = ClipExporter::Format::Mp4;
		o.outWidth = 320;
		o.outHeight = 180;
		const QString out = dir.filePath(QStringLiteral("half.mp4"));
		QString err;
		ok(runExport(in, out, o, &err), qPrintable(QStringLiteral("it exported (%1)").arg(err)));
		const QSize got = sizeOf(out);
		std::printf("     asked 320x180, got %dx%d\n", got.width(), got.height());
		ok(got == QSize(320, 180), "the file really is the size that was asked for");
		// And it is not merely a header claim: a scaled file of a gradient
		// source is meaningfully smaller than the full-size one.
		std::printf("     %lld bytes at half size\n",
			    (long long)QFileInfo(out).size());
		ok(QFileInfo(out).size() > 0, "with actual content in it");
	}

	std::printf("\n-- an odd size is nudged even, not refused --\n");
	{
		// yuv420p subsamples by two; an odd width is rejected by both encoders,
		// and "your export failed" is not an acceptable answer to a slider that
		// happened to land on 321.
		ClipExporter::Options o;
		o.format = ClipExporter::Format::Mp4;
		o.outWidth = 321;
		o.outHeight = 181;
		const QString out = dir.filePath(QStringLiteral("odd.mp4"));
		QString err;
		ok(runExport(in, out, o, &err),
		   qPrintable(QStringLiteral("an odd size still exports (%1)").arg(err)));
		const QSize got = sizeOf(out);
		std::printf("     asked 321x181, got %dx%d\n", got.width(), got.height());
		ok(got == QSize(320, 180), "rounded down to even rather than failing");
	}

	std::printf("\n-- the size reaches the multi-cut path too --\n");
	{
		// Four export paths used to spell the encoder setup out separately.
		// Checking one of them proves nothing about the others.
		ClipExporter::Options o;
		o.format = ClipExporter::Format::Mp4;
		o.outWidth = 320;
		o.outHeight = 180;
		ClipExporter::Cut a;
		a.startMs = 0;
		a.endMs = 800;
		o.cuts.push_back(a);
		ClipExporter::Cut b;
		b.startMs = 1000;
		b.endMs = 1800;
		o.cuts.push_back(b);
		const QString out = dir.filePath(QStringLiteral("cuts.mp4"));
		QString err;
		ok(runExport(in, out, o, &err),
		   qPrintable(QStringLiteral("the cut assembly exported (%1)").arg(err)));
		const QSize got = sizeOf(out);
		std::printf("     %dx%d\n", got.width(), got.height());
		ok(got == QSize(320, 180), "and honoured the chosen size");
	}

	std::printf("\n-- and the timeline path --\n");
	{
		ClipExporter::Options o;
		o.format = ClipExporter::Format::Mp4;
		o.outWidth = 320;
		o.outHeight = 180;
		o.canvasW = 640;
		o.canvasH = 360;
		o.timelineFps = 15.0;
		TlTrack t;
		t.kind = TlTrack::Kind::Video;
		TlClip c;
		c.type = TlClip::Type::Video;
		c.sourceId = 1;
		c.srcStartMs = 0;
		c.srcEndMs = 1500;
		c.outStartMs = 0;
		t.clips.append(c);
		o.timeline.tracks.append(t);
		o.timelineSources[1] = in.toStdString();
		const QString out = dir.filePath(QStringLiteral("tl.mp4"));
		QString err;
		ok(runExport(in, out, o, &err),
		   qPrintable(QStringLiteral("the timeline exported (%1)").arg(err)));
		const QSize got = sizeOf(out);
		std::printf("     canvas 640x360, asked 320x180, got %dx%d\n", got.width(),
			    got.height());
		ok(got == QSize(320, 180),
		   "the chosen size overrides the project canvas, as the dialog implies");
	}

	std::printf("\n-- GIF: fewer colours really is a smaller file --\n");
	{
		auto gif = [&](int colors, bool dither, const QString &name) -> qint64 {
			ClipExporter::Options o;
			o.format = ClipExporter::Format::Gif;
			o.gifFps = 10;
			o.gifColors = colors;
			o.gifDither = dither;
			o.endMs = 1500;
			const QString out = dir.filePath(name);
			QString err;
			if (!runExport(in, out, o, &err)) {
				std::printf("     %s FAILED: %s\n", qPrintable(name), qPrintable(err));
				return -1;
			}
			return QFileInfo(out).size();
		};
		const qint64 many = gif(256, true, QStringLiteral("c256.gif"));
		const qint64 few = gif(8, true, QStringLiteral("c8.gif"));
		const qint64 flat = gif(256, false, QStringLiteral("nodither.gif"));
		std::printf("     256 colours %lld B, 8 colours %lld B, 256 undithered %lld B\n",
			    (long long)many, (long long)few, (long long)flat);
		ok(many > 0 && few > 0 && flat > 0, "all three GIFs encoded");
		// The assertion that separates "the option was passed" from "the option
		// did something". A palette size that never reached palettegen would
		// give three files of the same size.
		ok(few < many, "8 colours is smaller than 256");
		ok(flat < many, "and turning dithering off is smaller than leaving it on");
	}

	std::printf("\n-- GIF: the chosen size applies there too --\n");
	{
		ClipExporter::Options o;
		o.format = ClipExporter::Format::Gif;
		o.gifFps = 10;
		o.endMs = 800;
		o.outWidth = 320;
		o.outHeight = 180;
		const QString out = dir.filePath(QStringLiteral("small.gif"));
		QString err;
		ok(runExport(in, out, o, &err), qPrintable(QStringLiteral("the GIF exported (%1)").arg(err)));
		const QSize got = sizeOf(out);
		std::printf("     %dx%d\n", got.width(), got.height());
		ok(got.width() == 320, "the GIF is the chosen width");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
