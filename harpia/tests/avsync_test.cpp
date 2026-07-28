// Does the exported timeline's AUDIO line up with its VIDEO?
//
// This is the thing that has never been checked. tlaudio_test covers the mix
// buffer the preview plays; nothing covered the file the exporter actually
// writes, and nothing at all compared the two streams against each other.
//
// The trick that makes it checkable without listening: every source is a solid
// colour AND a distinct sine tone. The colour says which clip a video frame
// came from; the dominant frequency says which clip a slice of audio came from.
// If they disagree at the same output timestamp, the export is out of sync.
#include "editor/ClipExporter.hpp"
#include "editor/TimelineAudio.hpp"
#include "editor/timeline/TimelineModel.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace harpia;

static int failures = 0;
static void ok(bool cond, const char *what)
{
	std::printf("  %s %s\n", cond ? "PASS" : "FAIL", what);
	if (!cond)
		++failures;
}

// ---- the three sources ------------------------------------------------------
struct Src {
	const char *path;
	QColor colour;
	double hz;
};
// Where run_avsync.sh put the generated clips.
static QString g_mediaDir = QStringLiteral(".");
static QString mediaPath(const char *name)
{
	return g_mediaDir + QLatin1Char('/') + QString::fromLatin1(name);
}

static const Src kRed = {"av_red.mp4", QColor(0xC0, 0x00, 0x00), 300.0};
static const Src kGreen = {"av_green.mp4", QColor(0x00, 0xA0, 0x00), 700.0};
static const Src kBlue = {"av_blue.mp4", QColor(0x00, 0x00, 0xC0), 1500.0};

// ---- decoding the result, via ffmpeg (an INDEPENDENT reader) ----------------
// Deliberately not FrameSeeker: checking our decoder against our encoder would
// let a shared misunderstanding pass. ffmpeg is the outside opinion.

// The average colour of the output frame at `ms`.
static QColor frameColourAt(const QString &file, qint64 ms)
{
	QProcess p;
	p.start(QStringLiteral("ffmpeg"),
		{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-ss"),
		 QString::number(ms / 1000.0, 'f', 3), QStringLiteral("-i"), file,
		 QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-vf"),
		 QStringLiteral("scale=1:1"), QStringLiteral("-f"), QStringLiteral("rawvideo"),
		 QStringLiteral("-pix_fmt"), QStringLiteral("rgb24"), QStringLiteral("-")});
	p.waitForFinished(20000);
	const QByteArray out = p.readAllStandardOutput();
	if (out.size() < 3)
		return QColor();
	return QColor(uchar(out[0]), uchar(out[1]), uchar(out[2]));
}

// The output's whole audio track as mono float, 48 kHz.
static std::vector<float> decodeAudio(const QString &file)
{
	QProcess p;
	p.start(QStringLiteral("ffmpeg"),
		{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"), file,
		 QStringLiteral("-ac"), QStringLiteral("1"), QStringLiteral("-ar"),
		 QStringLiteral("48000"), QStringLiteral("-f"), QStringLiteral("f32le"),
		 QStringLiteral("-")});
	p.waitForFinished(60000);
	const QByteArray raw = p.readAllStandardOutput();
	std::vector<float> out(size_t(raw.size() / 4));
	std::memcpy(out.data(), raw.constData(), out.size() * 4);
	return out;
}

// Which of the three tones is loudest in a 200 ms window at `ms`? A Goertzel
// filter per candidate — cheaper than an FFT and enough when the candidates are
// known. Returns the frequency, or 0 when the window is essentially silent.
static double dominantHz(const std::vector<float> &mono, qint64 ms, double *rmsOut = nullptr,
			 double windowSec = 0.2)
{
	const int rate = 48000;
	const size_t start = size_t(ms * rate / 1000);
	const size_t n = size_t(windowSec * rate);
	if (start + n > mono.size())
		return -1.0;

	double sum2 = 0.0;
	for (size_t i = 0; i < n; ++i)
		sum2 += double(mono[start + i]) * mono[start + i];
	const double rms = std::sqrt(sum2 / n);
	if (rmsOut)
		*rmsOut = rms;
	if (rms < 0.01)
		return 0.0; // silence

	double best = 0.0, bestMag = 0.0;
	for (double hz : {300.0, 700.0, 1500.0}) {
		const double w = 2.0 * M_PI * hz / rate;
		const double coeff = 2.0 * std::cos(w);
		double s0 = 0, s1 = 0, s2 = 0;
		for (size_t i = 0; i < n; ++i) {
			s0 = mono[start + i] + coeff * s1 - s2;
			s2 = s1;
			s1 = s0;
		}
		const double mag = std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2);
		if (mag > bestMag) {
			bestMag = mag;
			best = hz;
		}
	}
	return best;
}

static bool colourIs(const QColor &got, const QColor &want, int tol = 60)
{
	return got.isValid() && std::abs(got.red() - want.red()) <= tol &&
	       std::abs(got.green() - want.green()) <= tol &&
	       std::abs(got.blue() - want.blue()) <= tol;
}

static TlClip mk(int src, qint64 outStart, qint64 srcStart, qint64 srcEnd)
{
	TlClip c;
	c.type = TlClip::Type::Video;
	c.sourceId = src;
	c.srcStartMs = srcStart;
	c.srcEndMs = srcEnd;
	c.outStartMs = outStart;
	return c;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	// The three source clips are generated by run_avsync.sh; point at them.
	if (argc > 1)
		g_mediaDir = QString::fromLocal8Bit(argv[1]);

	for (const Src *s : {&kRed, &kGreen, &kBlue})
		if (!QFile::exists(mediaPath(s->path))) {
			std::printf("missing %s\n", s->path);
			return 2;
		}

	// Three clips back to back on one track, each trimmed from a different part
	// of its source so a naive "start from zero" bug shows up as the wrong tone
	// arriving early.
	//   0.0 - 1.5 s   red   (300 Hz), from 0.5 s into the source
	//   1.5 - 3.0 s   green (700 Hz), from 1.0 s in
	//   3.0 - 4.5 s   blue  (1500 Hz), from 0.0 s in
	TimelineModel m;
	TlTrack t;
	t.kind = TlTrack::Kind::Video;
	t.name = QStringLiteral("V1");
	t.clips.append(mk(1, 0, 500, 2000));
	t.clips.append(mk(2, 1500, 1000, 2500));
	t.clips.append(mk(3, 3000, 0, 1500));
	m.tracks.append(t);

	std::printf("timeline: %lld ms, %d clips\n", (long long)m.durationMs(), t.clips.size());

	// ---- 1. the buffer the PREVIEW would play --------------------------
	std::printf("\n-- the preview mix --\n");
	auto lookup = [](int id) -> QString {
		switch (id) {
		case 1: return mediaPath(kRed.path);
		case 2: return mediaPath(kGreen.path);
		case 3: return mediaPath(kBlue.path);
		}
		return QString();
	};
	const std::vector<float> mix = TimelineAudio::mixToBuffer(m, lookup);
	ok(!mix.empty(), "the timeline mixes to a non-empty buffer");
	if (mix.empty())
		return 1;
	// mixToBuffer is interleaved stereo; fold to mono for the analysis.
	std::vector<float> previewMono(mix.size() / 2);
	for (size_t i = 0; i < previewMono.size(); ++i)
		previewMono[i] = 0.5f * (mix[i * 2] + mix[i * 2 + 1]);
	std::printf("     %.3f s of audio\n", double(previewMono.size()) / 48000.0);

	struct Probe {
		qint64 ms;
		const Src *src;
		const char *what;
	};
	const Probe probes[] = {
		{300, &kRed, "0.3 s is red's tone"},
		{1200, &kRed, "1.2 s is still red"},
		{1800, &kGreen, "1.8 s has switched to green"},
		{2700, &kGreen, "2.7 s is still green"},
		{3300, &kBlue, "3.3 s has switched to blue"},
		{4200, &kBlue, "4.2 s is still blue"},
	};
	for (const Probe &pr : probes) {
		double rms = 0;
		const double hz = dominantHz(previewMono, pr.ms, &rms);
		const bool good = std::abs(hz - pr.src->hz) < 1.0;
		std::printf("  %s %s (%.0f Hz, rms %.3f)\n", good ? "PASS" : "FAIL", pr.what, hz, rms);
		if (!good)
			++failures;
	}

	// ---- 2. export it for real -----------------------------------------
	std::printf("\n-- exporting --\n");
	QTemporaryDir dir;
	const QString outFile = dir.path() + QStringLiteral("/out.mp4");

	ClipExporter::Options o;
	o.format = ClipExporter::Format::Mp4;
	o.keepAudio = true;
	o.timeline = m;
	o.timelineSources = {{1, mediaPath(kRed.path).toStdString()},
			     {2, mediaPath(kGreen.path).toStdString()},
			     {3, mediaPath(kBlue.path).toStdString()}};
	o.canvasW = 320;
	o.canvasH = 180;
	o.timelineFps = 30.0;

	ClipExporter ex;
	bool done = false, exportOk = false;
	QString err;
	QObject::connect(&ex, &ClipExporter::finished,
			 [&](bool good, bool cancelled, const QString &e) {
				 done = true;
				 exportOk = good && !cancelled;
				 err = e;
			 });
	ex.run(mediaPath(kRed.path), outFile, o);
	// The exporter runs on its own thread and reports back through the event
	// loop, so the loop has to actually turn.
	QElapsedTimer wall;
	wall.start();
	while (!done && wall.elapsed() < 180000) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
		QThread::msleep(20);
	}
	ok(done, "the export finished");
	ok(exportOk, "and reported success");
	if (!exportOk) {
		std::printf("     error: %s\n", qPrintable(err));
		return 1;
	}
	ok(QFile(outFile).size() > 1000, "the file has content");

	// ---- 3. does the FILE agree with itself? ---------------------------
	std::printf("\n-- the exported file: colour vs tone at the same instant --\n");
	const std::vector<float> outMono = decodeAudio(outFile);
	ok(!outMono.empty(), "the export has an audio track at all");
	if (outMono.empty()) {
		std::printf("     (nothing to compare against the video)\n");
		return 1;
	}
	std::printf("     %.3f s of audio in the file\n", double(outMono.size()) / 48000.0);

	for (const Probe &pr : probes) {
		const QColor c = frameColourAt(outFile, pr.ms);
		double rms = 0;
		const double hz = dominantHz(outMono, pr.ms, &rms);
		const bool vOk = colourIs(c, pr.src->colour);
		const bool aOk = std::abs(hz - pr.src->hz) < 1.0;
		std::printf("  %s %-32s  video rgb(%3d,%3d,%3d) %s   audio %4.0f Hz %s\n",
			    (vOk && aOk) ? "PASS" : "FAIL", pr.what, c.red(), c.green(), c.blue(),
			    vOk ? "ok" : "WRONG", hz, aOk ? "ok" : "WRONG");
		if (!vOk || !aOk)
			++failures;
	}

	// ---- 4. how far apart are the two streams, really? -----------------
	//
	// The measurement has to compare the audio against the VIDEO, not against
	// the preview mix. Both of those come from the same buildTakes(), so a
	// placement bug shifts them together and comparing them sees nothing -- an
	// earlier version of this test did exactly that and passed a deliberate
	// 300 ms offset. The video comes from TimelineCompositor, which is a
	// genuinely independent path, so the two disagreeing IS the sync error.
	//
	// A short window, because this is looking for an edge rather than
	// identifying a steady tone: 40 ms is a dozen cycles even at 300 Hz.
	std::printf("\n-- measured A/V sync at each cut --\n");
	auto audioCutNear = [&](const std::vector<float> &mono, qint64 aroundMs,
				double toHz) -> qint64 {
		for (qint64 ms = std::max<qint64>(0, aroundMs - 600); ms <= aroundMs + 600; ms += 5)
			if (std::abs(dominantHz(mono, ms, nullptr, 0.04) - toHz) < 1.0)
				return ms;
		return -1;
	};
	auto videoCutNear = [&](qint64 aroundMs, const QColor &to) -> qint64 {
		for (qint64 ms = std::max<qint64>(0, aroundMs - 600); ms <= aroundMs + 600; ms += 10)
			if (colourIs(frameColourAt(outFile, ms), to))
				return ms;
		return -1;
	};
	struct Cut {
		qint64 at;
		const Src *to;
	};
	for (const Cut &cut : {Cut{1500, &kGreen}, Cut{3000, &kBlue}}) {
		const qint64 aCut = audioCutNear(outMono, cut.at, cut.to->hz);
		const qint64 vCut = videoCutNear(cut.at, cut.to->colour);
		const qint64 skew = (aCut >= 0 && vCut >= 0) ? (aCut - vCut) : 99999;
		// One video frame at 30 fps is 33 ms; the audio probe steps in 5 ms and
		// the video probe in 10, so anything inside 50 ms is measurement noise
		// and anything outside it is a real offset.
		const bool good = std::abs(skew) <= 50;
		std::printf("  %s cut at %lld ms: video at %lld, audio at %lld, skew %+lld ms\n",
			    good ? "PASS" : "FAIL", (long long)cut.at, (long long)vCut,
			    (long long)aCut, (long long)skew);
		if (!good)
			++failures;
		// And the cut must be where the TIMELINE said, not merely
		// self-consistent: two streams that agree but both land a second late
		// are still wrong.
		const bool onTime = vCut >= 0 && std::abs(vCut - cut.at) <= 50;
		std::printf("  %s     and it is where the timeline put it\n", onTime ? "PASS" : "FAIL");
		if (!onTime)
			++failures;
	}

	// The preview mix must agree with the file as well -- that is the
	// shared-description guarantee, and it is a different claim from sync.
	std::printf("\n-- preview mix vs exported file --\n");
	for (const Cut &cut : {Cut{1500, &kGreen}, Cut{3000, &kBlue}}) {
		const qint64 inPreview = audioCutNear(previewMono, cut.at, cut.to->hz);
		const qint64 inExport = audioCutNear(outMono, cut.at, cut.to->hz);
		const qint64 drift = (inPreview >= 0 && inExport >= 0) ? (inExport - inPreview) : 99999;
		std::printf("  %s cut at %lld ms: preview %lld, export %lld, drift %+lld ms\n",
			    std::abs(drift) <= 20 ? "PASS" : "FAIL", (long long)cut.at,
			    (long long)inPreview, (long long)inExport, (long long)drift);
		if (std::abs(drift) > 20)
			++failures;
	}

	// ---- 5. a muted track must be silent in the FILE too ---------------
	std::printf("\n-- muting --\n");
	{
		TimelineModel mm = m;
		mm.tracks[0].muted = true;
		const std::vector<float> silent = TimelineAudio::mixToBuffer(mm, lookup);
		bool anyLoud = false;
		for (float f : silent)
			if (std::abs(f) > 0.01f)
				anyLoud = true;
		ok(!anyLoud, "muting the only track leaves nothing audible");
	}

	// ---- 6. speed: does sped-up sound stay locked to sped-up picture? ---
	//
	// TimelineAudio time-stretches a clip's audio by its speed with atempo, and
	// the comment says that keeps it locked to the picture. Nothing checked it.
	// A 2x clip should occupy half the output time, keep its own pitch (atempo
	// preserves it -- a naive resample would raise it), and still cut where the
	// video cuts.
	std::printf("\n-- speed --\n");
	{
		TimelineModel sm;
		TlTrack st;
		st.kind = TlTrack::Kind::Video;
		TlClip a = mk(1, 0, 0, 2000);    // red, 2 s of source
		a.speed = 2.0;                   // -> 1 s of output
		TlClip b = mk(3, 1000, 0, 2000); // blue starts where red ends
		b.speed = 2.0;
		st.clips.append(a);
		st.clips.append(b);
		sm.tracks.append(st);
		std::printf("     two 2x clips, timeline is %lld ms\n", (long long)sm.durationMs());
		ok(sm.durationMs() == 2000, "a 2x clip takes half the output time");

		const QString fastFile = dir.path() + QStringLiteral("/fast.mp4");
		ClipExporter::Options fo = o;
		fo.timeline = sm;
		ClipExporter ex2;
		bool d2 = false, ok2 = false;
		QObject::connect(&ex2, &ClipExporter::finished,
				 [&](bool g, bool c, const QString &) { d2 = true; ok2 = g && !c; });
		ex2.run(mediaPath(kRed.path), fastFile, fo);
		QElapsedTimer w2;
		w2.start();
		while (!d2 && w2.elapsed() < 180000) {
			QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
			QThread::msleep(20);
		}
		ok(ok2, "the sped-up timeline exports");
		if (ok2) {
			const std::vector<float> fastMono = decodeAudio(fastFile);
			ok(!fastMono.empty(), "and has audio");
			// Pitch preserved: red is still 300 Hz, not 600.
			const double hz = dominantHz(fastMono, 400, nullptr, 0.2);
			std::printf("  %s red at 2x is still %.0f Hz, so the pitch was preserved\n",
				    std::abs(hz - kRed.hz) < 1.0 ? "PASS" : "FAIL", hz);
			if (std::abs(hz - kRed.hz) >= 1.0)
				++failures;
			// And the cut is still where the picture cuts.
			qint64 aCut = -1, vCut = -1;
			for (qint64 ms = 600; ms <= 1400; ms += 5)
				if (std::abs(dominantHz(fastMono, ms, nullptr, 0.04) - kBlue.hz) < 1.0) {
					aCut = ms;
					break;
				}
			for (qint64 ms = 600; ms <= 1400; ms += 10)
				if (colourIs(frameColourAt(fastFile, ms), kBlue.colour)) {
					vCut = ms;
					break;
				}
			const qint64 skew = (aCut >= 0 && vCut >= 0) ? (aCut - vCut) : 99999;
			std::printf("  %s 2x cut: video %lld, audio %lld, skew %+lld ms\n",
				    std::abs(skew) <= 50 ? "PASS" : "FAIL", (long long)vCut,
				    (long long)aCut, (long long)skew);
			if (std::abs(skew) > 50)
				++failures;
		}
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
