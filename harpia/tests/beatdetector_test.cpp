// Musical pulses, rather than a restatement of the detector's envelope:
// low kicks and noisy percussion sit over a sustained chord. Quiet offbeats,
// changes in level, and gaps must not turn into a second tempo or fake beats.
#include "editor/BeatDetector.hpp"

#include <QCoreApplication>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using namespace harpia::beat_detection;

namespace {
constexpr double pi = 3.14159265358979323846;
int failures = 0;

void check(bool condition, const char *message)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}

struct Music {
	std::vector<float> pcm;
	QVector<qint64> expected;
	int rate = 22050;
	int channels = 1;
	int durationMs = 12000;
};

void hit(Music &music, double atMs, double level)
{
	const int start = int(std::llround(atMs * music.rate / 1000.0));
	const int length = int(music.rate * 0.09);
	unsigned random = 12345;
	for (int i = 0; i < length; ++i) {
		const int frame = start + i;
		if (frame < 0 || size_t(frame) * music.channels >= music.pcm.size())
			continue;
		const double t = double(i) / music.rate;
		random = random * 1664525u + 1013904223u;
		const double noise = double((random >> 8) & 0xffffu) / 32767.5 - 1.0;
		const double kick = std::sin(2.0 * pi * (90.0 * t - 220.0 * t * t));
		const float value = float(level * (0.7 * kick * std::exp(-t * 45.0) +
						  0.3 * noise * std::exp(-t * 110.0)));
		for (int channel = 0; channel < music.channels; ++channel)
			music.pcm[size_t(frame) * music.channels + channel] += value;
	}
}

Music music(double bpm, bool offbeats = false, bool changingGain = false, bool gap = false,
	    int rate = 22050, int channels = 1)
{
	Music result;
	result.rate = rate;
	result.channels = channels;
	result.pcm.resize(size_t(result.durationMs) * rate / 1000 * channels);
	for (size_t frame = 0; frame < result.pcm.size() / channels; ++frame) {
		const double t = double(frame) / rate;
		const float chord = float(0.025 * (std::sin(2 * pi * 220 * t) +
						      0.5 * std::sin(2 * pi * 330 * t)));
		for (int channel = 0; channel < channels; ++channel)
			result.pcm[frame * channels + channel] = chord;
	}
	const double period = 60000.0 / bpm;
	for (double at = 250; at + 100 < result.durationMs; at += period) {
		if (gap && at >= 4500 && at < 8000)
			continue;
		result.expected.append(qint64(std::llround(at)));
		const double level = changingGain ? 0.25 + 0.55 * at / result.durationMs : 0.7;
		hit(result, at, level);
		if (offbeats)
			hit(result, at + period / 2, level * 0.13);
	}
	return result;
}

qint64 nearestDistance(qint64 at, const QVector<qint64> &times)
{
	qint64 distance = std::numeric_limits<qint64>::max();
	for (qint64 time : times)
		distance = std::min(distance, qint64(std::llabs(time - at)));
	return distance;
}

bool matches(const Result &found, const Music &source, double minimum = 0.8)
{
	if (found.beatsMs.isEmpty() || !found.error.isEmpty() || found.canceled)
		return false;
	int matched = 0, recalled = 0;
	for (qint64 beat : found.beatsMs) {
		if (beat < 0 || beat >= source.durationMs)
			return false;
		matched += nearestDistance(beat, source.expected) <= 45;
	}
	for (qint64 beat : source.expected)
		recalled += nearestDistance(beat, found.beatsMs) <= 45;
	return double(matched) / found.beatsMs.size() >= minimum &&
	       double(recalled) / source.expected.size() >= minimum;
}
} // namespace

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	for (double bpm : {90.0, 120.0, 150.0}) {
		const Music source = music(bpm);
		const Result result = detect(source.pcm, source.rate, source.channels);
		std::printf("  %.0f BPM fixture: %.2f BPM, %lld markers, confidence %.3f\n", bpm, result.bpm,
			    static_cast<long long>(result.beatsMs.size()), result.confidence);
		check(matches(result, source), "percussive music produces markers on its actual beats");
		check(std::isfinite(result.bpm) && std::abs(result.bpm - bpm) < bpm * 0.08,
		      "the detected pulse tempo follows the musical fixture");
	}
	{
		const Music source = music(120, true);
		const Result result = detect(source.pcm, source.rate, source.channels);
		check(matches(result, source), "quiet offbeat percussion does not double the marker density");
	}
	{
		Music source = music(120, false, true);
		check(matches(detect(source.pcm, source.rate, source.channels), source),
		      "beats remain detectable when the music gets louder over time");
		for (float &value : source.pcm)
			value *= 0.02f;
		check(matches(detect(source.pcm, source.rate, source.channels), source),
		      "quiet music keeps the same beat timing instead of falling below a loudness threshold");
	}
	{
		const Music source = music(120, false, false, true);
		const Result result = detect(source.pcm, source.rate, source.channels);
		check(matches(result, source), "music on either side of a pause keeps its pulse timing");
		check(std::none_of(result.beatsMs.begin(), result.beatsMs.end(), [](qint64 beat) {
			return beat > 4800 && beat < 7900;
		}), "no fabricated grid markers are placed through the pause");
	}
	{
		Music source;
		source.durationMs = 48000;
		source.pcm.resize(size_t(source.durationMs) * source.rate / 1000);
		for (int at = 250; at < source.durationMs; at += 500) {
			if (at >= 7500 && at < 40500)
				continue;
			source.expected.append(at);
			hit(source, at, 0.7);
		}
		const Result result = detect(source.pcm, source.rate, 1);
		check(matches(result, source), "a long quiet bridge does not discard the music on either side");
		check(std::none_of(result.beatsMs.begin(), result.beatsMs.end(), [](qint64 beat) {
			return beat > 7800 && beat < 40200;
		}), "a long quiet bridge receives no invented beat markers");
	}
	{
		const Music source = music(120, false, false, false, 48000, 2);
		check(matches(detect(source.pcm, source.rate, source.channels), source),
		      "48 kHz stereo input preserves beat timing");
	}
	{
		std::vector<float> silence(22050 * 8, 0.0f);
		check(detect(silence, 22050, 1).beatsMs.isEmpty(), "silence produces no beat markers");
		for (size_t i = 0; i < silence.size(); ++i)
			silence[i] = float(0.4 * std::sin(2.0 * pi * 440.0 * i / 22050.0));
		check(detect(silence, 22050, 1).beatsMs.isEmpty(), "a sustained tone has no false repeating beat");
		std::fill(silence.begin(), silence.end(), std::numeric_limits<float>::quiet_NaN());
		check(detect(silence, 22050, 1).beatsMs.isEmpty(), "non-finite audio cannot create markers");
	}
	{
		Music irregular;
		irregular.pcm.resize(size_t(irregular.durationMs) * irregular.rate / 1000);
		for (double at : {250.0, 1100.0, 2900.0, 5400.0, 8200.0, 11500.0})
			hit(irregular, at, 0.7);
		check(detect(irregular.pcm, irregular.rate, 1).beatsMs.isEmpty(),
		      "isolated irregular sounds do not pretend to be a stable music pulse");
	}
	{
		const Music source = music(120);
		std::atomic<bool> cancel{true};
		const Result result = detect(source.pcm, source.rate, source.channels, {}, &cancel);
		check(result.canceled && result.beatsMs.isEmpty(), "canceling analysis produces no partial marker set");
		check(detect({}, 22050, 1).beatsMs.isEmpty() && detect(source.pcm, 0, 1).beatsMs.isEmpty() &&
		      detect(source.pcm, source.rate, 0).beatsMs.isEmpty(), "empty and invalid input is handled safely");
	}
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
