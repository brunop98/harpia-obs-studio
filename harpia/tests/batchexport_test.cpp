// Batch export through the real exporter: three cuts to three files with the
// right lengths and names, a cut that cannot be exported is reported and the
// rest still written, and Cancel stops after the clip being written.
#include "editor/BatchExport.hpp"
#include "editor/ClipExporter.hpp"

#include <QCoreApplication>
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
	QCoreApplication app(argc, argv);
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

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
