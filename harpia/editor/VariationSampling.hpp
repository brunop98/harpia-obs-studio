#pragma once

// Sample combinations without constructing the Cartesian product. Each entry
// is an option index per varying clip, so even products beyond 64 bits can be
// sampled without losing the early clips' choices.
#include <QRandomGenerator>
#include <QSet>
#include <QVector>

#include <algorithm>
#include <limits>
#include <random>

namespace harpia::variation_sampling {
inline constexpr int kListLimit = 2000;
struct Count {
	qint64 value = 0;
	bool overflow = false;
};
inline Count count(const QVector<int> &sizes)
{
	if (sizes.isEmpty() || std::any_of(sizes.begin(), sizes.end(), [](int n) { return n <= 0; })) {
		return {};
	}
	qint64 total = 1;
	for (int n : sizes) {
		if (total > std::numeric_limits<qint64>::max() / n) {
			return {std::numeric_limits<qint64>::max(), true};
		}
		total *= n;
	}
	return {total, false};
}
inline QVector<int> at(const QVector<int> &sizes, qint64 index)
{
	QVector<int> pick(sizes.size(), 0);
	for (int i = int(sizes.size()) - 1; i >= 0; --i) {
		const int n = std::max(1, sizes[i]);
		pick[i] = int(index % n);
		index /= n;
	}
	return pick;
}
inline qint64 indexOf(const QVector<int> &sizes, const QVector<int> &pick)
{
	if (sizes.size() != pick.size()) {
		return -1;
	}
	qint64 index = 0;
	for (int i = 0; i < sizes.size(); ++i) {
		if (sizes[i] <= 0 || pick[i] < 0 || pick[i] >= sizes[i] ||
		    index > (std::numeric_limits<qint64>::max() - pick[i]) / sizes[i]) {
			return -1;
		}
		index = index * sizes[i] + pick[i];
	}
	return index;
}
inline QVector<QVector<int>> sample(const QVector<int> &sizes, int requested, quint64 seed = 0)
{
	const Count population = count(sizes);
	const int take = int(std::min<qint64>(std::clamp(requested, 0, kListLimit), population.value));
	QVector<QVector<int>> out;
	if (!take) {
		return out;
	}
	std::mt19937_64 rng(seed ? seed : QRandomGenerator::system()->generate64());
	if (!population.overflow) {
		// Floyd's algorithm: exactly `take` distinct, uniform indices in O(take)
		// space, including when the request is almost the entire population.
		QSet<qint64> indices;
		for (int i = 0; i < take; ++i) {
			const qint64 j = population.value - take + i;
			const qint64 draw = std::uniform_int_distribution<qint64>(0, j)(rng);
			indices.insert(indices.contains(draw) ? j : draw);
		}
		auto ordered = indices.values();
		std::sort(ordered.begin(), ordered.end());
		for (qint64 index : ordered) {
			out.append(at(sizes, index));
		}
		return out;
	}
	QSet<QVector<int>> seen;
	const auto add = [&](const QVector<int> &pick) {
		if (!seen.contains(pick)) {
			seen.insert(pick);
			out.append(pick);
		}
	};
	for (int tries = 0; out.size() < take && tries < take * 4 + 100; ++tries) {
		QVector<int> pick;
		for (int n : sizes) {
			pick.append(std::uniform_int_distribution<int>(0, n - 1)(rng));
		}
		add(pick);
	}
	// A bounded fallback keeps the requested count even for an unlucky draw.
	QVector<int> next(sizes.size(), 0);
	while (out.size() < take) {
		add(next);
		for (int i = int(sizes.size()) - 1; i >= 0; --i) {
			if (++next[i] < sizes[i]) {
				break;
			}
			next[i] = 0;
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}
} // namespace harpia::variation_sampling
