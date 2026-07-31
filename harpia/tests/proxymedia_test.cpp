// Editing proxies: a smaller, short-GOP stand-in the preview decodes from.
//
// The claim is narrow and it is the whole point: seeking the proxy is much
// cheaper than seeking the original, AND the proxy shows the same thing at the
// same time. A fast proxy that is a few frames out of step would be worse than
// none, because every cut you place against the preview would be wrong.
//
// So there are three separate things to check, and the third is the one that
// would quietly ruin an edit:
//   1. which files get a proxy at all (a proxy for an ordinary recording is a
//      transcode that buys nothing);
//   2. the cache key -- same file, same path; changed file, different path;
//   3. the proxy is faster to seek, has the same duration, and shows the same
//      content at the same timestamp.
//
// The media is a colour that changes every second, so "the same content at the
// same timestamp" is something a test can actually read off a frame.
#include "editor/FrameSeeker.hpp"
#include "editor/PreviewDecoder.hpp"
#include "editor/ProxyMedia.hpp"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

template <typename Pred> static bool pump(Pred pred, int ms = 300000)
{
	QDeadlineTimer dl(ms);
	while (!dl.hasExpired()) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		if (pred())
			return true;
	}
	return pred();
}

// The dominant hue of a frame, as a crude label: which of R/G/B is largest.
static char channelOf(const QImage &img)
{
	if (img.isNull())
		return '?';
	qint64 r = 0, g = 0, b = 0;
	for (int y = img.height() / 4; y < img.height() * 3 / 4; y += 4)
		for (int x = img.width() / 4; x < img.width() * 3 / 4; x += 4) {
			const QColor c = img.pixelColor(x, y);
			r += c.red();
			g += c.green();
			b += c.blue();
		}
	if (r >= g && r >= b)
		return 'R';
	return g >= b ? 'G' : 'B';
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	std::printf("\n-- which files are worth proxying --\n");
	{
		// Above 1080p, whatever the codec: the decode itself is the problem.
		ok(wantsProxy(3840, 2160, "h264"), "4K H.264 gets one");
		ok(wantsProxy(3840, 2160, "vp9"), "as does 4K VP9");
		ok(wantsProxy(2560, 1440, "h264"), "and 1440p");
		// At 1080p and below, only the codecs that are slow to seek.
		ok(wantsProxy(1920, 1080, "vp9"), "1080p VP9 gets one -- it seeks badly");
		ok(wantsProxy(1920, 1080, "av1"), "so does 1080p AV1");
		ok(!wantsProxy(1920, 1080, "h264"),
		   "but an ordinary 1080p H.264 recording does not");
		ok(!wantsProxy(1280, 720, "vp9"), "nor 720p VP9, which is already small");
		ok(!wantsProxy(0, 0, "h264"), "nor a file whose size is unknown");
	}

	const QString work = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
	const QString src = work + QStringLiteral("/proxy_src.webm");
	if (!QFile::exists(src)) {
		// 1440p VP9 with a four-second GOP: big enough and slow enough to seek
		// that the comparison below means something, and small enough to encode
		// here. Red, then green, then blue, a second each.
		QProcess ff;
		ff.start(QStringLiteral("ffmpeg"),
			 {"-y", "-loglevel", "error", "-f", "lavfi", "-i",
			  "color=c=red:s=2560x1440:r=30:d=2", "-f", "lavfi", "-i",
			  "color=c=green:s=2560x1440:r=30:d=2", "-f", "lavfi", "-i",
			  "color=c=blue:s=2560x1440:r=30:d=2", "-filter_complex",
			  "[0:v][1:v][2:v]concat=n=3:v=1:a=0[v]", "-map", "[v]", "-c:v", "libvpx-vp9",
			  "-deadline", "realtime", "-cpu-used", "8", "-b:v", "3M", "-g", "120", src});
		ff.waitForFinished(600000);
	}
	if (!QFile::exists(src)) {
		std::printf("  SKIP could not generate test media (is ffmpeg on PATH?)\n");
		return failures ? 1 : 0;
	}

	std::printf("\n-- the source is one this rule would proxy --\n");
	{
		int w = 0, h = 0;
		QString codec;
		ok(probeVideo(src, &w, &h, &codec), "the file probes");
		std::printf("     %dx%d %s\n", w, h, qUtf8Printable(codec));
		ok(wantsProxy(w, h, codec), "and the rule agrees it needs a proxy");
	}

	std::printf("\n-- the cache key --\n");
	{
		const QString a = proxyPathFor(src);
		const QString b = proxyPathFor(src);
		ok(a == b, "the same file always maps to the same proxy path");
		ok(a.startsWith(proxyCacheDir()), "which is inside the proxy cache folder");
		// A different file must not share it. Same directory, different name and
		// contents -- if the key were only the size, or only the name, this
		// would collide and one video would be previewed as another.
		const QString other = work + QStringLiteral("/proxy_src_other.webm");
		QFile::remove(other);
		QFile::copy(src, other);
		ok(proxyPathFor(other) != a, "a different file maps somewhere else");
		QFile::remove(other);
	}

	// Clear any proxy left by an earlier run, or "built it" would be untested.
	QFile::remove(proxyPathFor(src));

	ProxyBuilder builder;
	QString gotPath;
	int gotId = -1;
	int lastPct = -1;
	QObject::connect(&builder, &ProxyBuilder::ready, &app, [&](int id, const QString &p) {
		gotId = id;
		gotPath = p;
	});
	QObject::connect(&builder, &ProxyBuilder::progress, &app,
			 [&](int, int pct) { lastPct = pct; });

	std::printf("\n-- building it --\n");
	{
		builder.request(7, src);
		ok(pump([&] { return !gotPath.isEmpty(); }), "the proxy is built");
		ok(gotId == 7, "and reported against the source that asked for it");
		ok(QFileInfo::exists(gotPath), "the file is really there");
		ok(gotPath == proxyPathFor(src), "at the cache path");
		ok(lastPct >= 0, "progress was reported while it built");
		const qint64 sBytes = QFileInfo(src).size();
		const qint64 pBytes = QFileInfo(gotPath).size();
		std::printf("     source %lld KB -> proxy %lld KB\n", (long long)(sBytes / 1024),
			    (long long)(pBytes / 1024));
		// No claim that a proxy is always smaller -- a short GOP costs bits, and
		// against a heavily-compressed source it can lose. Only that it is not
		// absurd, which would mean the encode went wrong.
		ok(pBytes > 0, "and it is not empty");
	}

	std::printf("\n-- the proxy lines up with the original --\n");
	{
		FrameSeeker so, sp;
		ok(so.open(src), "the original opens");
		ok(sp.open(gotPath), "and so does the proxy");
		std::printf("     original %dx%d %s, proxy %dx%d %s\n", so.width(), so.height(),
			    qUtf8Printable(so.codecName()), sp.width(), sp.height(),
			    qUtf8Printable(sp.codecName()));
		ok(sp.height() <= 540, "the proxy is reduced to preview size");
		ok(sp.width() < so.width(), "and is smaller than the original");
		// Same length: a proxy that ran short would make the tail of the clip
		// unreachable in the preview.
		const qint64 d = std::llabs(sp.durationMs() - so.durationMs());
		std::printf("     duration original %lld ms, proxy %lld ms (diff %lld)\n",
			    (long long)so.durationMs(), (long long)sp.durationMs(), (long long)d);
		ok(d <= 100, "the durations agree");

		// THE claim: the same timestamp shows the same thing. Red / green / blue,
		// one second each, sampled in the middle of each so a frame either way
		// cannot flip the answer.
		bool aligned = true;
		for (auto pr : {std::pair<qint64, char>{500, 'R'}, {2500, 'G'}, {4500, 'B'}}) {
			const char co = channelOf(so.frameAt(pr.first, 480, 270));
			const char cp = channelOf(sp.frameAt(pr.first, 480, 270));
			std::printf("     %lld ms: original %c, proxy %c (want %c)\n",
				    (long long)pr.first, co, cp, pr.second);
			if (co != pr.second || cp != pr.second)
				aligned = false;
		}
		ok(aligned, "every sampled timestamp shows the same content in both");
	}

	std::printf("\n-- and it is safe for PLAYBACK to read, not just scrubbing --\n");
	{
		// Playback pulls frames sequentially from the source's own FrameSeeker
		// and paces itself by its rate, so pointing that at the proxy only works
		// if the rate matches and the frames come out in the same order at the
		// same times. A proxy at a different frame rate would play at the wrong
		// speed while looking perfectly fine frame by frame.
		FrameSeeker so, sp;
		so.open(src);
		sp.open(gotPath);
		std::printf("     fps original %.3f, proxy %.3f\n", so.fps(), sp.fps());
		ok(std::abs(so.fps() - sp.fps()) < 0.01, "the frame rates agree");

		// Walk the proxy sequentially from the green section into the blue one
		// and check the timestamps advance and the colour changes where it
		// should -- that is the whole of what playback needs from it.
		ok(sp.seekTo(2500), "the proxy seeks for sequential play");
		qint64 ts = -1;
		int frames = 0;
		char atGreen = '?', atBlue = '?';
		qint64 lastTs = -1;
		bool monotonic = true;
		while (frames < 90) {
			const QImage f = sp.nextFrame(&ts, 480, 270);
			if (f.isNull())
				break;
			if (lastTs >= 0 && ts < lastTs)
				monotonic = false;
			lastTs = ts;
			if (atGreen == '?' && ts >= 2500)
				atGreen = channelOf(f);
			if (ts >= 4500) {
				atBlue = channelOf(f);
				break;
			}
			++frames;
		}
		std::printf("     played %d frames to %lld ms: %c then %c\n", frames, (long long)ts,
			    atGreen, atBlue);
		ok(monotonic, "timestamps only move forward");
		ok(atGreen == 'G' && atBlue == 'B', "and the colours change where the original does");
	}

	std::printf("\n-- and it is the point: seeking is cheaper --\n");
	{
		// Backwards through the clip, which is the worst case: every step
		// re-seeks to a keyframe and rolls forward.
		const QVector<qint64> marks = {5500, 4700, 3900, 3100, 2300, 1500, 700};
		auto timeSeeks = [&](const QString &path) {
			FrameSeeker fs;
			if (!fs.open(path))
				return qint64(-1);
			fs.frameAt(marks.front(), 480, 270); // warm the decoder, not the clock
			QElapsedTimer t;
			t.start();
			for (qint64 m : marks)
				fs.frameAt(m, 480, 270);
			return t.elapsed();
		};
		const qint64 origMs = timeSeeks(src);
		const qint64 proxMs = timeSeeks(gotPath);
		std::printf("     %d backward seeks: original %lld ms, proxy %lld ms\n",
			    int(marks.size()), (long long)origMs, (long long)proxMs);
		ok(proxMs >= 0 && origMs > 0, "both were measurable");
		ok(proxMs * 2 < origMs, "the proxy seeks at least twice as fast");
	}

	std::printf("\n-- and the filmstrip can be built from it --\n");
	{
		// The filmstrip is sixty evenly-spaced thumbnails, and building it from
		// the original is sixty full-size seeks on a background thread while you
		// are trying to work. Building it from the proxy is only correct if the
		// thumbnails show the same frames -- a filmstrip that disagrees with the
		// video is worse than a slow one.
		FrameSeeker fo, fp;
		fo.open(src, true); // the fast mode TimelineThumbs uses
		fp.open(gotPath, true);
		int agree = 0;
		const int N = 30;
		for (int i = 0; i < N; ++i) {
			const qint64 ms = qint64((i + 0.5) * double(fo.durationMs()) / N);
			if (channelOf(fo.frameAt(ms, 128, 72)) == channelOf(fp.frameAt(ms, 128, 72)))
				++agree;
		}
		std::printf("     %d of %d thumbnails match the original\n", agree, N);
		// Not all N: the fast mode can land a frame or two off, and two of these
		// sit right on a colour change. The claim is that the strip describes the
		// video, not that it is frame-exact -- which the fast mode never was.
		ok(agree >= N - 2, "the strip from the proxy shows what the original shows");
	}

	std::printf("\n-- and the preview decoder switches to it --\n");
	{
		PreviewDecoder dec;
		dec.setSource(7, src);
		ok(dec.decodePathFor(7) == src, "before the proxy, the original is read");
		dec.setProxy(7, gotPath);
		ok(dec.decodePathFor(7) == gotPath, "after it, the proxy is");
		// A frame through the decoder, from the proxy, still says the right thing.
		bool exact = false;
		dec.frame(7, 2500, 480, 270, &exact);
		ok(pump([&] { return dec.has(7, 2500, 480, 270); }, 30000),
		   "a frame comes back through the decoder");
		const QImage img = dec.frame(7, 2500, 480, 270, &exact);
		ok(exact && channelOf(img) == 'G', "and it is the frame that belongs at 2500 ms");
		// Going back to the original must drop what the proxy put in the cache;
		// they are different encodes of the same picture, not the same bytes.
		dec.setProxy(7, QString());
		ok(dec.decodePathFor(7) == src, "clearing the proxy goes back to the original");
		ok(!dec.has(7, 2500, 480, 270), "and the proxy's cached frames are dropped");
	}

	std::printf("\n-- a second request is served from the cache --\n");
	{
		// The proxy already exists now, so this must come back without building
		// anything -- that is what makes reopening a project cheap.
		QString again;
		ProxyBuilder b2;
		QObject::connect(&b2, &ProxyBuilder::ready, &app,
				 [&](int, const QString &p) { again = p; });
		QElapsedTimer t;
		t.start();
		b2.request(1, src);
		ok(pump([&] { return !again.isEmpty(); }, 20000), "it is returned");
		std::printf("     took %lld ms\n", (long long)t.elapsed());
		ok(again == gotPath, "and it is the same file, not a rebuild");
		ok(t.elapsed() < 2000, "returned without transcoding again");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
