// The three things that were limiting export quality, and whether they moved.
//
// The quality slider only ever set CRF, and CRF was never the binding
// constraint. Three settings were hard-coded in openVideoEncoder:
//
//   - x264 preset "veryfast" -- a LIVE preset, there so an encoder can keep up
//     with a real-time capture. An export has no such deadline;
//   - 4:2:0 chroma, which stores one colour sample per 2x2 block. Invisible on
//     camera footage, very visible on coloured text and thin lines, which is
//     most of what a screen recorder produces;
//   - no colour metadata at all, so a player has to guess the matrix, and the
//     usual guess for a small frame is BT.601 -- every colour shifted.
//
// None of these can be checked by looking at the Options struct, and two of
// them cannot be checked by looking at the file's headers either. So the
// picture itself is decoded back and compared against the source, per pixel.
//
// Built like exportsize_test (needs libav).
#include "editor/ClipExporter.hpp"
#include "editor/timeline/TimelineModel.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
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

static QString probe(const QString &f, const QString &entries)
{
	QProcess p;
	p.start(QStringLiteral("ffprobe"),
		{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"),
		 QStringLiteral("v:0"), QStringLiteral("-show_entries"), entries,
		 QStringLiteral("-of"), QStringLiteral("default=nw=1:nk=1"), f});
	p.waitForFinished(20000);
	return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

// Decode one frame back out to PNG and load it, so the comparison is against
// the picture rather than against a header field.
static QImage frameOf(const QString &video, const QString &tmpPng)
{
	QProcess p;
	p.start(QStringLiteral("ffmpeg"),
		{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
		 QStringLiteral("-i"), video, QStringLiteral("-frames:v"), QStringLiteral("1"),
		 QStringLiteral("-y"), tmpPng});
	p.waitForFinished(60000);
	return QImage(tmpPng);
}

static bool runExport(const QString &in, const QString &out, const ClipExporter::Options &o,
		      QString *err)
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
	while (!done && t.elapsed() < 180000) {
		QCoreApplication::processEvents();
		QThread::msleep(10);
	}
	return good && done;
}

// Mean absolute error per channel between two same-size images. The measure
// that says whether the picture is actually closer to the original.
static double meanError(const QImage &a, const QImage &b)
{
	if (a.isNull() || b.isNull() || a.size() != b.size())
		return -1.0;
	double sum = 0.0;
	qint64 n = 0;
	for (int y = 0; y < a.height(); ++y) {
		for (int x = 0; x < a.width(); ++x) {
			const QColor p = a.pixelColor(x, y), q = b.pixelColor(x, y);
			sum += std::abs(p.red() - q.red()) + std::abs(p.green() - q.green()) +
			       std::abs(p.blue() - q.blue());
			n += 3;
		}
	}
	return n ? sum / double(n) : -1.0;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	QTemporaryDir dir;
	// Content that behaves like a screen recording rather than like film:
	// saturated text on flat colour, hard edges, no grain. This is the case
	// 4:2:0 mangles and camera footage does not, so a test shot on testsrc
	// would show nothing.
	const QString in = dir.filePath(QStringLiteral("screen.mp4"));
	{
		QProcess ff;
		ff.start(QStringLiteral("ffmpeg"),
			 {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
			  QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
			  QStringLiteral("-i"),
			  QStringLiteral("color=c=white:s=640x360:d=2:r=15"), QStringLiteral("-vf"),
			  QStringLiteral("drawbox=x=0:y=0:w=640:h=360:color=red@1:t=fill,"
					 "drawbox=x=40:y=40:w=560:h=280:color=blue@1:t=fill,"
					 "drawbox=x=80:y=80:w=8:h=200:color=white@1:t=fill,"
					 "drawbox=x=120:y=80:w=8:h=200:color=yellow@1:t=fill"),
			  // Lossless source, so every difference measured later is the
			  // EXPORT's doing and not the fixture's.
			  QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-qp"),
			  QStringLiteral("0"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv444p"),
			  // Tagged the same way the export tags itself. Untagged, a 640x360
			  // stream is decoded as BT.601 while the export says BT.709, and
			  // every comparison below picks up a constant colour offset that
			  // has nothing to do with the encoder.
			  QStringLiteral("-colorspace"), QStringLiteral("bt709"),
			  QStringLiteral("-color_primaries"), QStringLiteral("bt709"),
			  QStringLiteral("-color_trc"), QStringLiteral("bt709"),
			  QStringLiteral("-color_range"), QStringLiteral("tv"),
			  QStringLiteral("-y"), in});
		ff.waitForFinished(60000);
	}
	ok(QFileInfo::exists(in), "made a lossless 640x360 fixture of hard-edged colour");
	if (!QFileInfo::exists(in))
		return 1;
	const QImage source = frameOf(in, dir.filePath(QStringLiteral("src.png")));
	ok(!source.isNull(), "and decoded its first frame to compare against");

	std::printf("\n-- colour metadata is written, so nothing has to guess --\n");
	{
		ClipExporter::Options o;
		o.format = ClipExporter::Format::Mp4;
		QString err;
		const QString out = dir.filePath(QStringLiteral("meta.mp4"));
		ok(runExport(in, out, o, &err), qPrintable(QStringLiteral("exported (%1)").arg(err)));
		const QString cs = probe(out, QStringLiteral("stream=color_space"));
		const QString pr = probe(out, QStringLiteral("stream=color_primaries"));
		std::printf("     colour space \"%s\", primaries \"%s\"\n", qPrintable(cs),
			    qPrintable(pr));
		// Unset, these come back as "unknown" and the player picks -- BT.601 for
		// small frames, which shifts every colour in a downscaled export.
		ok(cs == QStringLiteral("bt709"), "the colour space is stated, not left unknown");
		ok(pr == QStringLiteral("bt709"), "and so are the primaries");
	}

	std::printf("\n-- 4:4:4 actually reaches the file --\n");
	{
		ClipExporter::Options o;
		o.format = ClipExporter::Format::Mp4;
		o.chroma444 = true;
		QString err;
		const QString out = dir.filePath(QStringLiteral("c444.mp4"));
		ok(runExport(in, out, o, &err),
		   qPrintable(QStringLiteral("a 4:4:4 export succeeds (%1)").arg(err)));
		const QString pf = probe(out, QStringLiteral("stream=pix_fmt"));
		const QString prof = probe(out, QStringLiteral("stream=profile"));
		std::printf("     pix_fmt %s, profile %s\n", qPrintable(pf), qPrintable(prof));
		// Asking for 444 with profile "high" is a refusal, not a downgrade, so
		// this also proves the profile followed the pixel format.
		ok(pf == QStringLiteral("yuv444p"), "the stream really is 4:4:4");
		ok(prof.contains(QStringLiteral("4:4:4")), "on a profile that can carry it");
	}

	std::printf("\n-- and 4:4:4 is visibly closer to the original --\n");
	{
		// The point of the option, measured rather than asserted. Same CRF and
		// same effort both times, so chroma is the only difference.
		auto shoot = [&](bool c444, const QString &name) {
			ClipExporter::Options o;
			o.format = ClipExporter::Format::Mp4;
			o.videoCrf = 20;
			o.chroma444 = c444;
			QString err;
			const QString out = dir.filePath(name + QStringLiteral(".mp4"));
			if (!runExport(in, out, o, &err))
				return -1.0;
			return meanError(source, frameOf(out, dir.filePath(name + QStringLiteral(".png"))));
		};
		const double e420 = shoot(false, QStringLiteral("q420"));
		const double e444 = shoot(true, QStringLiteral("q444"));
		std::printf("     mean per-channel error: 4:2:0 %.2f, 4:4:4 %.2f\n", e420, e444);
		ok(e420 >= 0 && e444 >= 0, "both exports decoded back");
		// On hard colour edges this is a large difference, not a marginal one.
		ok(e444 < e420, "4:4:4 is closer to the source picture than 4:2:0");
	}

	std::printf("\n-- the encoder effort setting changes the encode --\n");
	{
		// veryfast vs slow at the SAME CRF. CRF targets a quality, so the sizes
		// differ rather than the error: a slower preset finds the same quality
		// in fewer bits. Either way, a setting that did not reach x264 would
		// give two identical files.
		auto shoot = [&](ClipExporter::Options::Effort ef, const QString &name) -> qint64 {
			ClipExporter::Options o;
			o.format = ClipExporter::Format::Mp4;
			o.videoCrf = 20;
			o.effort = ef;
			QString err;
			const QString out = dir.filePath(name + QStringLiteral(".mp4"));
			if (!runExport(in, out, o, &err)) {
				std::printf("     %s FAILED: %s\n", qPrintable(name), qPrintable(err));
				return -1;
			}
			return QFileInfo(out).size();
		};
		const qint64 fast = shoot(ClipExporter::Options::Effort::Fast, QStringLiteral("ef_fast"));
		const qint64 best = shoot(ClipExporter::Options::Effort::Best, QStringLiteral("ef_best"));
		std::printf("     veryfast %lld B, slow %lld B\n", (long long)fast, (long long)best);
		ok(fast > 0 && best > 0, "both efforts exported");
		// Only that they DIFFER. It is tempting to assert the slow preset is
		// smaller -- that is the usual result -- but CRF targets a quality, not
		// a size, and on trivial content the slower preset can spend slightly
		// more reaching the same place. Measured here: 3036 B veryfast, 3100 B
		// slow. Asserting a direction would be asserting a guess.
		ok(fast != best, "the preset reached the encoder — the two files differ");
	}

	std::printf("\n-- CRF 0 is lossless, and the slider can now reach it --\n");
	{
		ClipExporter::Options o;
		o.format = ClipExporter::Format::Mp4;
		o.videoCrf = 0;
		o.chroma444 = true; // lossless in 4:2:0 still throws chroma away first
		o.effort = ClipExporter::Options::Effort::Balanced;
		QString err;
		const QString out = dir.filePath(QStringLiteral("lossless.mp4"));
		ok(runExport(in, out, o, &err),
		   qPrintable(QStringLiteral("a lossless export succeeds (%1)").arg(err)));
		const QImage back = frameOf(out, dir.filePath(QStringLiteral("lossless.png")));
		const double e = meanError(source, back);
		std::printf("     mean per-channel error vs the source: %.4f\n", e);
		// Not "small". Zero. That is what lossless means, and anything above it
		// means the pipeline lost something on the way to the encoder.
		ok(e == 0.0, "the exported picture is identical to the source, pixel for pixel");
	}

	std::printf("\n-- better quality settings really do cost more --\n");
	{
		auto size = [&](int crf, const QString &name) -> qint64 {
			ClipExporter::Options o;
			o.format = ClipExporter::Format::Mp4;
			o.videoCrf = crf;
			QString err;
			const QString out = dir.filePath(name + QStringLiteral(".mp4"));
			return runExport(in, out, o, &err) ? QFileInfo(out).size() : -1;
		};
		const qint64 rough = size(30, QStringLiteral("crf30"));
		const qint64 fine = size(12, QStringLiteral("crf12"));
		std::printf("     CRF 30 %lld B, CRF 12 %lld B\n", (long long)rough, (long long)fine);
		ok(rough > 0 && fine > rough, "a lower CRF produces a bigger file");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
