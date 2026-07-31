// Multi-Cut output preview: a cut must stop at its end handle.
//
// Playing the assembled Output, the last cut ran past its end handle and kept
// going into the rest of the source file. The cause is a contract nobody was
// honouring: FrameSeeker::nextFrameAt always advances AT LEAST ONE FRAME -- it
// cannot show you the frame you are already on. So when the output clock has
// not yet moved past the frame on screen, asking again advances the PICTURE
// without advancing the TIMELINE, and since the picture is what you see, it
// walks straight out of the cut.
//
// It bites whenever a frame lasts longer than a preview tick:
//   * a 15 fps source against the 30 fps timer -- two frames of source per tick
//     of output, so a one-second cut played two seconds of footage;
//   * any cut slowed below 1x, for the same reason.
// A 30 fps source at 1x is exactly matched and never showed it, which is why it
// survived: the obvious thing to test is the one case that works.
//
// So the test drives the real tick loop -- TrackEditor's own segment/output
// mapping and a real decoder -- over both frame rates and several cut shapes,
// and asserts no frame shown for a cut comes from past that cut's end.
#include "editor/FrameSeeker.hpp"
#include "editor/TrackEditor.hpp"

#include <QApplication>
#include <QDir>
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

struct Result {
	qint64 worstOver = 0; // ms past a cut's end handle
	int shown = 0;
	int loops = 0;
};

// One run of the playback loop, transcribed from VideoEditorWindow::onPlayTick.
// `hold` selects the fix: skip the decode when the clock has not reached the
// next frame. false reproduces what shipped.
static Result play(FrameSeeker &fs, TrackEditor &te, const QVector<CutSegment> &segs, bool hold)
{
	Result r;
	const qint64 total = te.totalOutputMs();
	qint64 playAnchor = 0;
	int playSeg = -1;
	// A simulated wall clock, so the run is deterministic rather than depending
	// on how fast this machine decodes.
	for (qint64 wall = 0; wall < total * 3 && r.loops < 2; wall += 33) {
		qint64 outPos = playAnchor + wall;
		if (outPos >= total) {
			playAnchor = -wall;
			++r.loops;
			playSeg = -1;
			outPos = 0;
		}
		qint64 srcTarget = 0;
		const int seg = te.sourceForOutput(outPos, &srcTarget);
		if (seg < 0)
			break;
		if (seg != playSeg) {
			fs.seekTo(segs[seg].srcStartMs);
			playSeg = seg;
		}
		if (hold && fs.positionMs() >= 0 && fs.positionMs() >= srcTarget)
			continue; // the clock has not reached the next frame yet
		qint64 ts = -1;
		if (fs.nextFrameAt(srcTarget, &ts, 320, 180, 240).isNull()) {
			playAnchor = te.outputStartOf(seg) + segs[seg].outDurationMs() - wall;
			playSeg = -1;
			continue;
		}
		++r.shown;
		r.worstOver = std::max(r.worstOver, ts - segs[seg].srcEndMs);
	}
	return r;
}

static QString makeMedia(const QString &work, int fps)
{
	const QString path = work + QStringLiteral("/mcplay_%1fps.mp4").arg(fps);
	if (!QFile::exists(path)) {
		QProcess ff;
		ff.start(QStringLiteral("ffmpeg"),
			 {"-y", "-loglevel", "error", "-f", "lavfi", "-i",
			  QStringLiteral("testsrc2=size=320x180:rate=%1:duration=8").arg(fps), "-c:v",
			  "libx264", "-preset", "ultrafast", "-g", "12", "-pix_fmt", "yuv420p", path});
		ff.waitForFinished(120000);
	}
	return QFile::exists(path) ? path : QString();
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	const QString work = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath();

	struct Shape {
		const char *name;
		QVector<CutSegment> segs;
	};
	const QVector<Shape> shapes = {
		{"two cuts", {{1000, 2000, 1.0, 1}, {4000, 5000, 1.0, 1}}},
		{"one cut, capped mid-file", {{4000, 5000, 1.0, 1}}},
		{"one cut at half speed", {{4000, 5000, 0.5, 1}}},
		{"one cut at double speed", {{4000, 5000, 2.0, 1}}},
		{"a cut from the very start", {{0, 1500, 1.0, 1}}},
	};

	for (int fps : {30, 15}) {
		const QString media = makeMedia(work, fps);
		if (media.isEmpty()) {
			std::printf("  SKIP could not generate %d fps media (is ffmpeg on PATH?)\n", fps);
			continue;
		}
		const qint64 frameMs = 1000 / fps;
		std::printf("\n-- %d fps source (a frame lasts %lld ms; the preview ticks every 33) --\n",
			    fps, (long long)frameMs);
		for (const Shape &sh : shapes) {
			FrameSeeker fs;
			if (!fs.open(media)) {
				ok(false, "the media opens");
				continue;
			}
			TrackEditor te;
			te.setDuration(fs.durationMs());
			te.setSegments(sh.segs);

			const Result fixed = play(fs, te, sh.segs, true);
			// At most one frame past the end: nextFrameAt returns the frame at
			// or AFTER the target, so the frame straddling a cut's last instant
			// is the right one to show. A whole frame is the floor of what any
			// forward-only decoder can promise.
			const bool held = fixed.worstOver <= frameMs;
			std::printf("     %-28s overrun %4lld ms (tolerance %lld)\n", sh.name,
				    (long long)fixed.worstOver, (long long)frameMs);
			ok(held, sh.name);
			ok(fixed.shown > 0, "  and it actually played something");
		}
	}

	std::printf("\n-- CONTROL: without the hold, it runs out of the cut --\n");
	{
		// Without this the section above proves nothing: it has to be shown that
		// these inputs can tell the fixed loop from the broken one.
		const QString media = makeMedia(work, 15);
		if (media.isEmpty()) {
			std::printf("  SKIP no media\n");
		} else {
			const QVector<CutSegment> segs = {{4000, 5000, 1.0, 1}};
			FrameSeeker fs;
			fs.open(media);
			TrackEditor te;
			te.setDuration(fs.durationMs());
			te.setSegments(segs);
			const Result broken = play(fs, te, segs, false);
			std::printf("     15 fps, a one-second cut: the old loop overran by %lld ms\n",
				    (long long)broken.worstOver);
			ok(broken.worstOver > 500,
			   "CONTROL: the shipped loop played most of a second past the end handle");
		}
	}
	{
		// And the case that always worked, so the control above is not just
		// "this test can fail somehow": a matched frame rate at 1x was fine
		// before the fix too, which is exactly why the bug survived.
		const QString media = makeMedia(work, 30);
		if (!media.isEmpty()) {
			const QVector<CutSegment> segs = {{4000, 5000, 1.0, 1}};
			FrameSeeker fs;
			fs.open(media);
			TrackEditor te;
			te.setDuration(fs.durationMs());
			te.setSegments(segs);
			const Result broken = play(fs, te, segs, false);
			std::printf("     30 fps at 1x, old loop: overran by %lld ms\n",
				    (long long)broken.worstOver);
			ok(broken.worstOver <= 33,
			   "CONTROL: the matched case was always fine -- the bug needed a mismatch");
		}
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
