// A Random clip in the real editor window, from making one to exporting its
// versions through the real Export dialog.
//
//   * A clip made random shows a Random clip section in the Inspector.
//   * "+ Add clip" lists the project's media; adding one shows it at once, in
//     the slot's place and for its length.
//   * The arrows step through them; × removes one.
//   * Export > Variations writes one video per clip, each showing its OWN
//     footage (the covers differ: red against blue), named after the files.
//   * Saved and reopened, the clips come back, pointing at the right files.
#include "editor/ExportOptionsDialog.hpp"
#include "editor/InspectorSection.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/ClipVariations.hpp"
#include "editor/timeline/TextVariations.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>

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

static void settle(int ms)
{
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < ms) {
		QApplication::processEvents();
		// deleteLater, as the application's own loop would run it.
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QThread::msleep(5);
	}
}

static void click(QWidget *w, QPoint p)
{
	QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), w->mapToGlobal(QPointF(p)), Qt::LeftButton,
			  Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(w, &press);
	QMouseEvent release(QEvent::MouseButtonRelease, QPointF(p), w->mapToGlobal(QPointF(p)), Qt::LeftButton,
			    Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(w, &release);
}

// The mean luma of the cover picture inside `path`; -1 if it has none.
static double coverLuma(const QString &path)
{
	double out = -1;
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0)
		return out;
	avformat_find_stream_info(fmt, nullptr);
	for (unsigned i = 0; i < fmt->nb_streams; ++i) {
		const AVStream *st = fmt->streams[i];
		if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC) || st->attached_pic.size <= 0)
			continue;
		const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
		AVCodecContext *d = avcodec_alloc_context3(c);
		AVFrame *f = av_frame_alloc();
		if (avcodec_open2(d, c, nullptr) >= 0 && avcodec_send_packet(d, &st->attached_pic) >= 0 &&
		    avcodec_receive_frame(d, f) >= 0) {
			double sum = 0;
			for (int y = 0; y < f->height; ++y)
				for (int x = 0; x < f->width; ++x)
					sum += f->data[0][y * f->linesize[0] + x];
			out = sum / (double(f->width) * f->height);
		}
		av_frame_free(&f);
		avcodec_free_context(&d);
	}
	avformat_close_input(&fmt);
	return out;
}

static TlClip randomClipOf(const TimelineView *tv)
{
	for (const TlTrack &t : tv->model().tracks)
		for (const TlClip &c : t.clips)
			if (c.isRandomClip())
				return c;
	return TlClip();
}

static QAction *menuAction(QPushButton *b, const QString &startsWith)
{
	QMenu *m = b->menu();
	if (!m)
		return nullptr;
	emit m->aboutToShow(); // fills it, as opening it does
	for (QAction *a : m->actions())
		if (a->text().startsWith(startsWith))
			return a;
	return nullptr;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	const QString media = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
	const QString red = media + QStringLiteral("/av_red.mp4");
	const QString blue = media + QStringLiteral("/av_blue.mp4");
	const QString green = media + QStringLiteral("/av_green.mp4");
	for (const QString &f : {red, blue, green})
		if (!QFile::exists(f)) {
			std::printf("missing %s\n", qPrintable(f));
			return 2;
		}
	QSettings().setValue(QStringLiteral("export/format"), int(ClipExporter::Format::Mp4));
	QTemporaryDir tmp;

	VideoEditorWindow w(red);
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

	// Blue and green into the project (dropped on the timeline, as a user
	// would), then a timeline of one 2 s slot of red, made random.
	emit tv->filesDropped({blue, green}, -1, -1, 0);
	settle(800);
	int redId = -1, blueId = -1, greenId = -1, lane = -1;
	for (int t = 0; t < tv->model().tracks.size(); ++t)
		for (const TlClip &c : tv->model().tracks[t].clips) {
			const QString p = w.sourcePathForTest(c.sourceId);
			if (p.endsWith(QStringLiteral("av_red.mp4"))) {
				redId = c.sourceId;
				lane = t;
			} else if (p.endsWith(QStringLiteral("av_blue.mp4")))
				blueId = c.sourceId;
			else if (p.endsWith(QStringLiteral("av_green.mp4")))
				greenId = c.sourceId;
		}
	ok(redId >= 0 && blueId >= 0 && greenId >= 0 && lane >= 0, "three files in the project");
	if (redId < 0 || blueId < 0 || greenId < 0 || lane < 0)
		return 1;
	{
		TimelineModel m = tv->model();
		TlClip slot;
		for (const TlClip &c : m.tracks[lane].clips)
			if (c.sourceId == redId)
				slot = c;
		slot.srcStartMs = 1000;
		slot.srcEndMs = 3000;
		slot.outStartMs = 0;
		clip_variations::makeRandom(slot); // what Make random clip does
		TlTrack only = m.tracks[lane];
		only.clips = {slot};
		m.tracks = {only};
		tv->setModelAndCommit(m);
		settle(300);
	}

	std::printf("\n-- the Inspector --\n");
	click(tv, tv->clipRectForTest(0, 0).center());
	settle(300);
	QPushButton *add = w.findChild<QPushButton *>(QStringLiteral("addRandomClipButton"));
	QWidget *section = add;
	while (section && !dynamic_cast<InspectorSection *>(section))
		section = section->parentWidget();
	ok(add && section && !section->isHidden(), "a Random clip section, with + Add clip");
	if (!add)
		return 1;
	QAction *addBlue = menuAction(add, QStringLiteral("av_blue"));
	ok(addBlue && addBlue->isEnabled(), "+ Add clip lists the project's media");
	if (addBlue)
		addBlue->trigger();
	settle(300);
	TlClip c = randomClipOf(tv);
	ok(c.clipOptions.size() == 2 && c.clipShown == 1 && c.sourceId == blueId,
	   "blue is added, and shown at once");
	ok(c.outStartMs == 0 && c.srcLenMs() == 2000, "in the slot's place, for its length");
	if (QAction *a = menuAction(add, QStringLiteral("av_green")))
		a->trigger();
	settle(300);
	c = randomClipOf(tv);
	ok(c.clipOptions.size() == 3 && c.sourceId == greenId, "then green");
	// Counted, not checked for visibility: the Clip tab need not be on screen
	// in a test window, and rows made under a hidden tab show with it.
	const int rows = int(w.findChildren<QToolButton *>(QStringLiteral("randomClipVideo")).size());
	ok(rows == 3, "three rows in the list");

	QPushButton *prev = nullptr;
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->toolTip() == QStringLiteral("Show the previous clip"))
			prev = b;
	ok(prev != nullptr, "the arrows");
	if (prev)
		prev->click();
	settle(300);
	ok(randomClipOf(tv).sourceId == blueId && randomClipOf(tv).clipShown == 1, "back one: blue again");

	QVector<QPushButton *> removes;
	for (QPushButton *b : w.findChildren<QPushButton *>())
		if (b->toolTip() == QStringLiteral("Remove this clip"))
			removes << b;
	ok(removes.size() == 3, "each row has its ×");
	if (removes.size() == 3)
		removes[2]->click(); // green
	settle(300);
	c = randomClipOf(tv);
	ok(c.clipOptions.size() == 2 && c.clipOptionAt(0).sourceId == redId && c.clipOptionAt(1).sourceId == blueId,
	   "green removed: red and blue are left");
	const QVector<text_variations::Slot> vs = text_variations::slotsOf(tv->model());
	ok(text_variations::combinationCount(vs) == 2, "two versions");

	std::printf("\n-- saved and reopened --\n");
	{
		const QString proj = tmp.filePath(QStringLiteral("random.harpiaproj"));
		w.saveProjectTo(proj, true);
		VideoEditorWindow w2{QString()};
		ok(w2.openProjectAt(proj), "the project opens");
		settle(500);
		TimelineView *tv2 = w2.findChild<TimelineView *>();
		const TlClip back = tv2 ? randomClipOf(tv2) : TlClip();
		ok(back.clipOptions.size() == 2, "with both clips");
		ok(w2.sourcePathForTest(back.clipOptionAt(0).sourceId).endsWith(QStringLiteral("av_red.mp4")) &&
			   w2.sourcePathForTest(back.clipOptionAt(1).sourceId).endsWith(QStringLiteral("av_blue.mp4")),
		   "each pointing at its own file");
		ok(back.clipShown == c.clipShown && back.srcLenMs() == 2000, "showing the same one, the same length");
	}

	std::printf("\n-- Export > Variations --\n");
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
						if (bb->buttonRole(b) == QDialogButtonBox::AcceptRole)
							b->click();
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
	exportBtn->click();
	QElapsedTimer t;
	t.start();
	while (lastMessage.isEmpty() && t.elapsed() < 240000)
		settle(50);
	driver.stop();
	ok(sawDialog, "the export dialog opened");
	ok(lastMessage.contains(QStringLiteral("2 clips exported")) || lastMessage.contains(QStringLiteral("2 of 2")),
	   "both versions written");
	int files = 0;
	for (const QString &d : QDir(media).entryList({QStringLiteral("*")}, QDir::Dirs | QDir::NoDotAndDotDot,
						    QDir::Time)) {
		const QString dir = media + QLatin1Char('/') + d;
		const QStringList mp4 = QDir(dir).entryList({QStringLiteral("*.mp4")}, QDir::Files, QDir::Name);
		const QString all = mp4.join(QLatin1Char(' '));
		if (mp4.size() != 2 || !all.contains(QStringLiteral("av_blue")))
			continue;
		files = mp4.size();
		std::printf("     %s: %s\n", qPrintable(d), qPrintable(all));
		ok(mp4[0].contains(QStringLiteral("av_red-@-1.0s")) && mp4[1].contains(QStringLiteral("av_blue-@-0.0s")),
		   "named after each clip and its moment");
		const double l0 = coverLuma(dir + QLatin1Char('/') + mp4[0]);
		const double l1 = coverLuma(dir + QLatin1Char('/') + mp4[1]);
		std::printf("     cover luma: %.1f (red), %.1f (blue)\n", l0, l1);
		ok(l0 > 0 && l1 > 0 && l0 - l1 > 15, "each shows its own footage: red is brighter than blue");
		QDir(dir).removeRecursively();
		break;
	}
	ok(files == 2, "two files");

	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
