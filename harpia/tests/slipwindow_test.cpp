// The Slip window: one clip, its own preview, and its source's strip under a
// fixed window -- in the real editor, from the clip's menu to the timeline.
//
//   * The preview shows the clip's moment and nothing else: its look, but not
//     its pose or its fades, and never the rest of the timeline (a caption
//     over it on the timeline is not in it).
//   * Dragging the strip slips the footage: drag left, later footage. It
//     stops at either end of the source, and says so.
//   * The arrow keys nudge it a frame (Shift: a second), Home and End go to
//     either end, the wheel a tenth of the clip; Reset puts it back.
//   * Play loops the moment, picture from the clip's own decoder, sound mixed
//     with its detached audio.
//   * OK applies the slip as one undo step; Cancel and Esc leave the clip.
//
// The source numbers its own frames (eight black or white bands, one per
// bit), so "the preview shows frame 60" is something the test can read.
#include "editor/AudioPreview.hpp"
#include "editor/FrameSeeker.hpp"
#include "editor/SlipDialog.hpp"
#include "editor/TimelineAudio.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>

#include <atomic>
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

static void settle(int ms)
{
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < ms) {
		QApplication::processEvents();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QThread::msleep(5);
	}
}

static bool makeSource(const QString &path)
{
	QProcess ff;
	ff.start(QStringLiteral("ffmpeg"),
		 {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
		  QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
		  QStringLiteral("nullsrc=s=64x64:r=60:d=4,format=yuv420p,"
				 "geq=lum='16+219*mod(floor(N/pow(2\\,floor(X/8)))\\,2)':cb=128:cr=128"),
		  QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
		  QStringLiteral("sine=frequency=440:sample_rate=48000:duration=4"), QStringLiteral("-c:v"),
		  QStringLiteral("libx264"), QStringLiteral("-g"), QStringLiteral("24"), QStringLiteral("-crf"),
		  QStringLiteral("8"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), QStringLiteral("-c:a"),
		  QStringLiteral("aac"), QStringLiteral("-shortest"), QStringLiteral("-y"), path});
	ff.waitForFinished(60000);
	return ff.exitCode() == 0 && QFileInfo::exists(path);
}

// Ten minutes of real seekable media without encoding 36,000 frames/audio:
// one numbered frame per second is enough to verify late absolute seeks.
static bool makeLongSource(const QString &path)
{
	QProcess ff;
	ff.start(QStringLiteral("ffmpeg"),
		{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
		 QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
		 QStringLiteral("nullsrc=s=64x64:r=1:d=600,format=yuv420p,"
			"geq=lum='16+219*mod(floor(N/pow(2\\,floor(X/8)))\\,2)':cb=128:cr=128"),
		 QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-g"), QStringLiteral("24"),
		 QStringLiteral("-crf"), QStringLiteral("8"), QStringLiteral("-y"), path});
	return ff.waitForFinished(30000) && ff.exitStatus() == QProcess::NormalExit && ff.exitCode() == 0;
}

// The frame number the picture shows; -1 for none. Read across the whole
// width, so it works at any size the preview is drawn.
static int counterOf(const QImage &img)
{
	if (img.isNull() || img.width() < 16)
		return -1;
	const QImage rgb = img.convertToFormat(QImage::Format_RGB32);
	int n = 0;
	for (int k = 0; k < 8; ++k)
		if (qGray(rgb.pixel(int((8 * k + 4) / 64.0 * rgb.width()), rgb.height() / 2)) > 128)
			n |= 1 << k;
	return n;
}

// The frame on screen at `ms` of the 60 fps source.
static int frameAt(qint64 ms)
{
	return int(std::floor(double(ms + 1) * 60.0 / 1000.0));
}

static void mouse(QWidget *w, QEvent::Type type, QPoint p, Qt::MouseButtons held)
{
	QMouseEvent e(type, QPointF(p), w->mapToGlobal(QPointF(p)),
		      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held, Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}

static void dragStrip(QWidget *bar, int dx)
{
	const QPoint c = bar->rect().center();
	mouse(bar, QEvent::MouseButtonPress, c, Qt::LeftButton);
	mouse(bar, QEvent::MouseMove, c + QPoint(dx / 2, 0), Qt::LeftButton);
	mouse(bar, QEvent::MouseMove, c + QPoint(dx, 0), Qt::LeftButton);
	mouse(bar, QEvent::MouseButtonRelease, c + QPoint(dx, 0), Qt::NoButton);
}

static void key(QWidget *w, int k, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
	QKeyEvent e(QEvent::KeyPress, k, mods);
	QApplication::sendEvent(w, &e);
}

static TlClip media(int source, qint64 out, qint64 from, qint64 to)
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
	QApplication app(argc, argv);
	{
		SlipTrackBar overview;
		overview.setOverview(true);
		overview.resize(612, 44);
		overview.setSource({}, {}, 600000); // ten-minute source, one-second clip
		overview.setWindow(1000, 1000, -1000, 598000);
		mouse(&overview, QEvent::MouseButtonPress, QPoint(456, 25), Qt::LeftButton);
		mouse(&overview, QEvent::MouseButtonRelease, QPoint(456, 25), Qt::NoButton);
		ok(overview.delta() == 449000, "overview jumps straight to 7:30 in a ten-minute video");
		mouse(&overview, QEvent::MouseButtonPress, QPoint(6, 25), Qt::LeftButton);
		ok(overview.delta() == -1000, "overview reaches the beginning without scrolling");
		mouse(&overview, QEvent::MouseButtonRelease, QPoint(900, 25), Qt::NoButton);
		ok(overview.delta() == 598000, "overview clamps the final source window inside the video even on an outside release");
	}
	QTemporaryDir dir;
	const QString src = dir.filePath(QStringLiteral("count.mp4"));
	ok(makeSource(src), "made a 4 s, 60 fps source that numbers its frames, with sound");
	{
		const QString longSource = dir.filePath(QStringLiteral("ten-minutes.mp4"));
		ok(makeLongSource(longSource), "made a real ten-minute source for overview seeking");
		SlipDialog::Input input;
		input.clip = media(1, 5000, 1000, 2000);
		input.title = QStringLiteral("Ten-minute overview verification");
		input.decodePath = longSource;
		input.sourceSize = QSize(64, 64);
		input.sourceMs = 600000;
		input.loMs = -1000;
		input.hiMs = 598000;
		FrameSeeker thumbnails;
		ok(thumbnails.open(longSource), "ten-minute source opens for cached overview thumbnails");
		for (int second = 0; second < 600; second += 60)
			input.thumbs.append(thumbnails.frameAt(second * 1000, 64, 36));
		SlipDialog dialog(input);
		dialog.resize(900, 640);
		dialog.show();
		settle(50);
		SlipTrackBar *overview = dialog.overviewBar();
		const QPoint late(int(std::lround(6 + 0.75 * (overview->width() - 12))), 25);
		QElapsedTimer seekTimer;
		seekTimer.start();
		mouse(overview, QEvent::MouseButtonPress, late, Qt::LeftButton);
		mouse(overview, QEvent::MouseButtonRelease, late, Qt::NoButton);
		const qint64 start = dialog.momentModel(false).tracks[0].clips[0].srcStartMs;
		std::printf("     overview seek to %.3f s took %lld ms\n", start / 1000.0, (long long)seekTimer.elapsed());
		ok(std::llabs(start - 450000) < 1000 && counterOf(dialog.previewImage()) == int(start / 1000) % 256,
		   "overview jumps near 7:30 and decodes the actual late source frame");
		dialog.setDeltaMs(449000);
		ok(counterOf(dialog.previewImage()) == 450 % 256 && overview->delta() == 449000,
		   "precise timing at 7:30 updates the decoded frame and overview together");
		// Optional manual-review artifact, from the actual production dialog.
		if (argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--snapshot"))
			ok(dialog.grab().save(QString::fromLocal8Bit(argv[2])), "saved the real Slip overview screenshot");
		mouse(overview, QEvent::MouseButtonPress, late, Qt::LeftButton);
		mouse(overview, QEvent::MouseButtonRelease, QPoint(overview->width() + 40, 25), Qt::NoButton);
		ok(dialog.deltaMs() == 598000 && counterOf(dialog.previewImage()) == 599 % 256,
		   "the overview reaches the final complete moment of real ten-minute media");
		dialog.reject();
		ok(dialog.result() == QDialog::Rejected && input.clip.srcStartMs == 1000,
		   "cancelling long-source navigation leaves the input clip unchanged");
	}
	{
		SlipDialog::Input input;
		input.soundOnly = true;
		input.clip = media(1, 9000, 1000, 2000);
		input.clip.speed = 2.0;
		input.clip.peaks = QVector<float>(100, 0.6f);
		input.sourceMs = 600000;
		input.loMs = -250; // narrower than the video, e.g. linked sound bounds
		input.hiMs = 1250;
		SlipDialog dialog(input);
		dialog.show();
		settle(20);
		auto *overview = dialog.overviewBar();
		mouse(overview, QEvent::MouseButtonPress, QPoint(overview->width() - 6, 25), Qt::LeftButton);
		mouse(overview, QEvent::MouseButtonRelease, QPoint(overview->width() - 6, 25), Qt::NoButton);
		const TlClip moment = dialog.momentModel(false).tracks[0].clips[0];
		ok(dialog.deltaMs() == 1250 && moment.srcStartMs == 2250 && moment.srcLenMs() == 1000 && moment.outDurationMs() == 500,
		   "coarse audio seeking respects partner limits and retains source length and speed");
		mouse(overview, QEvent::MouseButtonPress, QPoint(6, 25), Qt::LeftButton);
		mouse(overview, QEvent::MouseButtonRelease, QPoint(6, 25), Qt::NoButton);
		ok(dialog.deltaMs() == -250 && dialog.trackBar()->delta() == -250 && !dialog.previewImage().isNull(),
		   "waveform overview clamps at the partner's earliest valid source time");
		dialog.reject();
	}

	std::printf("\n-- the window on its own --\n");
	{
		SlipDialog::Input in;
		// On the timeline it sits small, turned, in a corner, fading in: none
		// of that is the moment.
		in.clip = media(1, 5000, 1000, 2000);
		in.clip.posX = 0.15;
		in.clip.posY = 0.85;
		in.clip.scale = 0.3;
		in.clip.rotation = 30;
		in.clip.fadeInMs = 800;
		TlClip sound = media(1, 5000, 1000, 2000); // its detached sound
		in.partners = {sound};
		in.title = QStringLiteral("count.mp4");
		in.decodePath = src;
		in.sourceSize = QSize(64, 64);
		in.sourceMs = 4000;
		in.loMs = -1000;
		in.hiMs = 2000;
		in.fps = 60.0;
		std::atomic<int> mixes{0};
		in.mixAudio = [&mixes, src](const TimelineModel &m) {
			++mixes;
			return TimelineAudio::mixToBuffer(m, [src](int) { return src; });
		};
		SlipDialog d(in);
		d.resize(900, 640);
		d.show();
		settle(200);
		std::printf("     preview %dx%d, frame %d\n", d.previewImage().width(), d.previewImage().height(),
			    counterOf(d.previewImage()));
		ok(counterOf(d.previewImage()) == 60, "the preview shows the clip's moment: frame 60, at 1 s");
		ok(d.previewImage().width() == d.previewImage().height(),
		   "filling the preview in the clip's own shape -- not small, turned or faded as on the timeline");
		ok(d.readout().contains(QStringLiteral("0:01.0")) && d.readout().contains(QStringLiteral("0:02.0")) &&
			   d.readout().contains(QStringLiteral("0:04.0")),
		   "it says which moment, of how long");
		auto *sourceStart = d.findChild<QDoubleSpinBox *>(QStringLiteral("slipSourceStart"));
		ok(sourceStart && sourceStart->decimals() == 3 && sourceStart->minimum() == 0.0 &&
			   sourceStart->maximum() == 3.0 && sourceStart->value() == 1.0,
		   "Source start shows milliseconds and only the source range that fits the unchanged clip");
		if (sourceStart) {
			sourceStart->setValue(1.237);
			const TlClip precise = d.momentModel(false).tracks[0].clips[0];
			ok(d.deltaMs() == 237 && precise.srcStartMs == 1237 && precise.srcLenMs() == 1000 &&
				   counterOf(d.previewImage()) == frameAt(1237),
			   "typing a precise source start changes the preview and retains the moment's duration");
			ok(in.clip.srcStartMs == 1000 && in.clip.outStartMs == 5000,
			   "choosing a timestamp leaves the caller's timeline clip unchanged until accepted");
			sourceStart->setValue(9.0);
			ok(d.deltaMs() == 2000 && sourceStart->value() == 3.0,
			   "a typed start beyond the source clamps to the latest full-length moment");
			sourceStart->setValue(-1.0);
			ok(d.deltaMs() == -1000 && sourceStart->value() == 0.0,
			   "a typed negative start clamps to the source beginning");
			d.setDeltaMs(345);
			ok(sourceStart->value() == 1.345,
			   "slipping by strip, keyboard or reset updates the timestamp field without rounding to tenths");
			sourceStart->setFocus();
			if (auto *edit = sourceStart->findChild<QLineEdit *>()) {
				edit->deselect();
				edit->setCursorPosition(2);
				key(edit, Qt::Key_Right);
				ok(d.deltaMs() == 345 && edit->cursorPosition() == 3,
				   "arrow keys navigate timestamp text while its input has focus");
			}
			d.setFocus();
		}
		d.setDeltaMs(0);

		SlipTrackBar *bar = d.trackBar();
		SlipTrackBar *overview = d.overviewBar();
		ok(overview && overview->height() < bar->height() && overview->width() == bar->width(),
		   "full-source overview spans the same width at a smaller height");
		const QPoint middle(6 + (overview->width() - 12) / 2, 25);
		mouse(overview, QEvent::MouseButtonPress, middle, Qt::LeftButton);
		mouse(overview, QEvent::MouseButtonRelease, middle, Qt::NoButton);
		ok(std::llabs(d.deltaMs() - 1000) < 10 && bar->delta() == d.deltaMs(),
		   "overview seeking updates the precision strip and selected source start");
		ok(counterOf(d.previewImage()) == frameAt(1000 + d.deltaMs()),
		   "overview navigation renders the newly selected source frame");
		d.setDeltaMs(0);
		ok(overview->delta() == 0, "precise changes and Reset keep the overview selection synchronized");
		const double ppm = bar->pxPerMs();
		dragStrip(bar, -100);
		const qint64 want = qint64(std::llround(100.0 / ppm));
		std::printf("     dragged 100 px left: %lld ms (%.3f px per ms)\n", (long long)d.deltaMs(), ppm);
		ok(d.deltaMs() > 0 && std::llabs(d.deltaMs() - want) <= 1, "dragged left: later footage, as far as the pointer went");
		ok(counterOf(d.previewImage()) == frameAt(1000 + d.deltaMs()), "and the preview shows where it now starts");
		ok(sourceStart && std::abs(sourceStart->value() - (1000 + d.deltaMs()) / 1000.0) < 0.0001,
		   "dragging the source strip keeps the timestamp field in sync");
		ok(bar->windowRect().center().x() == bar->rect().center().x(), "the window stays put; the strip moved");

		dragStrip(bar, 3000);
		ok(d.deltaMs() == -1000 && counterOf(d.previewImage()) == 0, "dragged far right: stops at the first frame");
		ok(d.readout().contains(QStringLiteral("start of the footage")), "and says so");

		d.setDeltaMs(0);
		key(&d, Qt::Key_Right);
		ok(d.deltaMs() == 17, "Right: one frame later");
		key(&d, Qt::Key_Right, Qt::ShiftModifier);
		ok(d.deltaMs() == 1017, "Shift+Right: a second later");
		key(&d, Qt::Key_End);
		ok(d.deltaMs() == 2000 && counterOf(d.previewImage()) == frameAt(3000), "End: the last moment there is");
		key(&d, Qt::Key_Home);
		ok(d.deltaMs() == -1000, "Home: the first");
		d.setDeltaMs(0);
		{
			QWheelEvent wheel(QPointF(bar->rect().center()), bar->mapToGlobal(QPointF(bar->rect().center())),
					  QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
			QApplication::sendEvent(bar, &wheel);
		}
		ok(d.deltaMs() == 100, "the wheel: a tenth of the clip a notch");
		QPushButton *reset = d.findChild<QPushButton *>(QStringLiteral("slipReset"));
		if (reset)
			reset->click();
		ok(reset && d.deltaMs() == 0 && counterOf(d.previewImage()) == 60, "Reset: back to the moment it showed");

		d.setDeltaMs(500);
		const TimelineModel withSound = d.momentModel(true);
		ok(withSound.tracks.size() == 2 && withSound.tracks[1].clips[0].outStartMs == 0 &&
			   withSound.tracks[1].clips[0].srcStartMs == 1500 && withSound.tracks[0].clips[0].fadeInMs == 0,
		   "its sound is heard with it, slipped the same, from the top");

		std::printf("\n-- playing --\n");
		d.setPlaying(true);
		int first = -1, last = -1;
		bool wrapped = false;
		qint64 prev = 0;
		QElapsedTimer t;
		t.start();
		while (t.elapsed() < 1600) {
			settle(40);
			const int n = counterOf(d.previewImage());
			if (first < 0)
				first = n;
			last = std::max(last, n);
			if (d.playheadMs() < prev)
				wrapped = true;
			prev = d.playheadMs();
		}
		std::printf("     frames %d to %d\n", first, last);
		ok(d.isPlaying() && last > first && first >= frameAt(1500) && last <= frameAt(2500),
		   "Play: the moment plays, and only the moment");
		ok(wrapped, "round again at its end");
		if (AudioPreview::available()) {
			ok(mixes.load() > 0, "with its sound");
		} else {
			std::printf("     (no audio output here: the sound is not checked)\n");
		}
		dragStrip(bar, -30);
		ok(d.isPlaying() && d.playheadMs() < 200, "a slip while playing plays the new moment from its top");
		const QPoint overviewDragStart(int(6 + 0.4 * (overview->width() - 12)), 25);
		const QPoint overviewDragEnd(int(6 + 0.6 * (overview->width() - 12)), 25);
		mouse(overview, QEvent::MouseButtonPress, overviewDragStart, Qt::LeftButton);
		const int heldFrame = counterOf(d.previewImage());
		settle(120);
		ok(d.isPlaying() && counterOf(d.previewImage()) == heldFrame,
		   "overview dragging holds the chosen frame while preserving the playback intent");
		mouse(overview, QEvent::MouseMove, overviewDragEnd, Qt::LeftButton);
		mouse(overview, QEvent::MouseButtonRelease, overviewDragEnd, Qt::NoButton);
		const int releasedFrame = counterOf(d.previewImage());
		settle(120);
		ok(d.isPlaying() && overview->delta() == bar->delta() && counterOf(d.previewImage()) > releasedFrame,
		   "releasing an overview drag resumes playback from the newly chosen moment");
		key(&d, Qt::Key_Escape);
		ok(d.result() == QDialog::Rejected && !d.isPlaying(), "Esc: closed, nothing chosen, nothing playing");

		SlipDialog pending(in);
		auto *pendingStart = pending.findChild<QDoubleSpinBox *>(QStringLiteral("slipSourceStart"));
		auto *pendingEdit = pendingStart ? pendingStart->findChild<QLineEdit *>() : nullptr;
		if (pendingEdit)
			pendingEdit->setText(QStringLiteral("2.345 s"));
		ok(pendingStart && pendingEdit && pendingStart->value() == 1.0 && pending.deltaMs() == 0,
		   "typing a timestamp leaves a pending edit until it is committed");
		pending.accept();
		ok(pending.result() == QDialog::Accepted && pending.deltaMs() == 1345 &&
			   pending.momentModel(false).tracks[0].clips[0].srcStartMs == 2345,
		   "OK commits pending timestamp text before accepting, even without focus leaving the field");
	}

	std::printf("\n-- in the editor --\n");
	{
		VideoEditorWindow w(src);
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
		int lane = -1, id = -1;
		for (int i = 0; i < tv->model().tracks.size() && lane < 0; ++i)
			for (const TlClip &c : tv->model().tracks[i].clips)
				if (c.type == TlClip::Type::Video && tv->model().tracks[i].kind == TlTrack::Kind::Video) {
					lane = i;
					id = c.sourceId;
				}
		ok(lane >= 0, "the source is on the timeline");
		if (lane < 0)
			return 1;
		{
			// The clip, 1 to 2 s of the source; a caption over it, on top.
			TimelineModel m = tv->model();
			TlTrack v = m.tracks[lane];
			v.clips = {media(id, 0, 1000, 2000)};
			TlTrack cap;
			cap.kind = TlTrack::Kind::Video;
			TlClip text;
			text.type = TlClip::Type::Text;
			text.text.text = QStringLiteral("CAPTION OVER IT");
			text.text.fontPx = 400;
			text.srcEndMs = 1000;
			cap.clips = {text};
			m.tracks = {cap, v};
			tv->setModelAndCommit(m);
			settle(300);
		}
		const int V = 1;
		int commits = 0;
		QObject::connect(tv, &TimelineView::editCommitted, [&commits]() { ++commits; });

		int seenFrame = -1;
		qint64 chose = 0;
		bool cancel = false;
		QTimer driver;
		driver.setInterval(50);
		bool menuHadSlip = false;
		QObject::connect(&driver, &QTimer::timeout, [&]() {
			// The clip's menu: choose Slip..., as a user would.
			if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
				for (QAction *a : menu->actions())
					if (a->text() == QStringLiteral("Slip\u2026") && a->isEnabled()) {
						menuHadSlip = true;
						menu->setActiveAction(a);
						QKeyEvent ret(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
						QApplication::sendEvent(menu, &ret);
						return;
					}
				menu->close();
				return;
			}
			for (QWidget *top : QApplication::topLevelWidgets()) {
				auto *d = dynamic_cast<SlipDialog *>(top);
				if (!d || !d->isVisible())
					continue;
				seenFrame = counterOf(d->previewImage());
				dragStrip(d->trackBar(), -80);
				chose = d->deltaMs();
				if (cancel)
					d->reject();
				else
					d->accept();
				return;
			}
		});
		driver.start();
		{
			const QPoint at = tv->clipRectForTest(V, 0).center();
			QMouseEvent right(QEvent::MouseButtonPress, QPointF(at), tv->mapToGlobal(QPointF(at)), Qt::RightButton,
					  Qt::RightButton, Qt::NoModifier);
			QApplication::sendEvent(tv, &right); // right-click the clip
		}
		settle(100);
		ok(menuHadSlip, "the clip's menu offers Slip...");
		ok(seenFrame == 60, "the window shows the clip's moment alone: the caption over it is not in it");
		const TlClip c = tv->model().tracks[V].clips[0];
		ok(chose > 0 && c.srcStartMs == 1000 + chose && c.srcLenMs() == 1000 && c.outStartMs == 0,
		   "OK: the clip shows the chosen moment, in its place, for its length");
		ok(commits == 1, "as one undo step");
		ok(tv->selectedTrack() == V && tv->selectedCountForTest() == 1, "the clip is the selection, alone");

		cancel = true;
		commits = 0;
		emit tv->slipRequested(V, 0);
		settle(100);
		ok(tv->model().tracks[V].clips[0].srcStartMs == 1000 + chose && commits == 0,
		   "Cancel: the clip as it was, and no undo step");
		driver.stop();
	}

	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
