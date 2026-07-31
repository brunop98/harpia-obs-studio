// The preview decoder: the GUI thread must never wait on a seek.
//
// Scrubbing a 4K clip froze the editor because showFrame() called
// FrameSeeker::frameAt straight from the GUI thread, and a seek that has to
// roll forward through a long GOP takes hundreds of milliseconds. PreviewDecoder
// answers immediately -- with the frame if it has it, otherwise the nearest one
// it holds -- and decodes behind the caller.
//
// So the claims worth testing are about TIME and about CONVERGENCE, not just
// about pixels:
//   1. a cold request returns far faster than the decode it stands in for;
//   2. the exact frame does arrive, and is the same picture the synchronous
//      decoder produces -- "fast" is worthless if it is also wrong;
//   3. a burst of positions does not decode every one of them (latest-wins);
//   4. the cache stays bounded;
//   5. a miss falls back to the nearest frame rather than to nothing.
//
// The media is deliberately awkward: 1080p with a 250-frame GOP, so a seek near
// the end really does have to decode a long way and the timing claim has
// something to measure.
#include "editor/FrameSeeker.hpp"
#include "editor/PreviewDecoder.hpp"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
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

// Spin the GUI thread's event loop until `pred` holds or the deadline passes.
// Deliberately not QThread::wait: the whole point is that the frames come back
// through the event loop the way they do in the window.
template <typename Pred> static bool pump(Pred pred, int ms = 20000)
{
	QDeadlineTimer dl(ms);
	while (!dl.hasExpired()) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		if (pred())
			return true;
	}
	return pred();
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	const QString work = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();
	const QString media = work + QStringLiteral("/pd_long_gop.mp4");
	if (!QFile::exists(media)) {
		QProcess ff;
		ff.start(QStringLiteral("ffmpeg"),
			 {"-y", "-loglevel", "error", "-f", "lavfi", "-i",
			  "testsrc2=size=1920x1080:rate=30:duration=10", "-c:v", "libx264", "-preset",
			  "ultrafast", "-g", "250", "-pix_fmt", "yuv420p", media});
		ff.waitForFinished(180000);
	}
	if (!QFile::exists(media)) {
		std::printf("  SKIP could not generate test media (is ffmpeg on PATH?)\n");
		return 0;
	}

	const int W = 640, H = 360;
	const qint64 kLate = 9000; // near the end, i.e. a long way past the keyframe

	// How long the blocking decoder takes for the same frame -- the number the
	// asynchronous path has to beat, measured rather than assumed.
	qint64 syncMs = 0;
	QImage syncImg;
	{
		FrameSeeker fs;
		ok(fs.open(media), "the test media opens");
		QElapsedTimer t;
		t.start();
		syncImg = fs.frameAt(kLate, W, H);
		syncMs = t.elapsed();
		std::printf("     blocking FrameSeeker::frameAt(%lldms) took %lld ms\n",
			    (long long)kLate, (long long)syncMs);
		ok(!syncImg.isNull(), "and the blocking decoder produces a frame to compare against");
	}

	PreviewDecoder dec;
	int ready = 0;
	QObject::connect(&dec, &PreviewDecoder::frameReady, &app, [&](int, qint64) { ++ready; });
	dec.setSource(1, media);

	std::printf("\n-- a cold request does not block --\n");
	{
		QElapsedTimer t;
		t.start();
		bool exact = true;
		const QImage img = dec.frame(1, kLate, W, H, &exact);
		const qint64 asyncMs = t.elapsed();
		std::printf("     PreviewDecoder::frame on a cold cache took %lld ms\n",
			    (long long)asyncMs);
		ok(!exact, "it reports that this is not the frame asked for");
		ok(img.isNull(), "and has nothing to stand in with yet");
		// The claim is "returns without decoding", so compare against the decode.
		if (syncMs < 20) {
			std::printf("     NOTE blocking decode was only %lld ms here, so this "
				    "machine cannot discriminate strongly; asserting the weak "
				    "form\n",
				    (long long)syncMs);
			ok(asyncMs <= 5, "the call returned promptly");
		} else {
			ok(asyncMs * 4 < syncMs, "it returned in a small fraction of the decode time");
		}
	}

	std::printf("\n-- and the real frame arrives behind it --\n");
	{
		ok(pump([&] { return dec.has(1, kLate, W, H); }), "the frame turns up");
		bool exact = false;
		const QImage img = dec.frame(1, kLate, W, H, &exact);
		ok(exact, "and is now reported as exact");
		ok(!img.isNull() && img.size() == syncImg.size(), "with the requested size");
		// Same picture as the blocking path: the decode moved threads, it did
		// not change.
		ok(img == syncImg, "and is pixel-for-pixel what the blocking decoder gives");
	}

	std::printf("\n-- a burst of positions is not decoded one by one --\n");
	{
		const int before = ready;
		// Twenty positions as fast as a drag delivers them. Only the last is
		// worth having, and the ones behind it should be dropped rather than
		// queued -- that is what stops a scrub falling further and further
		// behind the cursor.
		qint64 last = 0;
		for (int i = 0; i < 20; ++i) {
			last = 500 + i * 300;
			dec.frame(1, last, W, H);
		}
		ok(pump([&] { return dec.has(1, last, W, H); }), "the last position asked for arrives");
		const int decoded = ready - before;
		std::printf("     20 requests -> %d decodes\n", decoded);
		ok(decoded < 20, "the ones overtaken were dropped, not queued");
		// The negative control for that number: twenty positions requested one
		// at a time, each awaited, must decode about twenty times. If this did
		// not rise, the count above would be meaningless.
		const int before2 = ready;
		for (int i = 0; i < 6; ++i) {
			const qint64 ms = 200 + i * 411;
			dec.frame(1, ms, W, H);
			pump([&] { return dec.has(1, ms, W, H); });
		}
		const int serial = ready - before2;
		std::printf("     6 awaited requests -> %d decodes (control)\n", serial);
		ok(serial >= 5, "CONTROL: awaited requests are each decoded");
	}

	std::printf("\n-- the cache is bounded, and a miss falls back to the nearest --\n");
	{
		// More distinct positions than the cache can hold, each awaited so each
		// is really stored.
		QVector<qint64> pos;
		for (int i = 0; i < PreviewDecoder::kCachePerSource + 3; ++i) {
			const qint64 ms = 1000 + i * 700;
			pos.push_back(ms);
			dec.frame(1, ms, W, H);
			pump([&] { return dec.has(1, ms, W, H); });
		}
		int held = 0;
		for (qint64 ms : pos)
			if (dec.has(1, ms, W, H))
				++held;
		std::printf("     %d positions requested, %d still cached (limit %d)\n", int(pos.size()),
			    held, PreviewDecoder::kCachePerSource);
		ok(held <= PreviewDecoder::kCachePerSource, "no more than the limit is kept");
		ok(held > 0, "CONTROL: and it is not simply keeping nothing");

		// A position nowhere near anything cached: answered with the closest
		// frame it does have, so the preview shows something rather than
		// blanking while the worker catches up.
		bool exact = true;
		const QImage near = dec.frame(1, 4321, W, H, &exact);
		ok(!exact, "an uncached position is not claimed to be exact");
		ok(!near.isNull(), "but a nearby frame stands in for it");
	}

	std::printf("\n-- degenerate requests --\n");
	{
		bool exact = true;
		ok(dec.frame(99, 1000, W, H, &exact).isNull(), "an unknown source has no frame");
		ok(!exact, "and does not claim one");
		ok(dec.frame(1, -1, W, H).isNull(), "nor does a negative timestamp");
		ok(dec.frame(1, 1000, 0, 0).isNull(), "nor a zero-sized preview");
		dec.removeSource(1);
		ok(dec.frame(1, 1000, W, H).isNull(), "a removed source is forgotten");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
