// How long the preview waits for its sound, and that the sound is right.
//
// The Full-editing preview plays one pre-mixed buffer of the whole timeline,
// and the picture does not wait for it: a mix that takes seconds is seconds
// of silent playback. A real project is many short clips cut from a few long
// recordings, and the mixer used to decode a clip's ENTIRE source to use a
// second of it -- per clip, on every mix. So: one ten-minute source, fifteen
// short clips from it, and the clock on a re-mix (the decoded source is cached
// for the session, so a re-mix is exactly what an edit-then-play costs).
//
// And the faster path must change nothing: the same takes are mixed from the
// cached WAV and from an equivalent file the fast path does not recognise
// (one extra chunk in the header), which goes the old, general way. The two
// buffers must be identical to the sample.
//
//   timelinemix_test <workdir>   (run_avsync.sh generates the source)
#include "editor/TimelineAudio.hpp"
#include "editor/VoiceoverMixer.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *what)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", what);
	if (!c)
		++failures;
}

static TlClip clip(qint64 outStart, qint64 srcStart, qint64 len)
{
	TlClip c;
	c.sourceId = 1;
	c.outStartMs = outStart;
	c.srcStartMs = srcStart;
	c.srcEndMs = srcStart + len;
	c.fadeInMs = 20;
	c.fadeOutMs = 20;
	return c;
}

// The same WAV with a LIST chunk between "fmt " and "data": every decoder
// reads it as the same audio, but it is not the layout decodeToWav writes.
static bool writeWithExtraChunk(const QString &from, const QString &to)
{
	QFile in(from);
	if (!in.open(QIODevice::ReadOnly))
		return false;
	QByteArray b = in.readAll();
	if (b.size() < 44 || b.mid(36, 4) != "data")
		return false;
	QByteArray list("LIST");
	const quint32 n = 4;
	list.append(char(n & 0xff)).append(char(0)).append(char(0)).append(char(0));
	list.append("INFO");
	b.insert(36, list);
	const quint32 riff = quint32(b.size() - 8);
	for (int i = 0; i < 4; ++i)
		b[4 + i] = char((riff >> (8 * i)) & 0xff);
	QFile out(to);
	return out.open(QIODevice::WriteOnly) && out.write(b) == b.size();
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	const QString src = QString::fromLocal8Bit(argc > 1 ? argv[1] : ".") + QStringLiteral("/long_tone.m4a");
	if (!QFile::exists(src)) {
		std::printf("missing %s\n", qUtf8Printable(src));
		return 2;
	}

	// Fifteen clips of 0.5-2 s from all over the ten minutes, on two lanes,
	// the way a cut-down recording looks.
	TimelineModel m;
	TlTrack a, b;
	a.kind = TlTrack::Kind::Video;
	b.kind = TlTrack::Kind::Audio;
	qint64 at = 0;
	for (int i = 0; i < 15; ++i) {
		const qint64 len = 500 + (i % 4) * 500;
		(i % 3 ? a : b).clips.append(clip(at, 20000 + i * 37000, len));
		at += len;
	}
	m.tracks.append(a);
	m.tracks.append(b);
	const auto lookup = [&src](int) { return src; };

	std::printf("\n-- a re-mix, the cost of every edit-then-play --\n");
	QElapsedTimer t;
	t.start();
	const std::vector<float> first = TimelineAudio::mixToBuffer(m, lookup);
	const qint64 coldMs = t.elapsed();
	t.restart();
	const std::vector<float> again = TimelineAudio::mixToBuffer(m, lookup);
	const qint64 warmMs = t.elapsed();
	std::printf("     first mix %lld ms (decodes the source once), re-mix %lld ms\n", (long long)coldMs,
		    (long long)warmMs);
	ok(!first.empty() && first == again, "the re-mix is the same sound");
	ok(warmMs < 250, "a re-mix of fifteen clips from a ten-minute source is quick (< 250 ms)");

	std::printf("\n-- the fast path changes nothing --\n");
	QTemporaryDir dir;
	const QString wav = dir.filePath(QStringLiteral("canonical.wav"));
	const QString other = dir.filePath(QStringLiteral("extra_chunk.wav"));
	ok(VoiceoverMixer::decodeToWav(src, wav), "the source decodes to a WAV");
	ok(writeWithExtraChunk(wav, other), "and an equivalent one the fast path does not read");

	std::vector<VoiceoverMixer::Take> takes, takesOther;
	const auto take = [&](qint64 outStart, qint64 srcStart, qint64 play, double vol) {
		VoiceoverMixer::Take k;
		k.path = wav;
		k.outStartMs = outStart;
		k.srcStartMs = srcStart;
		k.playMs = play;
		k.volume = vol;
		k.fadeInMs = 30;
		k.fadeOutMs = 45;
		takes.push_back(k);
		k.path = other;
		takesOther.push_back(k);
	};
	take(0, 1000, 1500, 1.0);
	take(1200, 123456, 777, 0.5);  // overlapping the first: two voices mix
	take(3000, 599000, 5000, 1.0); // runs off the end of the source: clipped
	take(9000, 650000, 1000, 1.0); // starts past the end: nothing
	take(9500, 0, 333, 1.7);       // the very start
	take(10000, 300000, 0, 1.0);   // playMs 0: to the end of the source
	t.restart();
	const std::vector<float> fast = VoiceoverMixer::renderTakes(takes);
	const qint64 fastMs = t.elapsed();
	t.restart();
	const std::vector<float> general = VoiceoverMixer::renderTakes(takesOther);
	const qint64 generalMs = t.elapsed();
	std::printf("     %zu samples; fast %lld ms, general %lld ms\n", fast.size(), (long long)fastMs,
		    (long long)generalMs);
	ok(!fast.empty() && fast.size() == general.size(), "both mixes are the same length");
	double maxDiff = 0.0;
	for (size_t i = 0; i < std::min(fast.size(), general.size()); ++i)
		maxDiff = std::max(maxDiff, double(std::abs(fast[i] - general[i])));
	std::printf("     largest difference %g\n", maxDiff);
	ok(maxDiff == 0.0, "and identical to the sample");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
