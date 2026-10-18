// Recording Windows audio in the voiceover panel: the source ids that sit
// beside the microphones' own, what happens where there is no Windows to ask,
// and the trim that takes the wait off both ends of a grab.
//
// The WASAPI half needs Windows and a sound device, neither of which is here;
// it is compile-checked with MinGW. Everything that decides what lands in the
// take -- which source, and which samples survive the trim -- is checked here.
#include "editor/SystemAudioCapture.hpp"
#include "editor/WavSilence.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static void putU32(char *p, quint32 v)
{
	for (int i = 0; i < 4; ++i)
		p[i] = char((v >> (8 * i)) & 0xff);
}
static void putU16(char *p, quint16 v)
{
	p[0] = char(v & 0xff);
	p[1] = char(v >> 8);
}

// A 16-bit PCM WAV laid out exactly like AudioRecorder's.
static void writeWav(const QString &path, int rate, int channels, const std::vector<qint16> &samples)
{
	QByteArray h(44, '\0');
	std::memcpy(h.data(), "RIFF", 4);
	putU32(h.data() + 4, quint32(36 + samples.size() * 2));
	std::memcpy(h.data() + 8, "WAVEfmt ", 8);
	putU32(h.data() + 16, 16);
	putU16(h.data() + 20, 1);
	putU16(h.data() + 22, quint16(channels));
	putU32(h.data() + 24, quint32(rate));
	putU32(h.data() + 28, quint32(rate * channels * 2));
	putU16(h.data() + 32, quint16(channels * 2));
	putU16(h.data() + 34, 16);
	std::memcpy(h.data() + 36, "data", 4);
	putU32(h.data() + 40, quint32(samples.size() * 2));
	QFile f(path);
	f.open(QIODevice::WriteOnly);
	f.write(h);
	f.write(reinterpret_cast<const char *>(samples.data()), qint64(samples.size() * 2));
}

// `silentMs` of zeros, `toneMs` of a 440 Hz tone, `tailMs` of zeros.
static std::vector<qint16> take(int rate, int channels, int silentMs, int toneMs, int tailMs, double amp = 0.5)
{
	std::vector<qint16> s;
	const auto frames = [rate](int ms) { return qint64(ms) * rate / 1000; };
	for (qint64 i = 0; i < frames(silentMs) * channels; ++i)
		s.push_back(0);
	for (qint64 i = 0; i < frames(toneMs); ++i)
		for (int c = 0; c < channels; ++c)
			s.push_back(qint16(std::lround(amp * 32767.0 * std::sin(2.0 * M_PI * 440.0 * double(i) / rate))));
	for (qint64 i = 0; i < frames(tailMs) * channels; ++i)
		s.push_back(0);
	return s;
}

static qint64 dataBytes(const QString &path)
{
	QFile f(path);
	f.open(QIODevice::ReadOnly);
	const QByteArray a = f.readAll();
	if (a.size() < 44)
		return -1;
	const auto *p = reinterpret_cast<const unsigned char *>(a.constData());
	const qint64 riff = qint64(p[4]) | (qint64(p[5]) << 8) | (qint64(p[6]) << 16) | (qint64(p[7]) << 24);
	const qint64 data = qint64(p[40]) | (qint64(p[41]) << 8) | (qint64(p[42]) << 16) | (qint64(p[43]) << 24);
	if (riff != a.size() - 8 || data != a.size() - 44)
		return -2; // the header lies about the file
	return data;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	QTemporaryDir dir;

	std::printf("\n-- the sources beside the microphones --\n");
	{
		QString exe = QStringLiteral("x");
		ok(system_audio::isSourceId(system_audio::sourceId(), &exe) && exe.isEmpty(),
		   "\"everything\" is a Windows audio source, for no app in particular");
		ok(system_audio::isSourceId(system_audio::sourceId(QStringLiteral("chrome.exe")), &exe) &&
			   exe == QStringLiteral("chrome.exe"),
		   "\"only Chrome\" carries its app, by file name (stable across launches, unlike a process id)");
		ok(!system_audio::isSourceId(QStringLiteral("{0.0.1.00000000}.{6f3c-mic}")),
		   "a microphone's id is not one");
		ok(!system_audio::isSourceId(QString()) && !system_audio::isSourceId(QStringLiteral("harpia:system:app:")),
		   "nor is nothing, or an app with no name");
	}

#if !defined(_WIN32)
	std::printf("\n-- with no Windows to ask --\n");
	{
		ok(!system_audio::available() && !system_audio::perAppAvailable() && system_audio::apps().isEmpty(),
		   "nothing to offer: the panel lists microphones only");
		ok(system_audio::runningApps().isEmpty(), "and no running apps to pick for PC audio");
		SystemAudioCapture cap;
		QString why;
		ok(!cap.start(QString(), &why) && !cap.running(), "starting fails cleanly");
		ok(why.contains(QStringLiteral("only available on Windows")), "and says why");
		ok(cap.take().isEmpty() && cap.failure().isEmpty(), "with nothing captured and nothing broken");
	}
#endif

	std::printf("\n-- trimming the wait off a grab --\n");
	{
		const QString p = dir.filePath(QStringLiteral("grab.wav"));
		// 2 s of switching to the browser, 1.5 s of sound, 3 s of the video
		// having ended -- stereo at 48 kHz, as Windows audio is recorded.
		writeWav(p, 48000, 2, take(48000, 2, 2000, 1500, 3000));
		const WavTrim t = trimWavSilence(p);
		ok(t.ok && !t.silent, "read, and found the sound");
		ok(t.leadMs >= 1950 && t.leadMs <= 1965, "the 2 s wait goes, less a 40 ms margin before the sound");
		ok(t.tailMs >= 2740 && t.tailMs <= 2760, "the 3 s tail goes, less 250 ms for the last echo");
		ok(t.durationMs >= 1780 && t.durationMs <= 1800, "about 1.8 s left");
		const qint64 bytes = dataBytes(p);
		ok(bytes > 0 && std::abs(bytes / 4 * 1000 / 48000 - t.durationMs) <= 1,
		   "the file is rewritten to match, its header sizes true");
		const WavTrim again = trimWavSilence(p);
		ok(again.ok && again.leadMs == 0 && again.tailMs == 0, "and trimming it again changes nothing");
	}
	{
		const QString p = dir.filePath(QStringLiteral("quiet.wav"));
		writeWav(p, 48000, 2, take(48000, 2, 4000, 0, 0));
		const QByteArray before = [&p]() {
			QFile f(p);
			f.open(QIODevice::ReadOnly);
			return f.readAll();
		}();
		const WavTrim t = trimWavSilence(p);
		ok(t.ok && t.silent, "all silence: said so (the video was paused, or muted)");
		QFile f(p);
		f.open(QIODevice::ReadOnly);
		ok(f.readAll() == before, "and the file is left alone");
	}
	{
		const QString p = dir.filePath(QStringLiteral("soft.wav"));
		// -50 dB is quiet, but it is sound: an ambience bed must not be cut.
		writeWav(p, 44100, 1, take(44100, 1, 1000, 1000, 1000, std::pow(10.0, -50.0 / 20.0)));
		const WavTrim t = trimWavSilence(p);
		ok(t.ok && !t.silent && t.durationMs > 1200 && t.durationMs < 1400,
		   "a soft sound at -50 dB is kept (mono, 44.1 kHz too)");
	}
	{
		const QString p = dir.filePath(QStringLiteral("full.wav"));
		writeWav(p, 48000, 2, take(48000, 2, 0, 1000, 0));
		const WavTrim t = trimWavSilence(p);
		ok(t.ok && t.leadMs == 0 && t.tailMs == 0 && t.durationMs == 1000, "sound end to end: nothing to cut");
	}
	{
		const QString p = dir.filePath(QStringLiteral("junk.wav"));
		QFile f(p);
		f.open(QIODevice::WriteOnly);
		f.write("not a wav file at all, just some text that is long enough to have a header");
		f.close();
		ok(!trimWavSilence(p).ok, "not a WAV: not touched");
		ok(!trimWavSilence(dir.filePath(QStringLiteral("missing.wav"))).ok, "no file: not touched");
	}

	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
