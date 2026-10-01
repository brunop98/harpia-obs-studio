// Batch export through the real exporter: three cuts to three files with the
// right lengths and names, a cut that cannot be exported is reported and the
// rest still written, Cancel stops after the clip being written, and text
// variations: one Full-editing timeline, one file per version, the same
// footage with only the words changed.
#include "editor/BatchExport.hpp"
#include "editor/ClipExporter.hpp"
#include "editor/timeline/TextVariations.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
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

static double durationOf(const QString &f)
{
	QProcess p;
	p.start(QStringLiteral("ffprobe"),
		{"-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", f});
	p.waitForFinished(20000);
	return p.readAllStandardOutput().trimmed().toDouble();
}

// One frame at `sec`, as raw 64x36 RGB bytes (small: enough to see the words).
static QByteArray frameAt(const QString &f, double sec)
{
	QProcess p;
	p.start(QStringLiteral("ffmpeg"), {"-v", "error", "-ss", QString::number(sec), "-i", f, "-frames:v", "1",
					   "-vf", "scale=64:36", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"});
	p.waitForFinished(20000);
	return p.readAllStandardOutput();
}

static int bytesApart(const QByteArray &a, const QByteArray &b)
{
	if (a.size() != b.size() || a.isEmpty())
		return -1;
	int n = 0;
	for (int i = 0; i < a.size(); ++i)
		if (std::abs(int(uchar(a[i])) - int(uchar(b[i]))) > 24)
			++n;
	return n;
}

// Run a batch to the end (or until `cancelAfterFirst`), pumping events.
struct Run {
	int okCount = -1, failCount = -1;
	bool canceled = false, done = false;
	QVector<int> finishedItems;
	int lastPct = -1;
};
static Run runBatch(BatchExporter &b, const QString &in, const ClipExporter::Options &base,
		    const QVector<BatchExporter::Item> &items, const QString &folder, bool cancelAfterFirst)
{
	Run r;
	QObject::connect(&b, &BatchExporter::finished, [&](int okc, int fc, bool c) {
		r.okCount = okc;
		r.failCount = fc;
		r.canceled = c;
		r.done = true;
	});
	QObject::connect(&b, &BatchExporter::itemFinished, [&](int i, bool, const QString &, const QString &) {
		r.finishedItems.push_back(i);
		if (cancelAfterFirst && r.finishedItems.size() == 1)
			b.cancel();
	});
	QObject::connect(&b, &BatchExporter::progress, [&](int, int, int pct, qint64) { r.lastPct = pct; });
	if (!b.start(in, base, items, folder)) {
		std::printf("     start refused\n");
		return r;
	}
	QElapsedTimer t;
	t.start();
	while (!r.done && t.elapsed() < 300000) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
		QThread::msleep(10);
	}
	return r;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QGuiApplication app(argc, argv); // text clips need fonts
	const QString work = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
	const QString red = work + "/av_red.mp4";
	if (!QFile::exists(red)) {
		std::printf("missing %s\n", qPrintable(red));
		return 2;
	}
	QTemporaryDir dir;

	ClipExporter::Options base;
	base.format = ClipExporter::Format::Mp4;
	base.keepAudio = true;
	auto cut = [](qint64 a, qint64 b, double sp) {
		ClipExporter::Cut c;
		c.startMs = a;
		c.endMs = b;
		c.speed = sp;
		return c;
	};
	auto item = [](const QString &n, ClipExporter::Cut c) {
		BatchExporter::Item it;
		it.name = n;
		it.cut = c;
		return it;
	};

	std::printf("\n-- three cuts, three files --\n");
	{
		const QString folder = batch_export::uniqueFolder(dir.path(), "Hi");
		BatchExporter b;
		const Run r = runBatch(b, red, base,
				       {item("Hi_01", cut(0, 1000, 1.0)), item("Hi_02", cut(1500, 2500, 2.0)),
					item("Hi_03", cut(3000, 4000, 1.0))},
				       folder, false);
		ok(r.done && r.okCount == 3 && r.failCount == 0 && !r.canceled, "all three exported");
		ok(r.finishedItems == QVector<int>({0, 1, 2}), "in order, each reported once");
		ok(r.lastPct == 100 || r.lastPct >= 95, "progress reached the end");
		const QVector<BatchExporter::Result> res = b.results();
		ok(res.size() == 3 && res[0].path == folder + "/Hi_01.mp4" && res[2].path == folder + "/Hi_03.mp4",
		   "named and placed in the batch folder");
		const double d1 = durationOf(folder + "/Hi_01.mp4"), d2 = durationOf(folder + "/Hi_02.mp4"),
			     d3 = durationOf(folder + "/Hi_03.mp4");
		std::printf("     lengths %.2f %.2f %.2f s\n", d1, d2, d3);
		ok(std::abs(d1 - 1.0) < 0.3 && std::abs(d3 - 1.0) < 0.3, "one-second cuts are one second");
		ok(std::abs(d2 - 0.5) < 0.3, "the 2x cut is half a second");
		ok(!b.running(), "and the runner is idle");
	}

	std::printf("\n-- a cut that cannot be exported does not stop the others --\n");
	{
		const QString folder = batch_export::uniqueFolder(dir.path(), "Mixed");
		BatchExporter b;
		// Past the end of a 4 s file: the exporter clamps it away and refuses.
		const Run r = runBatch(b, red, base,
				       {item("Mixed_01", cut(0, 800, 1.0)), item("Mixed_02", cut(90000, 91000, 1.0)),
					item("Mixed_03", cut(2000, 2800, 1.0))},
				       folder, false);
		ok(r.done && r.okCount == 2 && r.failCount == 1, "two written, one failed");
		const QVector<BatchExporter::Result> res = b.results();
		ok(res.size() == 3 && !res[1].ok && !res[1].error.isEmpty(), "the failure carries a reason");
		std::printf("     reason: %s\n", qPrintable(res.size() > 1 ? res[1].error : QString()));
		ok(!QFile::exists(folder + "/Mixed_02.mp4"), "no empty file for the failed one");
		ok(QFile::exists(folder + "/Mixed_03.mp4"), "the one after it was still written");
		ok(batch_export::summary(r.okCount, r.failCount, 3, r.canceled) == "2 of 3 clips exported (1 failed)",
		   "and the summary says so");
	}

	std::printf("\n-- Cancel stops after the clip being written --\n");
	{
		const QString folder = batch_export::uniqueFolder(dir.path(), "Stop");
		BatchExporter b;
		const Run r = runBatch(b, red, base,
				       {item("Stop_01", cut(0, 1000, 1.0)), item("Stop_02", cut(1000, 2000, 1.0)),
					item("Stop_03", cut(2000, 3000, 1.0))},
				       folder, true);
		ok(r.done && r.canceled, "reports the stop");
		ok(r.okCount >= 1 && r.okCount <= 2, "one or two clips got written before it took effect");
		ok(!QFile::exists(folder + "/Stop_03.mp4"), "the last one was never started");
		ok(!b.running(), "idle afterwards");
	}

	std::printf("\n-- text variations: one file per version --\n");
	{
		using namespace text_variations;
		TimelineModel whole;
		TlTrack words, footage;
		TlClip txt;
		txt.type = TlClip::Type::Text;
		txt.outStartMs = 0;
		txt.srcEndMs = 1500;
		txt.text.text = QStringLiteral("I like dogs");
		txt.text.fontPx = 220;
		txt.text.boxEnabled = true;
		txt.text.boxOpacity = 1.0;
		txt.text.random = true;
		txt.text.variations = {QStringLiteral("I like cats"), QStringLiteral(""),
				       QStringLiteral("WWWWWWWWWWWWWWWW")};
		words.clips = {txt};
		TlClip v;
		v.type = TlClip::Type::Video;
		v.sourceId = 1;
		v.srcStartMs = 0;
		v.srcEndMs = 1500;
		footage.clips = {v};
		whole.tracks = {words, footage};

		const QVector<Slot> vs = slotsOf(whole);
		const qint64 total = combinationCount(vs);
		ok(total == 3, "three versions (the blank line is not one)");
		QVector<BatchExporter::Item> items;
		for (qint64 k = 0; k < total; ++k) {
			const QVector<int> pick = combinationAt(vs, k);
			BatchExporter::Item it;
			it.name = fileName(QStringLiteral("Pets"), int(k + 1), int(total), textsAt(vs, pick));
			it.useTimeline = true;
			it.timeline = applyCombination(whole, vs, pick);
			items.push_back(it);
		}
		ClipExporter::Options tb = base;
		tb.canvasW = 640;
		tb.canvasH = 360;
		tb.timelineFps = 15.0;
		tb.timeline = whole;
		tb.timelineSources[1] = red.toStdString();

		const QString folder = batch_export::uniqueFolder(dir.path(), "Pets");
		BatchExporter b;
		const Run r = runBatch(b, red, tb, items, folder, false);
		ok(r.done && r.okCount == 3 && r.failCount == 0, "all three versions exported");
		const QString f1 = folder + "/Pets_01_I-like-dogs.mp4", f2 = folder + "/Pets_02_I-like-cats.mp4",
			      f3 = folder + "/Pets_03_WWWWWWWWWWWWWWWW.mp4";
		ok(QFile::exists(f1) && QFile::exists(f2) && QFile::exists(f3),
		   "named prefix + number + text");
		const double d1 = durationOf(f1), d3 = durationOf(f3);
		std::printf("     lengths %.2f %.2f s\n", d1, d3);
		ok(std::abs(d1 - 1.5) < 0.3 && std::abs(d3 - 1.5) < 0.3, "each the timeline's length");
		const QByteArray a = frameAt(f1, 0.7), c = frameAt(f2, 0.7), w = frameAt(f3, 0.7);
		const int ac = bytesApart(a, c), aw = bytesApart(a, w);
		std::printf("     bytes apart: dogs/cats %d, dogs/WWW %d (of %d)\n", ac, aw, int(a.size()));
		ok(ac > 0 && aw > ac, "the words differ between versions");
		ok(aw < a.size() / 2, "and the footage around them is the same");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
