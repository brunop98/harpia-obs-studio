// The export dialog, and whether its promises are true.
//
// The old dialog was a Save As box with a format combo. The new one makes
// claims -- this will be 1280x720, it will be about 18 MB, it will land in this
// folder -- and a claim is only worth making if the export honours it. So this
// checks the two halves that can drift apart:
//
//   1. What the dialog REPORTS matches what it will ASK FOR. A summary panel
//      computed from different numbers than the Options struct is a lie that
//      looks like a feature.
//   2. The size estimate is in the right ballpark and, more importantly, MOVES
//      the right way. An estimate that never changes when you halve the
//      resolution is worse than no estimate: it actively misinforms.
//
// The estimate is calibrated against a REAL encode from this build's encoder,
// not against a table, so changing the encoder or its preset shows up here
// rather than as a number that quietly stops matching.
//
// Built like exportsize_test (needs libav): ClipExporter + GifEncoder + the
// compositor, plus Qt Widgets for the dialog itself.
#include "editor/ExportEstimate.hpp"
#include "editor/ExportOptionsDialog.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QDir>
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

static ExportOptionsDialog::Context ctx1080()
{
	ExportOptionsDialog::Context c;
	c.defaultName = QStringLiteral("myclip");
	c.defaultFolder = QDir::tempPath();
	c.sourceSize = QSize(1920, 1080);
	c.fps = 30.0;
	c.seconds = 10.0;
	c.canKeepAudio = true;
	return c;
}

static QLabel *labelStartingWith(QWidget *w, const QString &prefix)
{
	for (QLabel *l : w->findChildren<QLabel *>())
		if (l->text().startsWith(prefix))
			return l;
	return nullptr;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	// A private settings store: the dialog remembers choices, and a test that
	// inherited the developer's real ones would pass or fail by accident.
	QCoreApplication::setOrganizationName(QStringLiteral("HarpiaTest"));
	QCoreApplication::setApplicationName(QStringLiteral("exportdialog_test"));
	QSettings().clear();

	std::printf("\n-- a fresh install exports at high quality --\n");
	{
		// The default used to be the middle of the slider on a live-capture
		// preset, which is a fine recording setting and a poor export one.
		ExportOptionsDialog dlg(ctx1080());
		dlg.show();
		QApplication::processEvents();
		std::printf("     CRF %d, effort %d, 4:4:4 %s\n", dlg.videoCrf(), int(dlg.effort()),
			    dlg.chroma444() ? "on" : "off");
		ok(dlg.videoCrf() <= 14, "the quality slider starts near the top of its range");
		ok(dlg.effort() == ClipExporter::Options::Effort::Best,
		   "and the encoder is allowed to take its time");
		// Not 4:4:4 by default, and that is deliberate: it is H.264 High 4:4:4
		// Predictive, which browsers and phones refuse to play. The best-looking
		// file nobody can open is not the best default.
		ok(!dlg.chroma444(), "but not 4:4:4, which many players cannot open");
	}

	std::printf("\n-- the estimate moves the way the settings do --\n");
	{
		ExportEstimateInput base;
		base.format = ClipExporter::Format::Mp4;
		base.width = 1920;
		base.height = 1080;
		base.fps = 30;
		base.seconds = 10;
		base.videoCrf = 23;
		base.keepAudio = true;
		const qint64 b = estimateExportBytes(base);
		std::printf("     1080p30 10s CRF23: %s\n", qPrintable(humanFileSize(b)));
		ok(b > 0, "a real duration gives a real number");

		ExportEstimateInput half = base;
		half.width = 960;
		half.height = 540;
		const qint64 h = estimateExportBytes(half);
		std::printf("     at 960x540: %s\n", qPrintable(humanFileSize(h)));
		// A quarter of the pixels. Not exactly a quarter of the bytes -- audio
		// and container overhead do not shrink -- but decisively smaller.
		ok(h < b / 2, "quartering the pixels at least halves the estimate");

		ExportEstimateInput slow = base;
		slow.fps = 15;
		ok(estimateExportBytes(slow) < b, "halving the frame rate shrinks it");

		ExportEstimateInput better = base;
		better.videoCrf = 17;
		std::printf("     at CRF 17: %s\n",
			    qPrintable(humanFileSize(estimateExportBytes(better))));
		ok(estimateExportBytes(better) > b, "asking for better quality grows it");

		ExportEstimateInput longer = base;
		longer.seconds = 20;
		ok(estimateExportBytes(longer) > b * 3 / 2, "twice as long is much bigger");

		ExportEstimateInput silent = base;
		silent.keepAudio = false;
		ok(estimateExportBytes(silent) < b, "dropping the audio shrinks it");

		ExportEstimateInput none = base;
		none.seconds = 0;
		ok(estimateExportBytes(none) == 0, "and nothing to encode estimates nothing");

		// GIF, where the palette is the dominant term.
		ExportEstimateInput g = base;
		g.format = ClipExporter::Format::Gif;
		g.fps = 15;
		g.gifColors = 256;
		const qint64 g256 = estimateExportBytes(g);
		g.gifColors = 8;
		const qint64 g8 = estimateExportBytes(g);
		std::printf("     GIF 256 colours %s, 8 colours %s\n", qPrintable(humanFileSize(g256)),
			    qPrintable(humanFileSize(g8)));
		ok(g8 < g256, "fewer GIF colours estimates smaller");
		g.gifColors = 256;
		g.gifDither = false;
		ok(estimateExportBytes(g) < g256, "and turning dithering off estimates smaller");
	}

	std::printf("\n-- and it lands between the easiest and hardest real encodes --\n");
	{
		// The part that keeps the model honest. An estimator can be perfectly
		// monotonic and still be off by 50x -- an early draft here multiplied by
		// 8 twice and nothing but a real encode caught it.
		//
		// "Within N% of one file" would be the wrong claim: at constant quality
		// the SAME settings give wildly different sizes depending on content.
		// So the bound is the range content can actually produce. Both ends are
		// encoded here at identical settings: a near-static test pattern, which
		// is about as compressible as video gets, and pure noise, which is about
		// as incompressible. A useful estimate for general content has to sit
		// between them, and well away from either edge.
		QTemporaryDir dir;
		const QString easy = dir.filePath(QStringLiteral("easy.mp4"));
		const QString hard = dir.filePath(QStringLiteral("hard.mp4"));
		auto enc = [&](const QStringList &pre, const QString &out) {
			QProcess ff;
			ff.start(QStringLiteral("ffmpeg"),
				 QStringList{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
					     QStringLiteral("error")}
					 + pre
					 + QStringList{QStringLiteral("-pix_fmt"),
						       QStringLiteral("yuv420p"),
						       QStringLiteral("-c:v"),
						       QStringLiteral("libx264"),
						       QStringLiteral("-crf"), QStringLiteral("23"),
						       QStringLiteral("-y"), out});
			return ff.waitForFinished(120000) && ff.exitCode() == 0;
		};
		const bool a = enc({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
				    QStringLiteral("testsrc=size=640x360:duration=4:rate=25")},
				   easy);
		const bool b = enc({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
				    QStringLiteral("nullsrc=s=640x360:d=4:r=25"),
				    QStringLiteral("-vf"),
				    QStringLiteral("noise=alls=100:allf=t")},
				   hard);
		ok(a && b, "encoded the same 4s at 640x360 CRF23 twice: near-static, and noise");
		if (a && b) {
			const qint64 lo = QFileInfo(easy).size();
			const qint64 hi = QFileInfo(hard).size();
			ExportEstimateInput e;
			e.format = ClipExporter::Format::Mp4;
			e.width = 640;
			e.height = 360;
			e.fps = 25;
			e.seconds = 4;
			e.videoCrf = 23;
			e.keepAudio = false;
			const qint64 est = estimateExportBytes(e);
			std::printf("     easiest %s, estimate %s, hardest %s\n",
				    qPrintable(humanFileSize(lo)), qPrintable(humanFileSize(est)),
				    qPrintable(humanFileSize(hi)));
			ok(lo < hi / 10, "the two ends really are far apart, so this is a real bound");
			ok(est > lo && est < hi, "the estimate is inside the range content can produce");
			// And not hugging an edge: an estimate pinned to the easy end would
			// tell everyone their export is tiny.
			ok(est > lo * 4, "comfortably above the trivially-compressible end");
			ok(est < hi / 4, "and comfortably below pure noise");
		}
	}

	std::printf("\n-- what the dialog says is what it will ask for --\n");
	{
		ExportOptionsDialog dlg(ctx1080());
		dlg.show();
		QApplication::processEvents();

		// Default: original size, so no override is sent at all.
		ok(!dlg.outputSize().isValid(), "\"Original\" asks the exporter for no override");

		QComboBox *res = nullptr;
		for (QComboBox *c : dlg.findChildren<QComboBox *>())
			if (c->count() && c->itemText(0).startsWith(QStringLiteral("Original")))
				res = c;
		ok(res != nullptr, "there is a resolution combo built from the source size");
		if (!res) {
			std::printf("\nFAILURES\n");
			return 1;
		}
		std::printf("     presets:");
		for (int i = 0; i < res->count(); ++i)
			std::printf(" [%s]", qPrintable(res->itemText(i)));
		std::printf("\n");
		// Nothing above the source: an "upscale to 4K" that the exporter would
		// honour by blurring the picture is not a useful offer.
		bool anyUpscale = false;
		for (int i = 0; i < res->count(); ++i)
			anyUpscale = anyUpscale || res->itemData(i).toInt() > 1080;
		ok(!anyUpscale, "no preset is taller than the source");

		const int i720 = res->findData(720);
		ok(i720 >= 0, "720p is offered for a 1080p source");
		res->setCurrentIndex(i720);
		QApplication::processEvents();
		std::printf("     720p -> %dx%d\n", dlg.outputSize().width(),
			    dlg.outputSize().height());
		ok(dlg.outputSize() == QSize(1280, 720), "and it means 1280x720, keeping the aspect");

		// The summary must agree with the getter the window will actually read.
		QLabel *sum = labelStartingWith(&dlg, QStringLiteral("Format:"));
		ok(sum != nullptr, "the summary panel exists");
		if (sum) {
			std::printf("     summary:\n%s\n", qPrintable(sum->text()));
			ok(sum->text().contains(QStringLiteral("1280×720")),
			   "and it reports the size the exporter will be given");
			ok(sum->text().contains(QStringLiteral("10.0 s")), "the real duration");
		}

		// Even sizes only: yuv420p rejects an odd dimension outright.
		const int iCustom = res->findData(-1);
		res->setCurrentIndex(iCustom);
		QApplication::processEvents();
		QList<QSpinBox *> spins;
		for (QSpinBox *sp : dlg.findChildren<QSpinBox *>())
			if (sp->maximum() > 1000)
				spins << sp;
		ok(spins.size() >= 2, "Custom reveals width and height boxes");
		if (spins.size() >= 2) {
			spins[0]->setValue(641);
			spins[1]->setValue(361);
			QApplication::processEvents();
			std::printf("     custom 641x361 -> %dx%d\n", dlg.outputSize().width(),
				    dlg.outputSize().height());
			ok(dlg.outputSize() == QSize(640, 360),
			   "an odd custom size is made even before it reaches the encoder");
		}
	}

	std::printf("\n-- the estimate on screen tracks the controls --\n");
	{
		ExportOptionsDialog dlg(ctx1080());
		dlg.show();
		QApplication::processEvents();
		const qint64 atOriginal = dlg.estimatedBytes();
		QLabel *shown = labelStartingWith(&dlg, QStringLiteral("≈"));
		ok(shown != nullptr, "the estimate is on screen");
		if (shown)
			std::printf("     shows %s\n", qPrintable(shown->text()));

		QComboBox *res = nullptr;
		for (QComboBox *c : dlg.findChildren<QComboBox *>())
			if (c->count() && c->itemText(0).startsWith(QStringLiteral("Original")))
				res = c;
		if (res) {
			res->setCurrentIndex(res->findData(480));
			QApplication::processEvents();
			std::printf("     at 480p: %s\n",
				    qPrintable(humanFileSize(dlg.estimatedBytes())));
			// This is the check that a live estimate is actually live. A label
			// computed once in the constructor passes every other assertion
			// here and fails this one.
			ok(dlg.estimatedBytes() < atOriginal / 2,
			   "choosing 480p immediately shrinks the number on screen");
			if (shown)
				ok(shown->text() != QStringLiteral("≈ —"),
				   "and the label is a size, not a dash");
		}
	}

	std::printf("\n-- the button says what it will do --\n");
	{
		ExportOptionsDialog dlg(ctx1080());
		dlg.show();
		QApplication::processEvents();
		QPushButton *exp = nullptr;
		for (QPushButton *b : dlg.findChildren<QPushButton *>())
			if (b->text().startsWith(QStringLiteral("Export")))
				exp = b;
		ok(exp != nullptr, "there is an export button");
		if (exp) {
			std::printf("     button says \"%s\"\n", qPrintable(exp->text()));
			// "Export" alone made you scroll up to remember which format was
			// selected.
			ok(exp->text() != QStringLiteral("Export"),
			   "and it names the format rather than saying just \"Export\"");
			QComboBox *fmt = nullptr;
			for (QComboBox *c : dlg.findChildren<QComboBox *>())
				if (c->count() && c->itemText(0).contains(QStringLiteral("GIF")))
					fmt = c;
			if (fmt) {
				ok(exp->text().contains(QStringLiteral("GIF")),
				   "GIF is first, so it starts as \"Export GIF\"");
				fmt->setCurrentIndex(fmt->findData(int(ClipExporter::Format::Mp4)));
				QApplication::processEvents();
				std::printf("     after switching format: \"%s\"\n",
					    qPrintable(exp->text()));
				ok(exp->text().contains(QStringLiteral("MP4")),
				   "and follows the format combo");
			}
		}
	}

	std::printf("\n-- the output path is folder + name + the right extension --\n");
	{
		ExportOptionsDialog::Context c = ctx1080();
		c.defaultFolder = QStringLiteral("/tmp/exports");
		ExportOptionsDialog dlg(c);
		dlg.show();
		QApplication::processEvents();
		QComboBox *fmt = nullptr;
		for (QComboBox *cb : dlg.findChildren<QComboBox *>())
			if (cb->count() && cb->itemText(0).contains(QStringLiteral("GIF")))
				fmt = cb;
		if (fmt)
			fmt->setCurrentIndex(fmt->findData(int(ClipExporter::Format::Mkv)));
		QApplication::processEvents();
		std::printf("     %s\n", qPrintable(dlg.outputPath()));
		ok(dlg.outputPath().endsWith(QStringLiteral("/exports/myclip.mkv")),
		   "the path follows the folder, the name and the format");
	}

	std::printf("\n-- settings are remembered between exports --\n");
	{
		// Re-picking 720p and 15 fps on every single export was most of the
		// tedium; this is the check that it stops.
		{
			ExportOptionsDialog dlg(ctx1080());
			dlg.show();
			QApplication::processEvents();
			for (QComboBox *c : dlg.findChildren<QComboBox *>())
				if (c->count() && c->itemText(0).startsWith(QStringLiteral("Original")))
					c->setCurrentIndex(c->findData(720));
			for (QSlider *sl : dlg.findChildren<QSlider *>())
				sl->setValue(100); // best quality end
			QApplication::processEvents();
			dlg.accept(); // saving happens on accept, not on close
			QApplication::processEvents();
		}
		ExportOptionsDialog again(ctx1080());
		again.show();
		QApplication::processEvents();
		std::printf("     reopened at %dx%d, CRF %d\n", again.outputSize().width(),
			    again.outputSize().height(), again.videoCrf());
		ok(again.outputSize() == QSize(1280, 720), "it reopened at the resolution last used");
		ok(again.videoCrf() == 0, "and at the quality last used (the slider's top is lossless)");

		// Remembered as a HEIGHT, not an index: the preset list depends on the
		// source, so an index means a different row for the next clip.
		ExportOptionsDialog::Context small = ctx1080();
		small.sourceSize = QSize(1280, 720); // 720p is no longer offered
		ExportOptionsDialog other(small);
		other.show();
		QApplication::processEvents();
		std::printf("     for a 720p source, reopened at %s\n",
			    other.outputSize().isValid()
				    ? qPrintable(QStringLiteral("%1x%2")
							 .arg(other.outputSize().width())
							 .arg(other.outputSize().height()))
				    : "Original");
		ok(!other.outputSize().isValid(),
		   "a source that cannot do the remembered size falls back to Original");
	}

	QSettings().clear();
	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
