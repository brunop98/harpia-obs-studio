#include "BeatDetector.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <exception>
#include <exception>
#include <limits>
#include <numeric>

namespace harpia::beat_detection {
namespace {

constexpr int kFft = 512;
constexpr double kPi = 3.14159265358979323846;

bool canceled(std::atomic<bool> *flag) { return flag && flag->load(); }

void fft(std::vector<std::complex<double>> &values)
{
	for (int i = 1, j = 0; i < kFft; ++i) {
		int bit = kFft >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j)
			std::swap(values[size_t(i)], values[size_t(j)]);
	}
	for (int size = 2; size <= kFft; size <<= 1) {
		const std::complex<double> step = std::polar(1.0, -2.0 * kPi / size);
		for (int at = 0; at < kFft; at += size) {
			std::complex<double> weight(1.0, 0.0);
			for (int k = 0; k < size / 2; ++k) {
				const auto even = values[size_t(at + k)];
				const auto odd = weight * values[size_t(at + k + size / 2)];
				values[size_t(at + k)] = even + odd;
				values[size_t(at + k + size / 2)] = even - odd;
				weight *= step;
			}
		}
	}
}

double median(std::vector<double> values)
{
	if (values.empty())
		return 0.0;
	const auto middle = values.begin() + values.size() / 2;
	std::nth_element(values.begin(), middle, values.end());
	return *middle;
}

struct Onset {
	int frame = 0;
	double strength = 0.0;
};

Result detectImpl(const std::vector<float> &pcm, int sampleRate, int channels,
		  const Options &options, std::atomic<bool> *cancel)
{
	Result result;
	if (canceled(cancel)) {
		result.canceled = true;
		return result;
	}
	if (sampleRate < 1000 || sampleRate > 384000 || channels <= 0 || channels > 32 ||
	    pcm.size() % size_t(channels) != 0) {
		result.error = QStringLiteral("The audio sample format is not supported for beat analysis.");
		return result;
	}
	const size_t frames = pcm.size() / size_t(channels);
	if (frames < size_t(sampleRate))
		return result; // not enough sound to establish a reliable pulse
	const int step = std::max(1, sampleRate / 11025);
	const double rate = double(sampleRate) / step;
	const size_t monoFrames = frames / size_t(step);
	if (monoFrames > size_t(std::numeric_limits<int>::max())) {
		result.error = QStringLiteral("Choose a shorter audio clip for beat analysis.");
		return result;
	}
	std::vector<double> mono(monoFrames);
	double peak = 0.0;
	for (size_t f = 0; f < monoFrames; ++f) {
		if ((f & 0x3fff) == 0 && canceled(cancel)) {
			result.canceled = true;
			return result;
		}
		double value = 0.0;
		for (int s = 0; s < step; ++s) {
			// The strongest channel preserves a pulse even in opposite-phase
			// stereo. NaNs and infinities are treated as missing samples.
			double channelValue = 0.0;
			for (int c = 0; c < channels; ++c) {
				const float sample = pcm[(f * size_t(step) + size_t(s)) * size_t(channels) + size_t(c)];
				if (std::isfinite(sample) && std::abs(sample) > std::abs(channelValue))
					channelValue = sample;
			}
			value += std::clamp(channelValue, -8.0, 8.0);
		}
		mono[f] = value / step;
		peak = std::max(peak, std::abs(mono[f]));
	}
	if (peak < 1e-7)
		return result;
	// Normalize once so a quiet master or a loud recording has the same
	// threshold. Local adaptation below still follows changing dynamics.
	for (double &sample : mono)
		sample /= peak;
	const int hop = std::max(1, int(std::lround(rate * 0.01)));
	const int count = int(monoFrames / size_t(hop));
	std::vector<double> novelty(size_t(count), 0.0);
	std::vector<double> previous(kFft / 2 + 1, 0.0);
	std::vector<std::complex<double>> spectrum(kFft);
	std::vector<double> window(kFft);
	for (int k = 0; k < kFft; ++k)
		window[size_t(k)] = 0.5 - 0.5 * std::cos(2.0 * kPi * k / (kFft - 1));
	for (int frame = 0; frame < count; ++frame) {
		if ((frame & 31) == 0 && canceled(cancel)) {
			result.canceled = true;
			return result;
		}
		const qint64 first = qint64(frame) * hop - kFft / 2;
		for (int k = 0; k < kFft; ++k) {
			const qint64 at = first + k;
			spectrum[size_t(k)] = at < 0 || at >= qint64(monoFrames)
				? 0.0 : mono[size_t(at)] * window[size_t(k)];
		}
		fft(spectrum);
		double flux = 0.0;
		for (int k = 1; k <= kFft / 2; ++k) {
			const double frequency = k * rate / kFft;
			const double magnitude = std::log1p(10.0 * std::abs(spectrum[size_t(k)]));
			const double weight = frequency < 250.0 ? 2.0 : frequency < 2500.0 ? 1.0 : 0.45;
			flux += std::max(0.0, magnitude - previous[size_t(k)]) * weight;
			previous[size_t(k)] = magnitude;
		}
		novelty[size_t(frame)] = flux;
	}
	const double maximum = *std::max_element(novelty.begin(), novelty.end());
	if (maximum < 1e-8)
		return result;
	const double sensitivity = std::isfinite(options.sensitivity)
		? std::clamp(options.sensitivity, 0.5, 2.0) : 1.0;
	std::vector<Onset> onsets;
	std::vector<double> neighborhood;
	neighborhood.reserve(51);
	for (int i = 1; i + 2 < count; ++i) {
		const double value = novelty[size_t(i)];
		if (value < novelty[size_t(i - 1)] || value <= novelty[size_t(i + 1)] ||
		    value < novelty[size_t(std::max(0, i - 2))] || value < novelty[size_t(i + 2)])
			continue;
		neighborhood.clear();
		for (int j = std::max(0, i - 25); j <= std::min(count - 1, i + 25); ++j)
			neighborhood.push_back(novelty[size_t(j)]);
		const double baseline = median(neighborhood);
		for (double &sample : neighborhood)
			sample = std::abs(sample - baseline);
		const double threshold = baseline + (1.6 * median(neighborhood) + maximum * 0.0075) / sensitivity;
		if (value <= threshold)
			continue;
		if (!onsets.empty() && i - onsets.back().frame < 9) {
			if (value > onsets.back().strength)
				onsets.back() = {i, value};
		} else {
			onsets.push_back({i, value});
		}
	}
	if (onsets.size() < 4)
		return result;
	std::vector<double> train(size_t(count), 0.0);
	for (const Onset &onset : onsets)
		train[size_t(onset.frame)] = onset.strength / maximum;
	const double secondsPerFrame = hop / rate;
	const int firstLag = std::max(1, int(std::lround(60.0 / (200.0 * secondsPerFrame))));
	const int lastLag = std::min(count / 2, int(std::lround(60.0 / (60.0 * secondsPerFrame))));
	double bestScore = 0.0, bestCorrelation = 0.0;
	int period = 0;
	for (int lag = firstLag; lag <= lastLag; ++lag) {
		if (canceled(cancel)) {
			result.canceled = true;
			return result;
		}
		double dot = 0.0, currentEnergy = 0.0, previousEnergy = 0.0;
		for (int i = lag + 1; i < count; ++i) {
			const double current = train[size_t(i)];
			const double prior = std::max({train[size_t(i - lag - 1)], train[size_t(i - lag)],
				train[size_t(std::min(count - 1, i - lag + 1))]});
			dot += current * prior;
			currentEnergy += current * current;
			previousEnergy += train[size_t(i - lag)] * train[size_t(i - lag)];
		}
		const double correlation = currentEnergy > 0.0 && previousEnergy > 0.0
			? std::clamp(dot / std::sqrt(currentEnergy * previousEnergy), 0.0, 1.0) : 0.0;
		const double bpm = 60.0 / (lag * secondsPerFrame);
		const double distance = std::log2(bpm / 120.0) / 0.6;
		const double score = correlation * (0.85 + 0.15 * std::exp(-0.5 * distance * distance));
		if (score > bestScore) {
			bestScore = score;
			bestCorrelation = correlation;
			period = lag;
		}
	}
	if (period == 0 || bestCorrelation < 0.28)
		return result;
	// Track actual attacks with a tempo constraint. Missing beats may bridge a
	// pause, but a bridge never creates a marker where no attack was heard.
	std::vector<double> score(onsets.size(), 0.0);
	std::vector<int> predecessor(onsets.size(), -1);
	for (int i = 0; i < int(onsets.size()); ++i) {
		const double strength = std::sqrt(onsets[size_t(i)].strength / maximum);
		score[size_t(i)] = strength;
		for (int j = i - 1; j >= 0; --j) {
			const double gap = onsets[size_t(i)].frame - onsets[size_t(j)].frame;
			if (gap > period * 32.18)
				break;
			const int multiple = int(std::lround(gap / period));
			if (multiple < 1 || multiple > 32)
				continue;
			const double offset = std::abs(gap - multiple * period) / period;
			if (offset > 0.18)
				continue;
			const double candidate = score[size_t(j)] + strength - 3.0 * offset * offset -
				0.02 * std::min(10, multiple - 1);
			if (candidate > score[size_t(i)]) {
				score[size_t(i)] = candidate;
				predecessor[size_t(i)] = j;
			}
		}
	}
	QVector<int> path;
	// Long breaks create independent rhythmic sections. Recover the best real
	// attack chain from each section rather than keeping only the globally
	// longest one, which would discard music after a quiet bridge or restart.
	int sectionStart = 0;
	for (int last = 0; last < int(onsets.size()); ++last) {
		if (last + 1 < int(onsets.size()) &&
		    onsets[size_t(last + 1)].frame - onsets[size_t(last)].frame <= period * 32.18)
			continue;
		int best = sectionStart;
		for (int i = sectionStart + 1; i <= last; ++i)
			if (score[size_t(i)] > score[size_t(best)])
				best = i;
		QVector<int> section;
		for (int at = best; at >= sectionStart; at = predecessor[size_t(at)])
			section.prepend(at);
		if (section.size() >= 4)
			path += section;
		sectionStart = last + 1;
	}
	if (path.size() < 4)
		return result;
	std::vector<double> intervals;
	for (int i = 1; i < path.size(); ++i) {
		const double gap = onsets[size_t(path[i])].frame - onsets[size_t(path[i - 1])].frame;
		const int multiple = std::max(1, int(std::lround(gap / period)));
		intervals.push_back(gap / multiple);
	}
	result.bpm = 60.0 / (median(intervals) * secondsPerFrame);
	result.confidence = bestCorrelation;
	for (int index : path) {
		// Hann-window spectral flux peaks shortly before a sharp attack. Move
		// to the attack edge, then use the local amplitude envelope to refine
		// the final timestamp without imposing a synthetic beat grid.
		const qint64 center = qint64(onsets[size_t(index)].frame) * hop;
		const qint64 begin = std::max<qint64>(0, center - kFft / 4);
		const qint64 end = std::min<qint64>(qint64(monoFrames), center + kFft / 2);
		double strongest = 0.0;
		for (qint64 sample = begin; sample < end; ++sample)
			strongest = std::max(strongest, std::abs(mono[size_t(sample)]));
		qint64 attack = center;
		for (qint64 sample = begin; sample < end; ++sample)
			if (std::abs(mono[size_t(sample)]) >= strongest * 0.35) {
				attack = sample;
				break;
			}
		const qint64 ms = std::max<qint64>(0, qint64(std::llround(attack * 1000.0 / rate)));
		if (result.beatsMs.isEmpty() || ms > result.beatsMs.back())
			result.beatsMs.append(ms);
	}
	if (canceled(cancel)) {
		result = {};
		result.canceled = true;
	}
	return result;
}

} // namespace

Result detect(const std::vector<float> &pcm, int sampleRate, int channels,
	      const Options &options, std::atomic<bool> *cancel)
{
	try {
		return detectImpl(pcm, sampleRate, channels, options, cancel);
	} catch (const std::exception &) {
		Result result;
		result.error = QStringLiteral("Could not analyze this audio. Choose a shorter music clip and try again.");
		return result;
	}
}

} // namespace harpia::beat_detection
