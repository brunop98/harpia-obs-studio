// Exporting a Random text's versions from the real editor window, the way the
// user does it: Export… → the Variations tab → Export N videos, through the
// real dialog, the real batch runner and the real "done" message.
//
// A Random text with rich-text tags over the footage; every version must be
// written, and the editor must still be alive and usable afterwards.
//
// Each file carries its own cover picture (editor/CoverArt), what Windows
// shows as its thumbnail: a frame of THAT version, so the three differ --
// and survive the audio mix, which rewrites the file after the picture.
#include "editor/ExportOptionsDialog.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TextVariations.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// The cover inside `path`, decoded: its luma, row by row. Empty if none.
// `bestIsVideo`: the file's main video is still the H.264, not the picture.
static QByteArray coverLuma(const QString &path, QSize &size, bool &bestIsVideo)
{
	QByteArray out;
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0)
		return out;
	avformat_find_stream_info(fmt, nullptr);
	const int best = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	bestIsVideo = best >= 0 && fmt->streams[best]->codecpar->codec_id == AV_CODEC_ID_H264;
	for (unsigned i = 0; i < fmt->nb_streams; ++i) {
		const AVStream *st = fmt->streams[i];
		if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC) || st->attached_pic.size <= 0)
			continue;
		const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
		AVCodecContext *d = avcodec_alloc_context3(c);
		AVFrame *f = av_frame_alloc();
		if (avcodec_open2(d, c, nullptr) >= 0 && avcodec_send_packet(d, &st->attached_pic) >= 0 &&
		    avcodec_receive_frame(d, f) >= 0) {
			size = QSize(f->width, f->height);
			for (int y = 0; y < f->height; ++y)
				out.append(reinterpret_cast<const char *>(f->data[0] + y * f->linesize[0]), f->width);
		}
		av_frame_free(&f);
		avcodec_free_context(&d);
	}
	avformat_close_input(&fmt);
	return out;
}

static double meanDiff(const QByteArray &a, const QByteArray &b)
{
	if (a.size() != b.size() || a.isEmpty())
		return 0.0;
	double sum = 0.0;
	for (int i = 0; i < a.size(); ++i)
		sum += std::abs(int(quint8(a[i])) - int(quint8(b[i])));
	return sum / a.size();
}

static void settle(int ms)
{
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < ms) {
		QApplication::processEvents();
		QThread::msleep(5);
	}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	const QString media = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
	const QString video = media + QStringLiteral("/av_red.mp4");
	if (!QFile::exists(video)) {
		std::printf("missing %s\n", qPrintable(video));
		return 2;
	}
	// MP4, whatever an earlier run left chosen (the dialog remembers it).
	QSettings().setValue(QStringLiteral("export/format"), int(ClipExporter::Format::Mp4));
	VideoEditorWindow w(video);
	w.resize(1400, 1000);
	w.show();
	settle(300);
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->text() == QStringLiteral("Full Editing")) {
			b->click();
			settle(300);
			break;
		}
	TimelineView *tv = w.findChild<TimelineView *>();
	ok(tv != nullptr, "in Full editing");
	if (!tv)
		return 1;

	// The footage as it came in, plus a Random text over it with tags.
	TimelineModel m = tv->model();
	TlTrack words;
	TlClip c;
	c.type = TlClip::Type::Text;
	c.srcEndMs = 4000; // the whole clip: the cover (its middle frame) shows the text
	c.text.text = QStringLiteral("I like <color=yellow>dogs</color>");
	c.text.random = true;
	c.text.variations = {QStringLiteral("I like <b>cats</b>"), QStringLiteral("I like\n<i>vultures</i>")};
	words.clips = {c};
	m.tracks.prepend(words);
	tv->setModelAndCommit(m);
	settle(300);
	const int versions = int(text_variations::combinationCount(text_variations::slotsOf(tv->model())));
	ok(versions == 3, "three versions");

	// Drive the modal dialogs as they open.
	QString lastMessage;
	bool sawDialog = false;
	QTimer driver;
	driver.setInterval(50);
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		for (QWidget *top : QApplication::topLevelWidgets()) {
			if (!top->isVisible())
				continue;
			if (auto *dlg = qobject_cast<ExportOptionsDialog *>(top)) {
				if (sawDialog)
					continue;
				sawDialog = true;
				dlg->setBatchTabForTest(true);
				for (QDialogButtonBox *bb : dlg->findChildren<QDialogButtonBox *>())
					for (QAbstractButton *b : bb->buttons())
						if (bb->buttonRole(b) == QDialogButtonBox::AcceptRole) {
							std::printf("     dialog button: %s\n", qPrintable(b->text()));
							b->click();
						}
				return;
			}
			if (auto *mb = qobject_cast<QMessageBox *>(top)) {
				lastMessage = mb->text();
				std::printf("     message: %s\n", qPrintable(lastMessage.simplified()));
				mb->accept();
				return;
			}
		}
	});
	driver.start();

	QPushButton *exportBtn = nullptr;
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->text() == QStringLiteral("Export…"))
			exportBtn = b;
	ok(exportBtn != nullptr, "the Export… button");
	if (!exportBtn)
		return 1;
	exportBtn->click(); // runs the dialog's own loop; the driver clicks through it

	// Wait for the batch to finish and its message to come and go.
	QElapsedTimer t;
	t.start();
	while (lastMessage.isEmpty() && t.elapsed() < 240000)
		settle(50);
	ok(sawDialog, "the export dialog opened");
	ok(lastMessage.contains(QStringLiteral("3 clips exported")) || lastMessage.contains(QStringLiteral("3 of 3")),
	   "all three versions written");
	const QStringList dirs = QDir(media).entryList({QStringLiteral("*")}, QDir::Dirs | QDir::NoDotAndDotDot,
						    QDir::Time);
	int files = 0;
	for (const QString &d : dirs) {
		const QStringList mp4 = QDir(media + QLatin1Char('/') + d).entryList({QStringLiteral("*.mp4")});
		if (mp4.size() == 3 && mp4.join(QLatin1Char(' ')).contains(QStringLiteral("I-like-cats"))) {
			files = mp4.size();
			std::printf("     %s: %s\n", qPrintable(d), qPrintable(mp4.join(QStringLiteral(", "))));
			QVector<QByteArray> covers;
			bool allVideo = true;
			QSize size;
			for (const QString &f : mp4) {
				bool video = false;
				covers.push_back(coverLuma(media + QLatin1Char('/') + d + QLatin1Char('/') + f, size, video));
				allVideo = allVideo && video;
			}
			ok(!covers[0].isEmpty() && !covers[1].isEmpty() && !covers[2].isEmpty(),
			   "each file has a cover picture (after the audio mix)");
			std::printf("     cover %d x %d\n", size.width(), size.height());
			ok(allVideo, "and its video is still the file's main video");
			const double d01 = meanDiff(covers[0], covers[1]), d02 = meanDiff(covers[0], covers[2]),
				     d12 = meanDiff(covers[1], covers[2]);
			std::printf("     cover differences %.2f %.2f %.2f\n", d01, d02, d12);
			ok(d01 > 0.3 && d02 > 0.3 && d12 > 0.3, "the three covers differ: each shows its own text");
			QDir(media + QLatin1Char('/') + d).removeRecursively();
			break;
		}
	}
	ok(files == 3, "three files, named after their texts");

	// Still alive and usable: select, edit, preview.
	tv->setSelectionState(TlSelection());
	settle(100);
	ok(tv->model().tracks.size() >= 2, "the editor is still there afterwards");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
