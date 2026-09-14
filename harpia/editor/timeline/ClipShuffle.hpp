#pragma once

// Randomising the ORDER of the clips on a lane, and nothing else about them.
//
// The point is experimentation: many versions of the same edit, differing only
// in sequence, produced and undone fast enough to compare by feel. So the rules
// are simple and the clips are untouched -- a shuffle moves each clip's start
// time and leaves its source range, speed, volume, keys and text exactly as
// they were. Pure functions over indices and a clip vector, so the whole thing
// is checked without a widget.
//
// Vocabulary: the clips of one lane, sorted by start, occupy SLOTS 0..n-1. An
// `order` is a permutation: order[k] is the index (in that sorted list) of the
// clip that now sits in slot k. Slots the options pin (the first clip, the last,
// anything not selected) map to themselves.

#include "TimelineModel.hpp"

#include <QVector>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace harpia {

struct ShuffleOptions {
	// Which slots stay put regardless of anything else.
	enum class Keep { None, First, Last, FirstAndLast };
	// How far from the current order the result may be.
	//   Low:    a few neighbouring swaps -- the edit is recognisably the same
	//           with a couple of beats traded.
	//   Medium: about half the movable clips change place.
	//   High:   a full Fisher-Yates over every movable clip.
	enum class Strength { Low, Medium, High };
	Keep keep = Keep::None;
	Strength strength = Strength::High;
	// Retry until the result differs from the current order by at least the
	// strength's own floor (see minDisplacedFor). Without it a full shuffle of
	// three clips hands the same order back one time in six.
	bool avoidSameOrder = true;
	// Only clips that are selected take part; everything else keeps its slot.
	bool selectedOnly = false;
};

// How many slots must change hands for a result to count as "different" at a
// given strength. At least one always; High wants most of them to move.
inline int minDisplacedFor(ShuffleOptions::Strength s, int movable)
{
	if (movable < 2)
		return 0;
	switch (s) {
	case ShuffleOptions::Strength::Low: return 2; // one swap displaces two
	case ShuffleOptions::Strength::Medium: return std::max(2, movable / 2);
	case ShuffleOptions::Strength::High: break;
	}
	return std::max(2, (movable * 2 + 2) / 3); // two thirds, rounded up
}

// Slots whose clip ends up somewhere else.
inline int displacedSlots(const QVector<int> &order)
{
	int d = 0;
	for (int k = 0; k < order.size(); ++k)
		if (order[k] != k)
			++d;
	return d;
}

// One shuffle attempt. `movable[k]` says whether slot k takes part. Uniform
// over the permutations it is allowed to produce: High is Fisher-Yates over
// the movable slots (std::shuffle), Medium shuffles a random half of them,
// Low swaps a few neighbouring pairs among them.
template<class Rng>
QVector<int> shuffleOnce(const QVector<bool> &movable, ShuffleOptions::Strength strength, Rng &rng)
{
	const int n = movable.size();
	QVector<int> order(n);
	std::iota(order.begin(), order.end(), 0);
	QVector<int> movableIdx; // the movable slots, in order
	for (int k = 0; k < n; ++k)
		if (movable[k])
			movableIdx.append(k);
	const int m = movableIdx.size();
	if (m < 2)
		return order;

	// The clips currently in the movable slots, permuted, then written back
	// into those same slots -- so a pinned slot in the middle is never crossed
	// by the bookkeeping, only by the clips around it.
	QVector<int> pool = movableIdx;
	switch (strength) {
	case ShuffleOptions::Strength::High:
		std::shuffle(pool.begin(), pool.end(), rng);
		break;
	case ShuffleOptions::Strength::Medium: {
		// Pick about half the movable slots and shuffle the clips among just
		// those; the other half stay where they are.
		QVector<int> pick(m);
		std::iota(pick.begin(), pick.end(), 0);
		std::shuffle(pick.begin(), pick.end(), rng);
		const int half = std::max(2, (m + 1) / 2);
		pick.resize(half);
		std::sort(pick.begin(), pick.end());
		QVector<int> sub;
		for (int i : pick)
			sub.append(pool[i]);
		std::shuffle(sub.begin(), sub.end(), rng);
		for (int i = 0; i < half; ++i)
			pool[pick[i]] = sub[i];
		break;
	}
	case ShuffleOptions::Strength::Low: {
		// About a fifth of the movable clips (at least one pair) trade places
		// with a neighbour: one swap per ten clips moves two in ten. Distinct
		// pairs, so two swaps cannot undo each other.
		const int swaps = std::max(1, m / 10);
		QVector<int> starts(m - 1);
		std::iota(starts.begin(), starts.end(), 0);
		std::shuffle(starts.begin(), starts.end(), rng);
		QVector<bool> used(m, false);
		int done = 0;
		for (int s : starts) {
			if (done >= swaps)
				break;
			if (used[s] || used[s + 1])
				continue;
			std::swap(pool[s], pool[s + 1]);
			used[s] = used[s + 1] = true;
			++done;
		}
		break;
	}
	}
	for (int i = 0; i < m; ++i)
		order[movableIdx[i]] = pool[i];
	return order;
}

// The order to apply: shuffleOnce, retried while avoidSameOrder asks for more
// change than it produced. Bounded, so a pathological case (two movable clips
// at High, where the only other order IS the swap) settles rather than spins.
// Returns the identity when fewer than two slots can move.
template<class Rng>
QVector<int> shuffledOrder(const QVector<bool> &movable, const ShuffleOptions &opt, Rng &rng)
{
	int m = 0;
	for (bool b : movable)
		m += b ? 1 : 0;
	const int want = opt.avoidSameOrder ? minDisplacedFor(opt.strength, m) : 0;
	QVector<int> best = shuffleOnce(movable, opt.strength, rng);
	int bestD = displacedSlots(best);
	for (int attempt = 0; attempt < 40 && bestD < want; ++attempt) {
		QVector<int> o = shuffleOnce(movable, opt.strength, rng);
		const int d = displacedSlots(o);
		if (d > bestD) {
			best = o;
			bestD = d;
		}
	}
	return best;
}

// Which slots of a lane may move under `opt`, given which are selected
// (`selected` may be empty when selectedOnly is off).
inline QVector<bool> movableSlots(int n, const ShuffleOptions &opt, const QVector<bool> &selected)
{
	QVector<bool> mv(n, true);
	if (opt.selectedOnly)
		for (int k = 0; k < n; ++k)
			mv[k] = k < selected.size() && selected[k];
	if (n > 0 && (opt.keep == ShuffleOptions::Keep::First || opt.keep == ShuffleOptions::Keep::FirstAndLast))
		mv[0] = false;
	if (n > 0 && (opt.keep == ShuffleOptions::Keep::Last || opt.keep == ShuffleOptions::Keep::FirstAndLast))
		mv[n - 1] = false;
	return mv;
}

// Lay `sorted` (one lane's clips, ascending by start) out in `order`. The
// timing that is kept is the SLOTS': slot 0 starts where it started, and the
// gap that followed each slot follows it still -- including a negative gap,
// which is a transition's overlap, so transitions keep overlapping. What
// changes is which clip fills each slot; every other field of every clip is
// left exactly as it was. Returns the clips in their new slot order.
inline QVector<TlClip> applyOrder(const QVector<TlClip> &sorted, const QVector<int> &order)
{
	const int n = sorted.size();
	if (n == 0 || order.size() != n)
		return sorted;
	QVector<qint64> gapAfter(n, 0);
	for (int k = 0; k + 1 < n; ++k)
		gapAfter[k] = sorted[k + 1].outStartMs - sorted[k].outEndMs();
	QVector<TlClip> out(n);
	qint64 cursor = sorted[0].outStartMs;
	for (int k = 0; k < n; ++k) {
		out[k] = sorted[order[k]];
		out[k].outStartMs = cursor;
		// The next slot: after this clip, plus the gap the slot always had.
		// A shorter clip landing under a long transition overlap could put
		// the next start before this one; a slot never runs backwards.
		cursor = std::max(out[k].outEndMs() + gapAfter[k], cursor + 1);
	}
	return out;
}

} // namespace harpia
