// Pasting a picture from the clipboard onto the timeline.
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
//     dispatch rule that always picks one branch passes a one-way test.
#include "editor/StillImage.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
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

	// Deliberately BEFORE anything copies clips: with clips in hand, text on the
	// clipboard falling through to a clip paste is the correct answer, and that
	// case is checked below. This is the empty-handed one.
	std::printf("\n-- with nothing to paste, Ctrl+V says so rather than going quiet --\n");
	{
		cb->setText(QStringLiteral("just some words"));
		settle(200);
		const int before = clipCount(tv);
		QMetaObject::invokeMethod(&w, "pasteFromClipboard");
		settle(400);
		std::printf("     clips %d -> %d\n", before, clipCount(tv));
		ok(clipCount(tv) == before, "no clip appeared");
		// Silence would read as a broken shortcut; the message is the feature.
		const QString msg = w.infoTextForTest();
		std::printf("     said: %s\n", qPrintable(msg));
		ok(msg.contains(QStringLiteral("text"), Qt::CaseInsensitive),
		   "and it said the clipboard holds text rather than saying nothing");
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
		const TlClip *c = lastImageClip(tv);
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
		const TlClip *c2 = lastImageClip(tv);
		const bool newImage =
			c2 && readStillImage(w.sourcePathForTest(c2->sourceId)).size() == QSize(64, 64);
		std::printf("     clips last: clips %d -> %d, a 64x64 image appeared: %s\n", before,
			    clipCount(tv), newImage ? "YES (wrong)" : "no");
		ok(clipCount(tv) == before + 1, "something was pasted");
		ok(!newImage, "and it was the CLIPS, because the clips were copied last");

		// Text arriving on the clipboard is not a reason to stop pasting clips.
		// It is newer than the copy, but it is not something that can be placed,
		// so the rule has to skip it rather than treat it as the winner.
		cb->setText(QStringLiteral("copied some words by accident"));
		settle(200);
		before = clipCount(tv);
		QMetaObject::invokeMethod(&w, "pasteFromClipboard");
		settle(500);
		std::printf("     then text arrives: clips %d -> %d\n", before, clipCount(tv));
		ok(clipCount(tv) == before + 1,
		   "unpasteable text does not cancel the clips you copied");
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
		const TlClip *c = lastImageClip(tv);
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

	std::printf("\n-- the button follows the clipboard --\n");
	{
		QPushButton *b = button(&w, QStringLiteral("Paste image"));
		ok(b != nullptr, "there is a Paste image button");
		if (b) {
			cb->setText(QStringLiteral("still words"));
			settle(250);
			const bool offWithText = !b->isEnabled();
			cb->setImage(marker(30, 30));
			settle(250);
			std::printf("     enabled with text: %s, with an image: %s\n",
				    offWithText ? "no" : "YES", b->isEnabled() ? "yes" : "NO");
			ok(offWithText, "greyed out when there is nothing to paste");
			ok(b->isEnabled(), "and live when there is");
		}
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
