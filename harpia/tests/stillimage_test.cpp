// Reading a still, and saying why when it cannot be read.
//
// "Could not read that image." was the whole message. It gave no way to tell
// apart a missing file, an unreadable one, a format this build has no plugin
// for, and an image too large for Qt's allocation limit — and the most common
// cause was the one nobody could guess: the file dialog offered *.webp on every
// build, while Qt only reads WebP when the qtimageformats plugin is present,
// which the trimmed obs-deps Qt used for releases does not ship.
//
// So two things are checked here. That the formats actually load — including
// WebP, which now falls back to the libav decoder already linked for video. And
// that a failure names a cause, because a message that cannot be acted on is
// barely better than no message.
#include "editor/VideoEditorWindow.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QProcess>
#include <QTemporaryDir>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

// Written with ffmpeg rather than Qt, so the WebP case is a genuinely foreign
// file and not something Qt made and can therefore obviously read back.
static bool makeWith(const QString &tool, const QStringList &args)
{
	QProcess p;
	p.start(tool, args);
	return p.waitForFinished(30000) && p.exitCode() == 0;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	QTemporaryDir dir;
	const QString png = dir.filePath(QStringLiteral("a.png"));
	const QString jpg = dir.filePath(QStringLiteral("a.jpg"));
	const QString webp = dir.filePath(QStringLiteral("a.webp"));
	const QStringList base = {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
				  QStringLiteral("error"),      QStringLiteral("-f"),
				  QStringLiteral("lavfi"),      QStringLiteral("-i"),
				  QStringLiteral("testsrc=size=320x180:duration=1:rate=1"),
				  QStringLiteral("-frames:v"),  QStringLiteral("1"),
				  QStringLiteral("-y")};
	const bool havePng = makeWith(QStringLiteral("ffmpeg"), QStringList(base) << png);
	const bool haveJpg = makeWith(QStringLiteral("ffmpeg"), QStringList(base) << jpg);
	const bool haveWebp = makeWith(QStringLiteral("ffmpeg"), QStringList(base) << webp);
	ok(havePng && haveJpg && haveWebp, "made a png, a jpg and a webp to read");
	if (!havePng || !haveJpg || !haveWebp)
		return 1;

	// Say out loud whether Qt can do WebP here, so a pass on a machine WITH the
	// plugin is not mistaken for proof that the fallback works.
	const bool qtDoesWebp = QImageReader::supportedImageFormats().contains("webp");
	std::printf("     Qt reads webp natively here: %s\n", qtDoesWebp ? "yes" : "NO");

	std::printf("\n-- the three formats load --\n");
	{
		struct { const QString &path; const char *name; } cases[] = {
			{png, "png"}, {jpg, "jpg"}, {webp, "webp"}};
		for (const auto &c : cases) {
			QString why;
			const QImage img = VideoEditorWindow::readStillImage(c.path, &why);
			std::printf("     %-4s -> %dx%d %s\n", c.name, img.width(), img.height(),
				    img.isNull() ? qPrintable(QStringLiteral("FAILED: ") + why) : "");
			ok(!img.isNull() && img.width() == 320 && img.height() == 180,
			   QByteArray(c.name).append(" decoded at its real size").constData());
		}
		if (!qtDoesWebp)
			std::printf("     (the webp above came through the libav fallback, which "
				    "is the case that used to fail)\n");
	}

	std::printf("\n-- a failure says WHY --\n");
	{
		// Missing file.
		QString why;
		QImage img = VideoEditorWindow::readStillImage(dir.filePath(QStringLiteral("nope.png")),
							       &why);
		std::printf("     missing: %s\n", qPrintable(why));
		ok(img.isNull(), "a missing file does not decode");
		ok(why.contains(QStringLiteral("no file"), Qt::CaseInsensitive),
		   "and the reason names it as missing rather than unreadable");

		// A real file that is not an image at all.
		const QString junk = dir.filePath(QStringLiteral("notes.png"));
		{
			QFile f(junk);
			f.open(QIODevice::WriteOnly);
			f.write("this is not a picture, whatever the extension says\n");
		}
		why.clear();
		img = VideoEditorWindow::readStillImage(junk, &why);
		std::printf("     junk message in full:\n---\n%s\n---\n", qPrintable(why));
		ok(img.isNull(), "junk with an image extension does not decode");
		// The three things the old message never said: which file, what the
		// decoder complained about, and what this build can read.
		ok(why.contains(QStringLiteral("notes.png")), "the reason names the file");
		ok(why.contains(QStringLiteral("could not be decoded")), "says what went wrong");
		ok(why.contains(QStringLiteral("This build reads")),
		   "and lists what this build CAN read, which is the actionable part");
		ok(!why.isEmpty() && why != QStringLiteral("Could not read that image."),
		   "it is not the old dead-end message");
	}

	std::printf("\n-- the dialog only offers what can be opened --\n");
	{
		const QString filter = VideoEditorWindow::imageOpenFilter();
		std::printf("     %s\n", qPrintable(filter.left(120)));
		ok(filter.contains(QStringLiteral("*.png")), "png is offered");
		ok(filter.contains(QStringLiteral("*.jpg")), "jpg is offered");
		// Offered because readStillImage really can open it now — via Qt where
		// the plugin exists, via libav where it does not.
		ok(filter.contains(QStringLiteral("*.webp")), "webp is offered, and now honoured");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
