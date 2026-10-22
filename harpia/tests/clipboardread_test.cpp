// Add > Paste, below the window: what the clipboard is read as, where on the
// timeline it goes, and how a pasted link is fetched.
//
//   * The clipboard, most to least specific: media files, sound data, a
//     picture (unless real words came with it -- Word copies a picture of
//     the text too), one web address, words. Nothing usable is Nothing.
//   * The "best fit" lane: the first lane of the kind free for the clips'
//     whole span, skipping locked lanes and the Subtitles lane; else a new
//     lane on top of the kind's group, under a Subtitles lane at the very top.
//   * A link, against a web server in the test: a picture or video is saved
//     (by its Content-Type, or by its name when the server only says
//     octet-stream), a redirect is followed, a page is reported and NOT saved,
//     a 404 fails, and a cancelled download leaves no half file behind.
#include "editor/ClipboardPaste.hpp"
#include "editor/LinkDownload.hpp"
#include "editor/timeline/PastePlacement.hpp"
#include "support/TinyHttpServer.hpp"

#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMimeData>
#include <QNetworkProxy>
#include <QSignalSpy>
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

static void settleUntil(const std::function<bool()> &done, int ms = 5000)
{
	QElapsedTimer t;
	t.start();
	while (!done() && t.elapsed() < ms)
		QApplication::processEvents(QEventLoop::AllEvents, 20);
}

// A real WAV: a RIFF header, a fmt chunk and a short burst of silence.
static QByteArray wav()
{
	QByteArray data(4800 * 4, '\0');
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

static QByteArray png()
{
	QImage img(8, 6, QImage::Format_RGB32);
	img.fill(Qt::red);
	QByteArray out;
	QBuffer buf(&out);
	buf.open(QIODevice::WriteOnly);
	img.save(&buf, "PNG");
	return out;
}

static const char *kindName(PasteContent::Kind k)
{
	switch (k) {
	case PasteContent::Kind::Nothing:
		return "Nothing";
	case PasteContent::Kind::Files:
		return "Files";
	case PasteContent::Kind::Audio:
		return "Audio";
	case PasteContent::Kind::Image:
		return "Image";
	case PasteContent::Kind::Link:
		return "Link";
	case PasteContent::Kind::Text:
		return "Text";
	}
	return "?";
}

static TlClip clipAt(qint64 start, qint64 len)
{
	TlClip c;
	c.type = TlClip::Type::Image;
	c.srcStartMs = 0;
	c.srcEndMs = len;
	c.outStartMs = start;
	return c;
}

static TlTrack lane(TlTrack::Kind kind, const QString &name, QVector<TlClip> clips = {})
{
	TlTrack t;
	t.kind = kind;
	t.name = name;
	t.clips = std::move(clips);
	return t;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	// The test's server is on this machine; a proxy in the environment must
	// not be asked for it.
	QNetworkProxyFactory::setUseSystemConfiguration(false);
	QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
	QTemporaryDir tmp;

	std::printf("\n-- what the clipboard is read as --\n");
	{
		const QString media = tmp.filePath(QStringLiteral("clip.mp4"));
		const QString notMedia = tmp.filePath(QStringLiteral("notes.txt"));
		QFile(media).open(QIODevice::WriteOnly);
		QFile(notMedia).open(QIODevice::WriteOnly);
		const QImage pic(10, 10, QImage::Format_RGB32);
		const auto read = [](const std::function<void(QMimeData &)> &fill, const QByteArray &native = {}) {
			QMimeData m;
			fill(m);
			return readPaste(&m, native);
		};

		PasteContent c = read([&](QMimeData &m) { m.setUrls({QUrl::fromLocalFile(media)}); });
		ok(c.kind == PasteContent::Kind::Files && c.files == QStringList{media}, "a copied video file: Files");
		c = read([&](QMimeData &m) {
			m.setUrls({QUrl::fromLocalFile(media)});
			m.setImageData(pic);
		});
		ok(c.kind == PasteContent::Kind::Files, "a file with a thumbnail alongside: still the file");
		c = read([&](QMimeData &m) {
			m.setUrls({QUrl::fromLocalFile(notMedia)});
			m.setText(QStringLiteral("notes.txt"));
		});
		ok(c.kind == PasteContent::Kind::Text, "a non-media file: not a file to place (its name is words)");

		c = read([&](QMimeData &m) { m.setImageData(pic); });
		ok(c.kind == PasteContent::Kind::Image && !c.image.isNull(), "a screenshot: Image");
		c = read([&](QMimeData &m) {
			m.setImageData(pic);
			m.setText(QStringLiteral("https://example.com/cat.png"));
		});
		ok(c.kind == PasteContent::Kind::Image, "a picture copied in a browser, with its address: Image");
		c = read([&](QMimeData &m) {
			m.setImageData(pic);
			m.setText(QStringLiteral("Quarterly results, as copied from Word"));
		});
		std::printf("     picture + real words read as %s\n", kindName(c.kind));
		ok(c.kind == PasteContent::Kind::Text, "a picture that came WITH words (Word, Excel): the words");

		c = read([&](QMimeData &m) { m.setData(QStringLiteral("audio/wav"), wav()); });
		ok(c.kind == PasteContent::Kind::Audio && c.audioExt == QLatin1String("wav"), "audio/wav data: Audio");
		c = read([&](QMimeData &m) { m.setData(QStringLiteral("audio/x-wav"), QByteArray("not a wav")); });
		ok(c.kind == PasteContent::Kind::Nothing, "something calling itself a WAV that is not one: Nothing");
		c = read([&](QMimeData &m) { m.setData(QStringLiteral("audio/mpeg"), QByteArray(500, 'x')); });
		ok(c.kind == PasteContent::Kind::Audio && c.audioExt == QLatin1String("mp3"), "audio/mpeg data: an mp3");
		c = read([&](QMimeData &m) { m.setText(QStringLiteral("hi")); }, wav());
		ok(c.kind == PasteContent::Kind::Audio, "Windows' CF_WAVE sound wins over text");

		c = read([&](QMimeData &m) { m.setText(QStringLiteral("  https://youtu.be/abc?t=3 \n")); });
		ok(c.kind == PasteContent::Kind::Link && c.link.host() == QLatin1String("youtu.be"),
		   "one web address (spaces and a newline around it): Link");
		c = read([&](QMimeData &m) { m.setText(QStringLiteral("watch https://youtu.be/abc")); });
		ok(c.kind == PasteContent::Kind::Text, "an address inside a sentence: Text");
		c = read([&](QMimeData &m) { m.setText(QStringLiteral("ftp://example.com/a.mp4")); });
		ok(c.kind == PasteContent::Kind::Text, "not http or https: Text");
		c = read([&](QMimeData &m) { m.setUrls({QUrl(QStringLiteral("https://example.com/v.mp4"))}); });
		ok(c.kind == PasteContent::Kind::Link, "a web address as a URL list, no text: Link");

		c = read([&](QMimeData &m) { m.setText(QStringLiteral("Hello\nworld")); });
		ok(c.kind == PasteContent::Kind::Text && c.text == QLatin1String("Hello\nworld"), "words: Text");
		c = read([&](QMimeData &m) { m.setText(QStringLiteral("  \n\t")); });
		ok(c.kind == PasteContent::Kind::Nothing, "only spaces: Nothing");
		ok(readPaste(nullptr).kind == PasteContent::Kind::Nothing, "no clipboard at all: Nothing");

		ok(extensionForContentType(QStringLiteral("image/jpeg")) == QLatin1String("jpg") &&
			   extensionForContentType(QStringLiteral("video/mp4; codecs=avc1")) == QLatin1String("mp4") &&
			   extensionForContentType(QStringLiteral("audio/mpeg")) == QLatin1String("mp3"),
		   "Content-Type to extension: jpg, mp4 (parameters ignored), mp3");
		ok(extensionForContentType(QStringLiteral("text/html; charset=utf-8")).isEmpty() &&
			   extensionForContentType(QStringLiteral("application/octet-stream")).isEmpty(),
		   "a page, or plain bytes: no extension");
	}

	std::printf("\n-- the best-fit lane --\n");
	{
		using namespace paste_placement;
		TimelineModel m;
		m.tracks = {lane(TlTrack::Kind::Video, QStringLiteral("Subtitles"), {clipAt(0, 60000)}),
			    lane(TlTrack::Kind::Video, QStringLiteral("V2"), {clipAt(0, 5000)}),
			    lane(TlTrack::Kind::Video, QStringLiteral("V1"), {clipAt(20000, 5000)}),
			    lane(TlTrack::Kind::Audio, QStringLiteral("A1"), {clipAt(0, 3000)})};
		ok(bestFitLane(m, TlTrack::Kind::Video, {clipAt(6000, 5000)}) == 1,
		   "the TOP lane, when it is free there");
		ok(bestFitLane(m, TlTrack::Kind::Video, {clipAt(2000, 5000)}) == 2,
		   "busy at the top: the next lane down that is free");
		ok(bestFitLane(m, TlTrack::Kind::Video, {clipAt(2000, 5000), clipAt(7000, 15000)}) == -1,
		   "free for the first clip but not the whole run: no fit");
		ok(bestFitLane(m, TlTrack::Kind::Audio, {clipAt(4000, 2000)}) == 3, "a sound: the audio lane free there");

		TimelineModel n = m;
		// Overlaps V2's clip at the start and V1's at the end.
		const int at = place(n, TlTrack::Kind::Video, {clipAt(3000, 18000)}, Qt::red);
		std::printf("     every picture lane busy: new lane at %d of %d\n", at, int(n.tracks.size()));
		ok(at == 1 && n.tracks.size() == 5 && n.tracks[0].name == QLatin1String("Subtitles"),
		   "none free: a new lane on top, under the Subtitles lane");
		ok(n.tracks[at].autoLane && n.tracks[at].clips.size() == 1, "made for the paste, holding it");
		ok(newLaneIndex(m, TlTrack::Kind::Audio) == 3, "a new audio lane: the top of the audio lanes");

		TimelineModel o = m;
		o.tracks[1].locked = true;
		ok(bestFitLane(o, TlTrack::Kind::Video, {clipAt(6000, 1000)}) == 2, "a locked lane is never written to");
		TimelineModel p;
		p.tracks = {lane(TlTrack::Kind::Video, QStringLiteral("V1"))};
		ok(place(p, TlTrack::Kind::Video, {clipAt(0, 1000), clipAt(1000, 1000)}, Qt::red) == 0 &&
			   p.tracks[0].clips.size() == 2,
		   "several clips: all on the one lane, in order");
	}

	std::printf("\n-- a pasted link --\n");
	{
		TinyHttpServer web;
		const QByteArray pic = png();
		web.routes["/pic.png"] = TinyRoute{200, "image/png", pic, {}, false, -1};
		web.routes["/page"] = TinyRoute{200, "text/html; charset=utf-8", "<html>a video page</html>", {}, false, -1};
		web.routes["/bucket/clip.mp4"] = TinyRoute{200, "application/octet-stream", QByteArray(4000, 'v'), {}, false, -1};
		web.routes["/go"] = TinyRoute{302, "text/html", "moved", web.url("/pic.png").toUtf8(), false, -1};
		web.routes["/slow.mp4"] = TinyRoute{200, "video/mp4", QByteArray(2000, 's'), {}, true, 50000000};
		const QString folder = tmp.filePath(QStringLiteral("pasted"));

		LinkDownload dl;
		QSignalSpy ready(&dl, &LinkDownload::fileReady), page(&dl, &LinkDownload::notMedia),
			failed(&dl, &LinkDownload::failed), progress(&dl, &LinkDownload::progress);
		const auto ended = [&]() { return ready.count() + page.count() + failed.count(); };

		dl.start(QUrl(web.url("/pic.png")), folder);
		settleUntil([&]() { return ended() == 1; });
		ok(ready.count() == 1, "a picture: saved");
		if (ready.count() == 1) {
			const QString path = ready.first().first().toString();
			QFile f(path);
			f.open(QIODevice::ReadOnly);
			std::printf("     -> %s\n", qPrintable(QFileInfo(path).fileName()));
			ok(path.endsWith(QLatin1String("pic.png")) && f.readAll() == pic, "as pic.png, byte for byte");
		}

		dl.start(QUrl(web.url("/pic.png")), folder);
		settleUntil([&]() { return ended() == 2; });
		ok(ready.count() == 2 && ready.last().first().toString().endsWith(QLatin1String("pic-1.png")),
		   "the same link again: a second file, the first left alone");

		dl.start(QUrl(web.url("/bucket/clip.mp4")), folder);
		settleUntil([&]() { return ended() == 3; });
		ok(ready.count() == 3 && ready.last().first().toString().endsWith(QLatin1String("clip.mp4")),
		   "octet-stream from a bucket: a video, by its name");

		dl.start(QUrl(web.url("/go")), folder);
		settleUntil([&]() { return ended() == 4; });
		ok(ready.count() == 4 && ready.last().first().toString().contains(QLatin1String("pic")),
		   "a redirect: followed to the picture");

		const int filesBefore = int(QDir(folder).entryList(QDir::Files).size());
		dl.start(QUrl(web.url("/page")), folder);
		settleUntil([&]() { return ended() == 5; });
		ok(page.count() == 1 && page.first().at(1).toString().startsWith(QLatin1String("text/html")),
		   "a web page: reported as not media, with its type");
		ok(int(QDir(folder).entryList(QDir::Files).size()) == filesBefore, "and nothing saved for it");

		dl.start(QUrl(web.url("/nothing-here.png")), folder);
		settleUntil([&]() { return ended() == 6; });
		ok(failed.count() == 1 && failed.first().first().toString().contains(QLatin1String("404")),
		   "a 404: fails, and says so");
		ok(int(QDir(folder).entryList(QDir::Files).size()) == filesBefore, "and leaves no file");

		dl.start(QUrl(web.url("/slow.mp4")), folder);
		settleUntil([&]() { return progress.count() > 0; });
		const bool wasRunning = dl.running();
		dl.cancel();
		QApplication::processEvents();
		settleUntil([]() { return false; }, 300);
		std::printf("     files before %d, after cancel %d\n", filesBefore,
			    int(QDir(folder).entryList(QDir::Files).size()));
		ok(wasRunning && !dl.running() && ended() == 6, "cancelled mid-download: says nothing more");
		ok(int(QDir(folder).entryList(QDir::Files).size()) == filesBefore, "and no half file is left");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
