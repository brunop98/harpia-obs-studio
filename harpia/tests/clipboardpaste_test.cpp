// Pasting from the clipboard onto the timeline: Add > Paste and Ctrl+V.
//
// The trap here is not the paste. It is that a source is identified by its
// PATH: the project writes sources[].path and relinks by it on open, and the
// export worker re-reads that path on its own thread rather than touching the
// editor's in-memory cache of decoded stills. An implementation that kept the
// pasted QImage only in RAM would look completely correct -- the clip appears,
// the preview shows the picture -- and then save a project that reopens broken
// and export an empty frame. Neither symptom appears until later, and neither
// points back at the paste.
//
// So the assertions here deliberately go past "a clip appeared":
//
//   - the image is on disk afterwards, and the source points at it;
//   - a project saved and reopened still resolves it;
//   - the EXPORTER's own reader gets the picture back, not a null image;
//   - Ctrl+V still pastes clips when clips are what was copied last, and
//     pastes the image when the image was -- in both orders, because a
//     dispatch rule that always picks one branch passes a one-way test;
//   - Add > Paste puts it on the best-fit lane: the first lane free for it,
//     or a new one on top -- never over a clip already there;
//   - sound copied as data, a link to a picture (from a web server inside the
//     test) and plain words each become the right kind of clip.
#include "editor/StillImage.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"
#include "support/TinyHttpServer.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QBuffer>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QNetworkProxy>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

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
		QThread::msleep(5);
	}
}
static QPushButton *button(QWidget *w, const QString &text)
{
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (b->text() == text)
			return b;
	return nullptr;
}

// A picture nothing else in the app could have produced, so finding it later
// proves it came from the clipboard rather than from the source video.
static QImage marker(int w = 240, int h = 120)
{
	QImage img(w, h, QImage::Format_RGBA8888);
	img.fill(QColor(17, 205, 89));
	for (int y = 0; y < h / 2; ++y)
		for (int x = 0; x < w / 2; ++x)
			img.setPixelColor(x, y, QColor(240, 30, 120));
	return img;
}

// The Add menu's Paste entry, as the menu bar shows it.
static QAction *addPasteAction(QWidget *w)
{
	for (QMenuBar *bar : w->findChildren<QMenuBar *>())
		for (QAction *top : bar->actions())
			if (top->menu() && top->text().remove(QLatin1Char('&')) == QLatin1String("Add"))
				for (QAction *a : top->menu()->actions())
					if (a->text() == QLatin1String("Paste"))
						return a;
	return nullptr;
}

// The selected clip -- the one just pasted -- overlaps nothing else on its
// lane. (Not every lane: Ctrl+V of copied clips pastes where it is told to,
// over whatever is there, and earlier sections do that on purpose.)
static bool pastedCoversNothing(TimelineView *tv)
{
	const int t = tv->selectedTrack(), c = tv->selectedClip();
	if (t < 0 || t >= tv->model().tracks.size() || c < 0 || c >= tv->model().tracks[t].clips.size())
		return false;
	const QVector<TlClip> &clips = tv->model().tracks[t].clips;
	for (int i = 0; i < clips.size(); ++i)
		if (i != c && clips[i].outStartMs < clips[c].outEndMs() && clips[c].outStartMs < clips[i].outEndMs())
			return false;
	return true;
}

// The lane and index of the clip that is selected after a paste.
static const TlClip *selectedClip(TimelineView *tv, int *lane = nullptr)
{
	const int t = tv->selectedTrack(), c = tv->selectedClip();
	if (lane)
		*lane = t;
	if (t < 0 || t >= tv->model().tracks.size() || c < 0 || c >= tv->model().tracks[t].clips.size())
		return nullptr;
	return &tv->model().tracks[t].clips[c];
}

static QByteArray wavBytes(int ms)
{
	const int frames = 48 * ms;
	QByteArray data(frames * 4, '\0');
	QByteArray b;
	auto u32 = [&b](quint32 v) {
		for (int i = 0; i < 4; ++i)
			b.append(char((v >> (8 * i)) & 0xff));
	};
	auto u16 = [&b](quint16 v) {
		b.append(char(v & 0xff));
		b.append(char(v >> 8));
	};
	b += "RIFF";
	u32(36 + quint32(data.size()));
	b += "WAVEfmt ";
	u32(16);
	u16(1);
	u16(2);
	u32(48000);
	u32(48000 * 4);
	u16(4);
	u16(16);
	b += "data";
	u32(quint32(data.size()));
	return b + data;
}

static int clipCount(TimelineView *tv)
{
	int n = 0;
	for (const TlTrack &t : tv->model().tracks)
		n += t.clips.size();
	return n;
}
static const TlClip *lastImageClip(TimelineView *tv)
{
	const TlClip *found = nullptr;
	for (const TlTrack &t : tv->model().tracks)
		for (const TlClip &c : t.clips)
			if (c.type == TlClip::Type::Image)
				found = &c;
	return found;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	// The link test's server is on this machine; no proxy is to be asked.
	QNetworkProxyFactory::setUseSystemConfiguration(false);
	QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);

	VideoEditorWindow w(QStringLiteral(SRC_MEDIA));
	w.resize(1300, 900);
	w.show();
	QApplication::processEvents();
	if (QPushButton *full = button(&w, QStringLiteral("Full Editing"))) {
		full->click();
		settle(400);
	}
	TimelineView *tv = w.findChild<TimelineView *>();
	ok(tv != nullptr, "the editor came up in Full editing");
	if (!tv)
		return 1;

	QClipboard *cb = QApplication::clipboard();

	std::printf("\n-- a clipboard image becomes a clip, and a FILE --\n");
	QString pastedPath;
	{
		const int before = clipCount(tv);
		cb->setImage(marker());
		settle(200);
		QMetaObject::invokeMethod(&w, "pasteFromClipboard");
		settle(600);

		ok(clipCount(tv) == before + 1, "one clip arrived");
		const TlClip *c = lastImageClip(tv);
		ok(c != nullptr, "and it is an image clip");
		if (!c) {
			std::printf("\nFAILURES\n");
			return 1;
		}
		// The assertion the RAM-only design fails. Everything downstream of
		// here -- project reload, export -- reads this path and nothing else.
		pastedPath = w.sourcePathForTest(c->sourceId);
		std::printf("     source path: %s\n", qPrintable(pastedPath));
		ok(!pastedPath.isEmpty(), "the clip's source has a path at all");
		ok(QFileInfo::exists(pastedPath), "and there is a real file at it");
		ok(pastedPath.endsWith(QStringLiteral(".png")),
		   "written as PNG, which is lossless and keeps alpha");

		// And it is the RIGHT picture, not a blank of the right size.
		const QImage back = readStillImage(pastedPath);
		std::printf("     read back: %dx%d, top-left rgb(%d,%d,%d)\n", back.width(),
			    back.height(), back.pixelColor(2, 2).red(), back.pixelColor(2, 2).green(),
			    back.pixelColor(2, 2).blue());
		ok(back.size() == QSize(240, 120), "the file holds the pasted image's size");
		ok(back.pixelColor(2, 2) == QColor(240, 30, 120) &&
			   back.pixelColor(200, 100) == QColor(17, 205, 89),
		   "and its actual pixels, both colours");
	}

	std::printf("\n-- and the reader the EXPORT worker uses can read it --\n");
	{
		// ClipExporter re-reads the path on its worker thread; it never sees the
		// window's decoded-still cache. It used to do a bare QImage(path), which
		// is the same call the line below proves is not enough.
		const QImage asExportSeesIt = readStillImage(pastedPath);
		ok(!asExportSeesIt.isNull(),
		   "the path the exporter is handed decodes to a real picture");

		// The bug that fix was for, stated as the difference between the two
		// readers. A WebP still previewed correctly (the window's reader falls
		// back to libav) and encoded as an empty frame (the exporter's did not).
		// On a Qt WITH the plugin both work and this proves nothing -- so say
		// which build this is rather than passing quietly either way.
		QTemporaryDir wd;
		const QString webp = wd.filePath(QStringLiteral("shot.webp"));
		QProcess ff;
		ff.start(QStringLiteral("ffmpeg"),
			 {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
			  QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
			  QStringLiteral("-i"), QStringLiteral("testsrc=size=64x48:duration=1:rate=1"),
			  QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-y"), webp});
		if (ff.waitForFinished(30000) && ff.exitCode() == 0) {
			const bool qtCan = !QImage(webp).isNull();
			const bool sharedCan = !readStillImage(webp).isNull();
			std::printf("     plain QImage() reads webp here: %s; readStillImage: %s\n",
				    qtCan ? "yes" : "NO", sharedCan ? "yes" : "NO");
			ok(sharedCan, "the shared reader decodes a webp still");
			if (!qtCan)
				ok(sharedCan && !qtCan,
				   "and it is strictly better than the bare QImage(path) the "
				   "exporter used to call -- which is the bug");
			else
				std::printf("     (this Qt has the webp plugin, so the exporter's "
					    "old call would have worked here too -- the release "
					    "build's Qt does not)\n");
		}
	}

	std::printf("\n-- a saved project reopens with the paste intact --\n");
	{
		QTemporaryDir dir;
		const QString proj = dir.filePath(QStringLiteral("p.harpiaproj"));
		const QString err = w.saveProjectTo(proj, /*quiet=*/true);
		ok(err.isEmpty(), "the project saved");

		// The path has to be IN the file: relink is by path, so a project that
		// wrote only an id would reopen with a clip pointing at nothing.
		QFile f(proj);
		f.open(QIODevice::ReadOnly);
		const QByteArray raw = f.readAll();
		ok(raw.contains(QFileInfo(pastedPath).fileName().toUtf8()),
		   "and it names the pasted file, which is how reopening finds it");

		const int wanted = clipCount(tv);
		ok(w.openProjectAt(proj), "the project reopened");
		settle(500);
		std::printf("     clips after reopen: %d (was %d)\n", clipCount(tv), wanted);
		ok(clipCount(tv) == wanted, "with the same clips");
		const TlClip *c = lastImageClip(tv);
		ok(c && !w.sourcePathForTest(c->sourceId).isEmpty(),
		   "and the image clip still resolves to a file");
	}

	// Deliberately BEFORE anything copies clips: with clips in hand, words
	// copied inside Harpia falling through to a clip paste is the correct
	// answer, and that case is checked below. These are the empty-handed ones.
	std::printf("\n-- with nothing to paste, Ctrl+V says so rather than going quiet --\n");
	{
		cb->clear();
		settle(200);
		const int before = clipCount(tv);
		QMetaObject::invokeMethod(&w, "pasteFromClipboard");
		settle(400);
		std::printf("     clips %d -> %d\n", before, clipCount(tv));
		ok(clipCount(tv) == before, "no clip appeared");
		// Silence would read as a broken shortcut; the message is the feature.
		const QString msg = w.infoTextForTest();
		std::printf("     said: %s\n", qPrintable(msg));
		ok(msg.contains(QStringLiteral("nothing"), Qt::CaseInsensitive),
		   "and it said there is nothing on the clipboard");
	}

	std::printf("\n-- words become a text clip --\n");
	{
		cb->setText(QStringLiteral("Hello from the clipboard"));
		settle(200);
		const int before = clipCount(tv);
		QMetaObject::invokeMethod(&w, "pasteFromClipboard");
		settle(400);
		const TlClip *c = selectedClip(tv);
		std::printf("     clips %d -> %d, selected: %s\n", before, clipCount(tv),
			    c && c->type == TlClip::Type::Text ? qPrintable(c->text.text) : "(not a text clip)");
		ok(clipCount(tv) == before + 1 && c && c->type == TlClip::Type::Text &&
			   c->text.text == QLatin1String("Hello from the clipboard"),
		   "a text clip, with those words");
		ok(pastedCoversNothing(tv), "on a lane where it covers nothing");
	}

	std::printf("\n-- Ctrl+V picks whichever was copied LAST --\n");
	{
		// Both orders. A dispatcher hard-wired to one branch passes whichever
		// half is tested alone, so neither half is worth anything by itself.
		tv->selectClip(0, 0);
		settle(200);

		// Order A: copy clips, THEN put an image on the clipboard -> image wins.
		QMetaObject::invokeMethod(&w, "copySelectedClipsForTest");
		settle(150);
		cb->setImage(marker(80, 60));
		settle(200);
		int before = clipCount(tv);
		QMetaObject::invokeMethod(&w, "pasteFromClipboard");
		settle(500);
		const TlClip *c = selectedClip(tv); // a paste selects what it placed
		std::printf("     image last: clips %d -> %d, newest image is %s\n", before,
			    clipCount(tv),
			    c ? qPrintable(QFileInfo(w.sourcePathForTest(c->sourceId)).fileName())
			      : "(none)");
		ok(clipCount(tv) == before + 1, "something was pasted");
		ok(c && readStillImage(w.sourcePathForTest(c->sourceId)).size() == QSize(80, 60),
		   "and it was the IMAGE, because the image was copied last");

		// Order B: put an image on the clipboard, THEN copy clips -> clips win.
		cb->setImage(marker(64, 64));
		settle(200);
		tv->selectClip(0, 0);
		settle(150);
		QMetaObject::invokeMethod(&w, "copySelectedClipsForTest");
		settle(150);
		before = clipCount(tv);
		QMetaObject::invokeMethod(&w, "pasteFromClipboard");
		settle(500);
		bool newImage = false; // any 64x64 picture on the timeline at all
		for (const TlTrack &t : tv->model().tracks)
			for (const TlClip &k : t.clips)
				newImage |= k.type == TlClip::Type::Image &&
					    readStillImage(w.sourcePathForTest(k.sourceId)).size() == QSize(64, 64);
		std::printf("     clips last: clips %d -> %d, a 64x64 image appeared: %s\n", before,
			    clipCount(tv), newImage ? "YES (wrong)" : "no");
		ok(clipCount(tv) == before + 1, "something was pasted");
		ok(!newImage, "and it was the CLIPS, because the clips were copied last");

		// Words copied inside Harpia (a clip name, a line of the Inspector) are
		// newer than the copy, but they were a side effect of editing, not a
		// thing to place -- so they do not take the place of the clips. (This
		// test is Harpia: its clipboard writes are Harpia's own.)
		cb->setText(QStringLiteral("copied some words by accident"));
		settle(200);
		before = clipCount(tv);
		int textClipsBefore = 0;
		for (const TlTrack &t : tv->model().tracks)
			for (const TlClip &k : t.clips)
				textClipsBefore += k.type == TlClip::Type::Text;
		QMetaObject::invokeMethod(&w, "pasteFromClipboard");
		settle(500);
		int textClipsAfter = 0;
		for (const TlTrack &t : tv->model().tracks)
			for (const TlClip &k : t.clips)
				textClipsAfter += k.type == TlClip::Type::Text;
		std::printf("     then words copied in Harpia: clips %d -> %d, text clips %d -> %d\n", before,
			    clipCount(tv), textClipsBefore, textClipsAfter);
		ok(clipCount(tv) == before + 1 && textClipsAfter == textClipsBefore,
		   "words copied inside Harpia do not replace the clips you copied");
	}

	std::printf("\n-- a file copied in the file manager pastes too --\n");
	{
		// Explorer and Finder put a URL list on the clipboard, not a picture.
		QTemporaryDir dir;
		const QString png = dir.filePath(QStringLiteral("logo.png"));
		marker(100, 50).save(png, "PNG");
		auto *mime = new QMimeData;
		mime->setUrls({QUrl::fromLocalFile(png)});
		cb->setMimeData(mime);
		settle(200);
		const int before = clipCount(tv);
		QMetaObject::invokeMethod(&w, "pasteFromClipboard");
		settle(600);
		const TlClip *c = selectedClip(tv);
		std::printf("     clips %d -> %d, source %s\n", before, clipCount(tv),
			    c ? qPrintable(QFileInfo(w.sourcePathForTest(c->sourceId)).fileName())
			      : "(none)");
		ok(clipCount(tv) == before + 1, "the copied file landed on the timeline");
		// Used where it lies rather than copied into the pasted folder: it
		// already has a path, which is all the project and the exporter need.
		ok(c && QFileInfo(w.sourcePathForTest(c->sourceId)).fileName() ==
				QStringLiteral("logo.png"),
		   "and it points at the original file, not a copy");
	}

	std::printf("\n-- Add > Paste: the best-fit lane --\n");
	{
		// A clip sitting at the playhead on the top picture lane: a paste
		// there has to go somewhere it covers nothing.
		tv->setPlayhead(500);
		settle(100);
		int topLane = -1;
		for (int i = 0; i < tv->model().tracks.size() && topLane < 0; ++i)
			if (tv->model().tracks[i].kind == TlTrack::Kind::Video)
				topLane = i;
		bool topBusy = false;
		for (const TlClip &k : tv->model().tracks[topLane].clips)
			topBusy |= k.outStartMs < 5500 && k.outEndMs() > 500;
		const int lanesBefore = tv->model().tracks.size();
		cb->setImage(marker(50, 40));
		settle(200);
		QMetaObject::invokeMethod(&w, "pasteSystemClipboardForTest");
		settle(500);
		int lane = -1;
		const TlClip *c = selectedClip(tv, &lane);
		std::printf("     top picture lane busy at the playhead: %s; landed on lane %d (lanes %d -> %d)\n",
			    topBusy ? "yes" : "no", lane, lanesBefore, int(tv->model().tracks.size()));
		ok(c && c->type == TlClip::Type::Image && c->outStartMs == 500, "an image clip, at the playhead");
		ok(pastedCoversNothing(tv), "covering nothing on its lane");
		bool firstFree = true;
		for (int i = 0; c && i < lane; ++i) {
			const TlTrack &t = tv->model().tracks[i];
			if (t.kind != TlTrack::Kind::Video || t.locked || t.name == QLatin1String("Subtitles"))
				continue;
			bool free = true;
			for (const TlClip &k : t.clips)
				free &= !(k.outStartMs < c->outEndMs() && k.outEndMs() > c->outStartMs);
			firstFree &= !free;
		}
		ok(firstFree, "and no free picture lane above the one it took");

		// Every picture lane busy there: a new lane, on top.
		cb->setImage(marker(52, 40));
		settle(200);
		TimelineModel m = tv->model();
		for (TlTrack &t : m.tracks)
			if (t.kind == TlTrack::Kind::Video) {
				TlClip blocker;
				blocker.type = TlClip::Type::Text;
				blocker.srcStartMs = 0;
				blocker.srcEndMs = 60000;
				blocker.outStartMs = 400000;
				blocker.text.text = QStringLiteral("blocker");
				t.clips.append(blocker);
			}
		tv->setModelAndCommit(m);
		tv->setPlayhead(410000);
		settle(100);
		const int before = tv->model().tracks.size();
		QMetaObject::invokeMethod(&w, "pasteSystemClipboardForTest");
		settle(500);
		const TlClip *c2 = selectedClip(tv, &lane);
		std::printf("     every picture lane busy: lanes %d -> %d, landed on lane %d\n", before,
			    int(tv->model().tracks.size()), lane);
		ok(c2 && tv->model().tracks.size() == before + 1 && lane == 0,
		   "every lane busy: a new picture lane, on top");
		ok(pastedCoversNothing(tv), "still covering nothing");
		tv->setPlayhead(0);
	}

	std::printf("\n-- Add > Paste: sound copied as data --\n");
	{
		auto *mime = new QMimeData;
		mime->setData(QStringLiteral("audio/wav"), wavBytes(1500));
		cb->setMimeData(mime);
		settle(200);
		tv->setPlayhead(2000);
		settle(100);
		QMetaObject::invokeMethod(&w, "pasteSystemClipboardForTest");
		settle(800);
		int lane = -1;
		const TlClip *c = selectedClip(tv, &lane);
		const bool onAudio = lane >= 0 && tv->model().tracks[lane].kind == TlTrack::Kind::Audio;
		std::printf("     landed on lane %d (%s), %lld ms long at %lld\n", lane, onAudio ? "audio" : "NOT audio",
			    c ? (long long)c->outDurationMs() : -1LL, c ? (long long)c->outStartMs : -1LL);
		ok(c && onAudio && c->outStartMs == 2000, "an audio clip, at the playhead, on an audio lane");
		ok(c && std::abs(c->outDurationMs() - 1500) < 50, "as long as the sound copied");
		ok(c && QFileInfo::exists(w.sourceFileForTest(c->sourceId)), "kept as a file, like a pasted picture");
		ok(pastedCoversNothing(tv), "covering nothing");
		tv->setPlayhead(0);
	}

	std::printf("\n-- Add > Paste: a link to a picture --\n");
	{
		TinyHttpServer web;
		QByteArray png;
		{
			QBuffer buf(&png);
			buf.open(QIODevice::WriteOnly);
			marker(64, 32).save(&buf, "PNG");
		}
		web.routes["/shot.png"] = TinyRoute{200, "image/png", png, {}, false, -1};
		cb->setText(web.url("/shot.png"));
		settle(200);
		tv->setPlayhead(3000);
		settle(100);
		const int before = clipCount(tv);
		QMetaObject::invokeMethod(&w, "pasteSystemClipboardForTest");
		QElapsedTimer t;
		t.start();
		while (clipCount(tv) == before && t.elapsed() < 5000)
			settle(50);
		const TlClip *c = selectedClip(tv);
		const QString path = c ? w.sourcePathForTest(c->sourceId) : QString();
		std::printf("     clips %d -> %d, source %s\n", before, clipCount(tv), qPrintable(QFileInfo(path).fileName()));
		ok(c && c->type == TlClip::Type::Image && c->outStartMs == 3000,
		   "downloaded, and an image clip at the playhead it was pasted at");
		ok(readStillImage(path).size() == QSize(64, 32), "of the picture the link pointed to");
		tv->setPlayhead(0);
	}

	std::printf("\n-- the menu entry follows the clipboard --\n");
	{
		QAction *a = addPasteAction(&w);
		ok(a != nullptr, "there is Add > Paste");
		if (a) {
			cb->clear();
			settle(250);
			const bool offEmpty = !a->isEnabled();
			cb->setText(QStringLiteral("words are pasteable now"));
			settle(250);
			const bool onText = a->isEnabled();
			cb->setImage(marker(30, 30));
			settle(250);
			std::printf("     empty: %s, words: %s, an image: %s\n", offEmpty ? "off" : "ON",
				    onText ? "on" : "OFF", a->isEnabled() ? "on" : "OFF");
			ok(offEmpty, "greyed out when there is nothing to paste");
			ok(onText && a->isEnabled(), "and live when there is: words, a picture");
		}
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
