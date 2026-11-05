#pragma once

// A Spark export is a copy of the timeline with footage blocks sequenced in
// a unique order. Planning and generation never mutate the editor's model.
//
// Every footage block takes part, wherever it sits -- clips after the end
// marker are shuffled like any other. The marker stays where it is and each
// export stops there, so a block placed past it is cut off. Two orders that
// differ only past the marker are then the same video, and "unique" means
// unique up to the marker: that is what is counted and what is exported.
#include "TimelineJson.hpp"
#include "TimelineSlice.hpp"
#include "SparkLimits.hpp"
#include "TextVariations.hpp"

#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSet>

#include <functional>
#include <numeric>
#include <random>

namespace harpia::spark {

struct Block {
	QVector<QPair<int, int>> members;
	qint64 startMs = 0, durationMs = 0;
	bool favorite = false;
	QByteArray fingerprint;
};
struct Plan {
	TimelineModel timeline; // export snapshot: the whole timeline, end marker included
	QVector<Block> blocks;
	int favoriteCount = 0;
	qint64 uniqueOrders = 0; // different videos, up to the end marker
	// Too many to count exactly within a sensible time: uniqueOrders is then
	// a lower bound, and exports are not capped by it.
	bool uniqueOrdersAtLeast = false;
	QString error;
	bool available() const { return error.isEmpty() && !blocks.isEmpty(); }
};

inline QByteArray fingerprint(const TimelineModel &m, const Block &b)
{
	struct Member { int track; qint64 time; QByteArray json; };
	QVector<Member> members;
	for (const auto &p : b.members) {
		TlClip c = m.tracks[p.first].clips[p.second];
		c.outStartMs -= b.startMs;
		c.group = c.link = 0;
		c.favorite = false;
		QJsonObject o = clipToJson(c);
		o[QStringLiteral("track")] = p.first;
		members.append({p.first, c.outStartMs, QJsonDocument(o).toJson(QJsonDocument::Compact)});
	}
	std::sort(members.begin(), members.end(), [](const Member &a, const Member &b) {
		if (a.track != b.track) return a.track < b.track;
		if (a.time != b.time) return a.time < b.time;
		return a.json < b.json;
	});
	QByteArray out;
	for (const Member &c : members) {
		out += QByteArray::number(c.json.size()) + ':' + c.json;
	}
	return out;
}

// A block as it is seen when the end marker leaves only its first `keepMs`:
// two blocks that differ only in the part cut off make the same video.
inline QByteArray seenFingerprint(const TimelineModel &m, const Block &b, qint64 keepMs)
{
	if (keepMs >= b.durationMs)
		return b.fingerprint;
	TimelineModel only; // just this block, starting at zero
	for (const TlTrack &t : m.tracks) {
		TlTrack empty = t;
		empty.clips.clear();
		only.tracks.append(empty);
	}
	for (const auto &p : b.members) {
		TlClip c = m.tracks[p.first].clips[p.second];
		c.outStartMs -= b.startMs;
		only.tracks[p.first].clips.append(c);
	}
	const TimelineModel cut = sliceTimeline(only, 0, keepMs);
	Block seen;
	for (int ti = 0; ti < cut.tracks.size(); ++ti)
		for (int ci = 0; ci < cut.tracks[ti].clips.size(); ++ci)
			seen.members.append({ti, ci});
	return fingerprint(cut, seen);
}

inline qint64 cappedChoose(int n, int k)
{
	k = std::min(k, n - k);
	qint64 count = 1;
	for (int i = 1; i <= k; ++i) {
		// count is capped before the next multiplication; n is an int.
		count = count * (n - k + i) / i;
		if (count >= kOrderCountCap) return kOrderCountCap;
	}
	return count;
}

inline qint64 permutationCount(const QVector<QByteArray> &keys)
{
	QMap<QByteArray, int> counts;
	for (const QByteArray &key : keys) ++counts[key];
	qint64 count = 1;
	int total = 0;
	for (int n : counts) {
		count = std::min(kOrderCountCap, count * cappedChoose(total + n, n));
		total += n;
	}
	return count;
}

// One kind of interchangeable block, for counting what the end marker leaves.
struct VisibleClass {
	qint64 durationMs = 0;
	int count = 0;
	bool favorite = false;
};

// How many different videos the end marker leaves. Blocks run end to end from
// zero with the favourites first, and a block is seen when it starts before
// the marker, so orders that differ only past it look the same -- and so do
// two blocks at the marker whose parts before it match (`seenKey(class, ms
// left)` says what a class looks like cut there). Counted over classes of
// interchangeable blocks and memoised on what is left to place, which also
// fixes the time already used. Past `budget` new states it stops exploring,
// and the answer is a lower bound (*atLeast).
inline qint64 visibleOrderCount(const QVector<VisibleClass> &classes, qint64 endMs,
				const std::function<QByteArray(int, qint64)> &seenKey, bool *atLeast,
				int budget = 200000)
{
	QVector<int> left;
	for (const VisibleClass &c : classes)
		left.append(c.count);
	QHash<QVector<int>, qint64> memo;
	int expanded = 0;
	bool gaveUp = false;
	std::function<qint64(qint64)> count = [&](qint64 at) -> qint64 {
		if (at >= endMs)
			return 1; // everything still to place is past the marker
		if (const auto it = memo.constFind(left); it != memo.cend())
			return it.value();
		if (++expanded > budget) {
			gaveUp = true;
			return 0;
		}
		bool favoritesLeft = false;
		for (int i = 0; i < classes.size(); ++i)
			favoritesLeft |= classes[i].favorite && left[i] > 0;
		qint64 total = 0;
		bool any = false;
		QSet<QByteArray> lastSeen; // the different ways the marker can be reached from here
		for (int i = 0; i < classes.size(); ++i) {
			if (!left[i] || (favoritesLeft && !classes[i].favorite))
				continue;
			any = true;
			if (at + classes[i].durationMs >= endMs) {
				lastSeen.insert(seenKey(i, endMs - at));
				continue;
			}
			--left[i];
			const qint64 n = count(at + classes[i].durationMs);
			++left[i];
			total = std::min(kOrderCountCap, total + n);
		}
		total = std::min<qint64>(kOrderCountCap, total + lastSeen.size());
		if (!any)
			total = 1; // every block placed, all of it before the marker
		memo.insert(left, total);
		return total;
	};
	const qint64 n = std::max<qint64>(1, count(0));
	if (atLeast)
		*atLeast = gaveUp;
	return n;
}

inline Plan buildPlan(const TimelineModel &original)
{
	Plan plan;
	// The whole timeline, end marker and all: footage after the marker takes
	// part in the shuffle, and each export still stops at the marker.
	plan.timeline = original;
	const TimelineModel &m = plan.timeline;
	QSet<QPair<int, int>> seen;
	for (int ti = 0; ti < original.tracks.size(); ++ti) {
		const TlTrack &track = original.tracks[ti];
		if (track.hidden || track.kind != TlTrack::Kind::Video) continue;
		for (int ci = 0; ci < track.clips.size(); ++ci) {
			const TlClip &clip = track.clips[ci];
			if (seen.contains({ti, ci}) || clip.soundRule ||
			    (clip.type != TlClip::Type::Video && clip.type != TlClip::Type::Image)) continue;
			Block b;
			b.members = original.relatedClips({{ti, ci}});
			b.startMs = std::numeric_limits<qint64>::max();
			qint64 end = 0;
			for (const auto &p : b.members) {
				seen.insert(p);
				const TlTrack &t = m.tracks[p.first];
				const TlClip &c = t.clips[p.second];
				b.startMs = std::min(b.startMs, c.outStartMs);
				end = std::max(end, c.outEndMs());
				b.favorite |= c.favorite;
				if (t.locked && plan.error.isEmpty())
					plan.error = QStringLiteral("Unlock track %1 before using Spark: it contains a footage block member.")
						.arg(t.name.isEmpty() ? QString::number(p.first + 1) : t.name);
			}
			b.durationMs = end - b.startMs;
			b.fingerprint = fingerprint(m, b);
			plan.favoriteCount += b.favorite ? 1 : 0;
			plan.blocks.append(b);
		}
	}
	QVector<QByteArray> favorites, ordinary;
	qint64 totalMs = 0;
	for (const Block &b : plan.blocks) {
		(b.favorite ? favorites : ordinary).append(b.fingerprint);
		totalMs += b.durationMs;
	}
	if (!plan.blocks.isEmpty() && m.hasVideoEnd() && totalMs > m.videoEndMs) {
		// The marker cuts the sequence: count the videos, not the orders.
		QMap<QByteArray, int> classOf;
		QVector<VisibleClass> classes;
		QVector<int> example; // a block of each class
		for (int i = 0; i < plan.blocks.size(); ++i) {
			const Block &b = plan.blocks[i];
			const QByteArray key = (b.favorite ? QByteArray("F") : QByteArray("O")) + b.fingerprint;
			if (!classOf.contains(key)) {
				classOf.insert(key, classes.size());
				classes.append({b.durationMs, 0, b.favorite});
				example.append(i);
			}
			++classes[classOf.value(key)].count;
		}
		QHash<QPair<int, qint64>, QByteArray> cache;
		const auto seenKey = [&](int cls, qint64 keepMs) {
			const QPair<int, qint64> k{cls, keepMs};
			if (!cache.contains(k))
				cache.insert(k, seenFingerprint(m, plan.blocks[example[cls]], keepMs));
			return cache.value(k);
		};
		plan.uniqueOrders = visibleOrderCount(classes, m.videoEndMs, seenKey, &plan.uniqueOrdersAtLeast);
	} else {
		plan.uniqueOrders = plan.blocks.isEmpty() ? 0 :
			std::min(kOrderCountCap, permutationCount(favorites) * permutationCount(ordinary));
	}
	if (plan.blocks.isEmpty())
		plan.error = QStringLiteral("Add visible video or image clips to use Spark.");
	return plan;
}

inline QVector<QVector<int>> generateOrders(const Plan &plan, int requested, quint64 seed = 0)
{
	QVector<QVector<int>> out;
	if (!plan.available()) return out;
	// A lower bound does not cap the request: as many as exist are made.
	const int wanted = std::clamp(requested, 0, kMaxExports);
	const int target = plan.uniqueOrdersAtLeast ? wanted : int(std::min<qint64>(wanted, plan.uniqueOrders));
	if (!target) return out;
	// Each class appears as many times as its equivalent blocks. Swapping
	// copies within a class is the same order, including during enumeration.
	QMap<QByteArray, QVector<int>> classes;
	for (int i = 0; i < plan.blocks.size(); ++i) {
		const Block &b = plan.blocks[i];
		classes[(b.favorite ? QByteArray("F") : QByteArray("O")) + b.fingerprint].append(i);
	}
	QVector<QVector<int>> members;
	QVector<int> favorites, ordinary;
	for (auto it = classes.cbegin(); it != classes.cend(); ++it) {
		const int id = members.size();
		members.append(it.value());
		for (int i : it.value())
			(plan.blocks[i].favorite ? favorites : ordinary).append(id);
	}
	// With an end marker that cuts the sequence, only the blocks starting
	// before it are seen: that part is what makes one export differ from
	// another. Without one, the whole order is.
	const qint64 endMs = plan.timeline.hasVideoEnd() ? plan.timeline.videoEndMs : 0;
	qint64 totalMs = 0;
	for (const Block &b : plan.blocks)
		totalMs += b.durationMs;
	const bool cut = endMs > 0 && totalMs > endMs;
	const auto durationOf = [&](int id) { return plan.blocks[members[id].first()].durationMs; };
	QHash<QPair<int, qint64>, QByteArray> cutCache;
	// What a viewer gets from an order: every block that starts before the
	// marker, the one reaching it as far as the marker lets it be seen.
	const auto seenPart = [&](const QVector<int> &ids) {
		QByteArray key;
		qint64 at = 0;
		for (int id : ids) {
			if (cut && at >= endMs)
				break;
			const qint64 d = durationOf(id);
			if (cut && at + d >= endMs) {
				// The last one seen, compared by what is seen of it -- the same
				// rule the count uses (visibleOrderCount's seenKey).
				const QPair<int, qint64> k{id, endMs - at};
				if (!cutCache.contains(k))
					cutCache.insert(k, seenFingerprint(plan.timeline, plan.blocks[members[id].first()], endMs - at));
				key += '|' + cutCache.value(k);
				break;
			}
			key += QByteArray::number(id) + ',';
			at += d;
		}
		return key;
	};
	QSet<QByteArray> seen;
	const auto add = [&](const QVector<int> &ids) {
		const QByteArray key = seenPart(ids);
		if (seen.contains(key)) return;
		seen.insert(key);
		QVector<int> order, used(members.size(), 0);
		for (int id : ids) order.append(members[id][used[id]++]);
		out.append(order);
	};
	std::mt19937_64 rng(seed ? seed : QRandomGenerator::system()->generate64());
	for (int tries = 0; tries < target * 20 + 100 && out.size() < target; ++tries) {
		std::shuffle(favorites.begin(), favorites.end(), rng);
		std::shuffle(ordinary.begin(), ordinary.end(), rng);
		add(favorites + ordinary);
	}
	std::sort(favorites.begin(), favorites.end());
	std::sort(ordinary.begin(), ordinary.end());
	if (cut) {
		// Every distinct seen part in turn, favourites first, each finished
		// with the rest in a fixed order. It stops once enough are found, so
		// it visits at most the parts already found plus the ones still wanted
		// -- never all the orderings of the blocks nobody sees.
		QVector<int> left(members.size(), 0);
		QVector<bool> favorite(members.size(), false);
		for (int id : favorites) {
			++left[id];
			favorite[id] = true;
		}
		for (int id : ordinary)
			++left[id];
		QVector<int> part;
		std::function<void(qint64)> walk = [&](qint64 at) {
			if (out.size() >= target) return;
			bool favoritesLeft = false, any = false;
			for (int id = 0; id < left.size(); ++id)
				favoritesLeft |= favorite[id] && left[id] > 0;
			if (at < endMs)
				for (int id = 0; id < left.size() && out.size() < target; ++id) {
					if (!left[id] || (favoritesLeft && !favorite[id])) continue;
					any = true;
					--left[id];
					part.append(id);
					walk(at + durationOf(id));
					part.removeLast();
					++left[id];
				}
			if (any) return;
			QVector<int> ids = part;
			for (int pass = 0; pass < 2; ++pass)
				for (int id = 0; id < left.size(); ++id)
					if (favorite[id] == (pass == 0))
						for (int k = 0; k < left[id]; ++k) ids.append(id);
			add(ids);
		};
		walk(0);
		return out;
	}
	// Finite fallback near exhaustion: no endless retry loop for a tiny space.
	const auto firstOrdinary = ordinary;
	do {
		ordinary = firstOrdinary;
		do {
			if (out.size() >= target) return out;
			add(favorites + ordinary);
		} while (std::next_permutation(ordinary.begin(), ordinary.end()));
	} while (std::next_permutation(favorites.begin(), favorites.end()));
	return out;
}

// One independent text choice per Random Text clip for this export. This does
// not enumerate combinations or change the footage-order count. Passing the
// timeline by value keeps the editor and the shared plan unchanged.
inline TimelineModel pickRandomTexts(TimelineModel timeline, std::mt19937_64 &rng)
{
	for (TlTrack &track : timeline.tracks)
		for (TlClip &clip : track.clips) {
			if (clip.type != TlClip::Type::Text || !clip.text.random)
				continue;
			const QStringList options = text_variations::optionsOf(clip.text);
			if (options.size() < 2)
				continue;
			std::uniform_int_distribution<int> pick(0, int(options.size()) - 1);
			const QString selected = options[pick(rng)];
			if (selected != clip.text.text) {
				clip.text.text = selected;
				clip.words.clear(); // spoken word timings belong to the previous text
			}
		}
	return timeline;
}

inline TimelineModel applyOrder(const Plan &plan, const QVector<int> &order)
{
	TimelineModel copy = plan.timeline;
	qint64 at = 0;
	for (int index : order) {
		const Block &b = plan.blocks[index];
		for (const auto &p : b.members)
			copy.tracks[p.first].clips[p.second].outStartMs =
				plan.timeline.tracks[p.first].clips[p.second].outStartMs - b.startMs + at;
		at += b.durationMs;
	}
	return copy;
}

} // namespace harpia::spark
