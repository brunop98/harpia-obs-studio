#pragma once

// A Spark export is a copy of the timeline with footage blocks sequenced in
// a unique order. Planning and generation never mutate the editor's model.
//
// Every footage block takes part, wherever it sits -- clips after the end
// marker are shuffled like any other. The marker stays where it is and each
// export stops there, so a block placed past it is cut off. Two orders that
// differ only past the marker are then the same video, and "unique" means
// unique up to the marker: that is what is counted and what is exported.
//
// The project's Spark settings (TlSparkSettings) and clip marks shape each
// video: an opener clip starts every video (several take turns), a range
// says how many favourites a video gets -- the ones left out are not in that
// video at all -- and a placement puts them first, spread out, anywhere, or at the
// beginning and the end. Always-included clips are favourites every video
// picks. With none of that set, Spark behaves as it always has.
#include "TimelineJson.hpp"
#include "TimelineSlice.hpp"
#include "SparkLimits.hpp"
#include "TextVariations.hpp"
#include "ClipVariations.hpp"

#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSet>
#include <QCryptographicHash>
#include <QElapsedTimer>

#include <functional>
#include <numeric>
#include <random>

namespace harpia::spark {

struct Block {
	QVector<QPair<int, int>> members;
	qint64 startMs = 0, durationMs = 0;
	bool favorite = false; // a favourite, or always included
	bool must = false;     // always included: every video picks it
	bool opener = false;   // opens the videos, in turn with any other openers
	QByteArray fingerprint;
};
struct Plan {
	TimelineModel timeline; // export snapshot: the whole timeline, end marker included
	QVector<Block> blocks;
	int favoriteCount = 0; // always-included ones counted; openers are not
	int mustCount = 0, openerCount = 0;
	qint64 uniqueOrders = 0; // different videos (orders + Random Clip choices), up to the end marker
	// Too many to count exactly within a sensible time: uniqueOrders is then
	// a lower bound, and exports are not capped by it.
	bool uniqueOrdersAtLeast = false;
	// Legacy order generation remains available separately from full videos.
	qint64 uniqueFootageOrders = 0;
	bool footageOrdersAtLeast = false;
	bool randomClips = false;
	QString error;
	bool available() const { return error.isEmpty() && !blocks.isEmpty(); }
};

inline void countRandomClipVideos(Plan &plan);

inline QByteArray fingerprint(const TimelineModel &m, const Block &b)
{
	struct Member { int track; qint64 time; QByteArray json; };
	QVector<Member> members;
	for (const auto &p : b.members) {
		TlClip c = m.tracks[p.first].clips[p.second];
		c.outStartMs -= b.startMs;
		c.group = c.link = 0;
		c.randomClipFamily.clear();
		c.favorite = c.sparkMust = c.sparkOpener = false;
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

namespace detail {

// Spark with settings: openers, a range of favourites, a placement.
inline bool configured(const Plan &plan)
{
	return plan.openerCount > 0 || !plan.timeline.spark.isDefault();
}

// The blocks as classes of interchangeable ones, by role.
struct Setup {
	QVector<QVector<int>> members; // the blocks of each class
	QVector<qint64> durationMs;
	QVector<int> look;  // shared by classes that look the same, whatever their role
	QVector<char> role; // 'S' opener, 'M' always included, 'F' favourite, 'O' ordinary
	QVector<int> openers, favorites; // classes; favorites: the ones a video may leave out
	int mustBlocks = 0, optionalBlocks = 0;
	int lo = 0, hi = 0; // favourites in each video, always-included ones counted
	TlSparkSettings::Placement placement = TlSparkSettings::Placement::First;
	qint64 endMs = 0; // where each video stops: the end marker, or 0 for none
};

inline Setup setupOf(const Plan &plan)
{
	Setup s;
	QHash<QByteArray, int> classOf, lookOf;
	for (int i = 0; i < plan.blocks.size(); ++i) {
		const Block &b = plan.blocks[i];
		const char role = b.opener ? 'S' : b.must ? 'M' : b.favorite ? 'F' : 'O';
		const QByteArray key = QByteArray(1, role) + b.fingerprint;
		auto it = classOf.find(key);
		if (it == classOf.end()) {
			it = classOf.insert(key, int(s.members.size()));
			s.members.append(QVector<int>());
			s.durationMs.append(b.durationMs);
			if (!lookOf.contains(b.fingerprint))
				lookOf.insert(b.fingerprint, int(lookOf.size()));
			s.look.append(lookOf.value(b.fingerprint));
			s.role.append(role);
			if (role == 'S')
				s.openers.append(it.value());
			else if (role == 'F')
				s.favorites.append(it.value());
		}
		s.members[it.value()].append(i);
		s.mustBlocks += role == 'M' ? 1 : 0;
		s.optionalBlocks += role == 'F' ? 1 : 0;
	}
	const TlSparkSettings &st = plan.timeline.spark;
	const int pool = s.mustBlocks + s.optionalBlocks;
	if (st.favoritesMin < 0) {
		s.lo = s.hi = pool; // every favourite
	} else {
		s.lo = std::min(st.favoritesMin, pool);
		s.hi = st.favoritesMax < 0 ? pool : std::clamp(st.favoritesMax, s.lo, pool);
	}
	// The always-included ones are in every video, whatever the range says.
	s.lo = std::max(s.lo, s.mustBlocks);
	s.hi = std::max(s.hi, s.lo);
	s.placement = st.placement;
	s.endMs = plan.timeline.hasVideoEnd() ? plan.timeline.videoEndMs : 0;
	return s;
}

// One video being put together, block by block, following the placement.
struct Walk {
	const Setup *s = nullptr;
	QVector<int> left;    // blocks of each class still to place in this video
	QVector<int> classes; // placed so far, in order
	qint64 at = 0;
	int opener = -1;
	int favorites = 0, placedFavorites = 0, front = 0;
	qint64 spreadFrom = 0, spreadTo = 0;

	bool isFavorite(int c) const { return s->role[c] == 'M' || s->role[c] == 'F'; }

	// `pick`: how many of each optional favourite class this video has.
	void start(const Setup &setup, int openerClass, const QVector<int> &pick)
	{
		s = &setup;
		opener = openerClass;
		left = QVector<int>(s->members.size(), 0);
		classes.clear();
		at = 0;
		favorites = placedFavorites = 0;
		qint64 total = 0;
		for (int c = 0; c < left.size(); ++c) {
			const char r = s->role[c];
			left[c] = r == 'S' ? 0 : r == 'F' ? pick.value(c) : int(s->members[c].size());
			favorites += isFavorite(c) ? left[c] : 0;
			total += left[c] * s->durationMs[c];
		}
		front = (favorites + 1) / 2;
		if (opener >= 0) {
			classes.append(opener);
			at = s->durationMs[opener];
			total += at;
		}
		spreadFrom = at;
		spreadTo = s->endMs > 0 ? std::min(s->endMs, total) : total;
	}
	void place(int c)
	{
		--left[c];
		classes.append(c);
		at += s->durationMs[c];
		placedFavorites += isFavorite(c) ? 1 : 0;
	}
	void unplace(int c)
	{
		++left[c];
		classes.removeLast();
		at -= s->durationMs[c];
		placedFavorites -= isFavorite(c) ? 1 : 0;
	}
	// The classes that may come next; none once the video is done -- all of
	// it placed, or the end marker reached (what is left is never seen).
	QVector<int> next() const
	{
		if (s->endMs > 0 && at >= s->endMs)
			return {};
		QVector<int> fav, ord;
		qint64 favoritesMs = 0;
		for (int c = 0; c < left.size(); ++c) {
			if (!left[c])
				continue;
			if (isFavorite(c)) {
				fav.append(c);
				favoritesMs += left[c] * s->durationMs[c];
			} else {
				ord.append(c);
			}
		}
		if (fav.isEmpty() || ord.isEmpty())
			return fav.isEmpty() ? ord : fav;
		switch (s->placement) {
		case TlSparkSettings::Placement::Spread:
			// Favourite n is due n/k of the way through what is seen.
			return at >= spreadFrom + (spreadTo - spreadFrom) * placedFavorites / favorites ? fav : ord;
		case TlSparkSettings::Placement::Ends: {
			if (placedFavorites < front)
				return fav;
			// In between: footage that still leaves the last favourites room
			// to finish before the end marker.
			QVector<int> fits;
			for (int c : ord)
				if (s->endMs <= 0 || at + s->durationMs[c] + favoritesMs <= s->endMs)
					fits.append(c);
			return fits.isEmpty() ? fav : fits;
		}
		case TlSparkSettings::Placement::Random:
			// Anywhere: favourites and the rest shuffled together.
			return fav + ord;
		case TlSparkSettings::Placement::First:
			break;
		}
		return fav;
	}
	// The blocks in timeline order: the ones placed, then whatever the end
	// marker cuts off. Blocks this video left out are not in it.
	QVector<int> order() const
	{
		QVector<int> used(s->members.size(), 0), out;
		for (int c : classes)
			out.append(s->members[c][used[c]++]);
		for (int c = 0; c < left.size(); ++c)
			for (int n = 0; n < left[c]; ++n)
				out.append(s->members[c][used[c]++]);
		return out;
	}
};

// What a viewer gets from a video: the blocks starting before the marker,
// the one reaching it compared by the part that is seen.
struct SeenKey {
	const Plan &plan;
	const Setup &s;
	QHash<QPair<int, qint64>, QByteArray> cache;
	QByteArray operator()(const Walk &w)
	{
		QByteArray key;
		qint64 t = 0;
		for (int c : w.classes) {
			const qint64 d = s.durationMs[c];
			if (s.endMs > 0 && t + d >= s.endMs) {
				const QPair<int, qint64> k{s.look[c], s.endMs - t};
				if (!cache.contains(k))
					cache.insert(k, seenFingerprint(plan.timeline, plan.blocks[s.members[c].first()], k.second));
				key += '|' + cache.value(k);
				break;
			}
			key += QByteArray::number(s.look[c]) + ',';
			t += d;
		}
		return key;
	}
};

// Every video one opener (-1: none) can start, each choice of favourites and
// each placing in turn: leaf() sees each finished walk and returns true to
// stop. Each step spends one of `budget`; running out sets *gaveUp.
inline void enumerate(const Setup &s, int opener, const std::function<bool(const Walk &)> &leaf, qint64 &budget,
		      bool *gaveUp)
{
	bool stop = false;
	QVector<int> pick(s.members.size(), 0), room(s.favorites.size() + 1, 0);
	for (int i = int(s.favorites.size()) - 1; i >= 0; --i)
		room[i] = room[i + 1] + int(s.members[s.favorites[i]].size());
	Walk w;
	std::function<void()> walk = [&]() {
		if (stop)
			return;
		if (--budget < 0) {
			stop = true;
			*gaveUp = true;
			return;
		}
		const QVector<int> next = w.next();
		if (next.isEmpty()) {
			stop = leaf(w);
			return;
		}
		for (int c : next) {
			w.place(c);
			walk();
			w.unplace(c);
			if (stop)
				return;
		}
	};
	std::function<void(int, int)> choose = [&](int i, int need) {
		if (stop || need > room[i])
			return;
		if (i == s.favorites.size()) {
			w.start(s, opener, pick);
			walk();
			return;
		}
		const int c = s.favorites[i];
		for (int n = 0; n <= std::min(need, int(s.members[c].size())) && !stop; ++n) {
			pick[c] = n;
			choose(i + 1, need - n);
		}
		pick[c] = 0;
	};
	for (int k = s.lo; k <= s.hi && !stop; ++k)
		choose(0, k - s.mustBlocks);
}

inline void forEachOpener(const Setup &s, const std::function<void(int index, int opener)> &f)
{
	if (s.openers.isEmpty())
		f(0, -1);
	for (int i = 0; i < s.openers.size(); ++i)
		f(i, s.openers[i]);
}

// More different videos than this is reported as "at least".
constexpr int kConfiguredCountLimit = 10000;

inline void countConfigured(Plan &plan)
{
	const Setup s = setupOf(plan);
	SeenKey keyOf{plan, s, {}};
	QSet<QByteArray> keys;
	qint64 budget = 200000;
	bool gaveUp = false;
	forEachOpener(s, [&](int, int opener) {
		if (gaveUp || keys.size() >= kConfiguredCountLimit)
			return;
		enumerate(s, opener, [&](const Walk &w) {
			keys.insert(keyOf(w));
			return keys.size() >= kConfiguredCountLimit;
		}, budget, &gaveUp);
	});
	plan.uniqueOrders = std::max<qint64>(1, keys.size());
	plan.uniqueOrdersAtLeast = gaveUp || keys.size() >= kConfiguredCountLimit;
}

// A random video: how many favourites and which, then each next block among
// the ones the placement allows.
inline Walk randomWalk(const Setup &s, int opener, std::mt19937_64 &rng)
{
	const int k = std::uniform_int_distribution<int>(s.lo, s.hi)(rng);
	QVector<int> bag;
	for (int c : s.favorites)
		for (int n = 0; n < s.members[c].size(); ++n)
			bag.append(c);
	std::shuffle(bag.begin(), bag.end(), rng);
	QVector<int> pick(s.members.size(), 0);
	for (int i = 0; i < k - s.mustBlocks && i < bag.size(); ++i)
		++pick[bag[i]];
	Walk w;
	w.start(s, opener, pick);
	for (QVector<int> next = w.next(); !next.isEmpty(); next = w.next()) {
		int total = 0;
		for (int c : next)
			total += w.left[c];
		int r = std::uniform_int_distribution<int>(0, total - 1)(rng);
		for (int c : next) {
			if (r < w.left[c]) {
				w.place(c);
				break;
			}
			r -= w.left[c];
		}
	}
	return w;
}

inline QVector<QVector<int>> configuredOrders(const Plan &plan, int target, std::mt19937_64 &rng)
{
	QVector<QVector<int>> out;
	const Setup s = setupOf(plan);
	SeenKey keyOf{plan, s, {}};
	QSet<QByteArray> seen;
	// Openers take turns; one with nothing new left hands over to the next.
	const int openers = std::max(1, int(s.openers.size()));
	int misses = 0;
	for (int tries = 0; tries < target * 20 + 100 && out.size() < target; ++tries) {
		const int turn = (int(out.size()) + misses) % openers;
		const Walk w = randomWalk(s, s.openers.isEmpty() ? -1 : s.openers[turn], rng);
		const QByteArray key = keyOf(w);
		if (seen.contains(key)) {
			++misses;
			continue;
		}
		seen.insert(key);
		out.append(w.order());
		misses = 0;
	}
	if (out.size() >= target)
		return out;
	// Few videos exist: go through them all, still taking the openers in turn.
	QVector<QVector<QVector<int>>> byOpener(openers);
	qint64 budget = 2000000;
	bool gaveUp = false;
	const int wanted = target - int(out.size());
	forEachOpener(s, [&](int index, int opener) {
		enumerate(s, opener, [&](const Walk &w) {
			const QByteArray key = keyOf(w);
			if (!seen.contains(key)) {
				seen.insert(key);
				byOpener[index].append(w.order());
			}
			return byOpener[index].size() >= wanted;
		}, budget, &gaveUp);
	});
	for (int round = 0; out.size() < target; ++round) {
		bool any = false;
		for (const auto &list : byOpener)
			if (round < list.size() && out.size() < target) {
				out.append(list[round]);
				any = true;
			}
		if (!any)
			break;
	}
	return out;
}

} // namespace detail

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
				b.favorite |= c.favorite || c.sparkMust;
				b.must |= c.sparkMust;
				b.opener |= c.sparkOpener;
				if (t.locked && plan.error.isEmpty())
					plan.error = QStringLiteral("Unlock track %1 before using Spark: it contains a footage block member.")
						.arg(t.name.isEmpty() ? QString::number(p.first + 1) : t.name);
			}
			b.durationMs = end - b.startMs;
			b.fingerprint = fingerprint(m, b);
			if (b.opener) {
				++plan.openerCount;
			} else {
				plan.favoriteCount += b.favorite ? 1 : 0;
				plan.mustCount += b.must ? 1 : 0;
			}
			plan.blocks.append(b);
		}
	}
	QVector<QByteArray> favorites, ordinary;
	qint64 totalMs = 0;
	for (const Block &b : plan.blocks) {
		(b.favorite ? favorites : ordinary).append(b.fingerprint);
		totalMs += b.durationMs;
	}
	if (plan.blocks.isEmpty()) {
		// nothing to count
	} else if (detail::configured(plan)) {
		detail::countConfigured(plan);
	} else if (m.hasVideoEnd() && totalMs > m.videoEndMs) {
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
	plan.uniqueFootageOrders = plan.uniqueOrders;
	plan.footageOrdersAtLeast = plan.uniqueOrdersAtLeast;
	for (const TlTrack &track : plan.timeline.tracks) {
		if (track.kind == TlTrack::Kind::Audio ? plan.timeline.trackAudible(track)
						       : !track.hidden || plan.timeline.trackAudible(track)) {
			for (const TlClip &clip : track.clips) {
				plan.randomClips |= clip.type == TlClip::Type::Video && clip.clipOptions.size() > 1;
			}
		}
	}
	if (plan.available() && plan.randomClips) {
		countRandomClipVideos(plan);
	}
	return plan;
}

inline QVector<QVector<int>> generateOrders(const Plan &plan, int requested, quint64 seed = 0)
{
	QVector<QVector<int>> out;
	if (!plan.available()) return out;
	// A lower bound does not cap the request: as many as exist are made.
	const int wanted = std::clamp(requested, 0, kMaxExports);
	const bool atLeast = plan.randomClips ? plan.footageOrdersAtLeast : plan.uniqueOrdersAtLeast;
	const qint64 count = plan.randomClips ? plan.uniqueFootageOrders : plan.uniqueOrders;
	const int target = atLeast ? wanted : int(std::min<qint64>(wanted, count));
	if (!target) return out;
	if (detail::configured(plan)) {
		std::mt19937_64 rng(seed ? seed : QRandomGenerator::system()->generate64());
		return detail::configuredOrders(plan, target, rng);
	}
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
	QVector<bool> placed(plan.blocks.size(), false);
	for (int index : order) {
		const Block &b = plan.blocks[index];
		for (const auto &p : b.members)
			copy.tracks[p.first].clips[p.second].outStartMs =
				plan.timeline.tracks[p.first].clips[p.second].outStartMs - b.startMs + at;
		at += b.durationMs;
		placed[index] = true;
	}
	// Blocks this video leaves out (favourites it did not pick, the other
	// openers) are not in it at all, linked media included.
	QVector<QVector<int>> drop(copy.tracks.size());
	bool dropped = false;
	for (int i = 0; i < plan.blocks.size(); ++i)
		if (!placed[i])
			for (const auto &p : plan.blocks[i].members) {
				drop[p.first].append(p.second);
				dropped = true;
			}
	if (!dropped)
		return copy;
	for (int ti = 0; ti < drop.size(); ++ti) {
		std::sort(drop[ti].begin(), drop[ti].end(), std::greater<int>());
		for (int ci : drop[ti])
			copy.tracks[ti].clips.removeAt(ci);
	}
	// Less footage, a shorter video: it loses the time of what it left out,
	// so what ran on after the footage (music, captions) still does, by as
	// much as before -- and it never runs past the end marker.
	qint64 footageMs = 0;
	for (const Block &b : plan.blocks)
		footageMs += b.durationMs;
	const qint64 endMs = plan.timeline.durationMs();
	const qint64 shorter = std::min(endMs, at + std::max<qint64>(0, endMs - footageMs));
	if (shorter > 0 && shorter < endMs)
		copy.videoEndMs = shorter;
	return copy;
}

struct ClipChoice {
	int track = -1, clip = -1, option = 0; // indices AFTER block placement/filtering
	bool operator==(const ClipChoice &other) const
	{
		return track == other.track && clip == other.clip && option == other.option;
	}
};
struct Video {
	QVector<int> order;
	QVector<ClipChoice> choices;
	bool operator==(const Video &other) const { return order == other.order && choices == other.choices; }
};

inline TimelineModel applyVideo(const Plan &plan, const Video &video)
{
	TimelineModel copy = applyOrder(plan, video.order);
	for (const ClipChoice &choice : video.choices) {
		if (choice.track >= 0 && choice.track < copy.tracks.size() && choice.clip >= 0 &&
		    choice.clip < copy.tracks[choice.track].clips.size()) {
			clip_variations::show(copy.tracks[choice.track].clips[choice.clip], choice.option);
		}
	}
	return copy;
}

namespace detail {
struct ClipSlot {
	int track, clip;
	QVector<int> options;
};

inline QVector<ClipSlot> clipSlots(const TimelineModel &model)
{
	QVector<ClipSlot> slotsOut;
	const qint64 end = model.durationMs();
	for (int ti = 0; ti < model.tracks.size(); ++ti) {
		const TlTrack &track = model.tracks[ti];
		if (track.kind == TlTrack::Kind::Audio ? !model.trackAudible(track)
						       : track.hidden && !model.trackAudible(track)) {
			continue;
		}
		for (int ci = 0; ci < track.clips.size(); ++ci) {
			const TlClip &clip = track.clips[ci];
			if (clip.type != TlClip::Type::Video || clip.soundRule || clip.clipOptions.size() < 2 ||
			    clip.outStartMs >= end || clip.outEndMs() <= 0) {
				continue;
			}
			ClipSlot slot{ti, ci, {}};
			QSet<QByteArray> seen;
			for (int k = 0; k < clip.clipOptions.size(); ++k) {
				const TlClipOption o = clip.clipOptionAt(k);
				const QByteArray key =
					QByteArray::number(o.sourceId) + ':' + QByteArray::number(o.srcStartMs) + ':' +
					QByteArray::number(
						track.kind == TlTrack::Kind::Audio || track.hidden ? 0.0 : o.offsetX,
						'g', 17) +
					':' +
					QByteArray::number(
						track.kind == TlTrack::Kind::Audio || track.hidden ? 0.0 : o.offsetY,
						'g', 17);
				if (!seen.contains(key)) {
					seen.insert(key);
					slot.options.append(k);
				}
			}
			slotsOut.append(slot);
		}
	}
	return slotsOut;
}

// Compare what the export actually contains. Unused alternatives, preview
// selection, group IDs, omitted favorites and content past the marker cannot
// make an otherwise identical video unique. Sorting by time also identifies
// equivalent results produced by swapping identical footage blocks.
inline QByteArray videoKey(const TimelineModel &model)
{
	const TimelineModel visible = sliceTimeline(model, 0, model.durationMs());
	QCryptographicHash hash(QCryptographicHash::Sha256);
	hash.addData(QByteArray::number(model.durationMs()) + '|');
	for (const TlTrack &track : visible.tracks) {
		if (track.kind == TlTrack::Kind::Audio ? !model.trackAudible(track)
						       : track.hidden && !model.trackAudible(track)) {
			continue;
		}
		hash.addData(QByteArray::number(int(track.kind)) + '|');
		QVector<TlClip> clips = track.clips;
		std::stable_sort(clips.begin(), clips.end(),
				 [](const TlClip &a, const TlClip &b) { return a.outStartMs < b.outStartMs; });
		for (TlClip &clip : clips) {
			clip.group = clip.link = 0;
			clip.randomClipFamily.clear();
			clip.favorite = clip.sparkMust = clip.sparkOpener = false;
			if (clip.type == TlClip::Type::Video) {
				TlClipOption chosen = clip.clipOptionAt(clip.clipShown);
				if (track.kind == TlTrack::Kind::Audio || track.hidden) {
					chosen.offsetX = chosen.offsetY = 0;
				}
				clip.clipOptions = {chosen};
				clip.clipShown = 0;
			}
			const QByteArray json = QJsonDocument(clipToJson(clip)).toJson(QJsonDocument::Compact);
			hash.addData(QByteArray::number(json.size()) + ':' + json);
		}
		hash.addData(QByteArray(";"));
	}
	return hash.result();
}

struct ChoiceBudget {
	qint64 steps;
	int maxMs;
	QElapsedTimer clock;
	bool exhausted = false;
	ChoiceBudget(qint64 n, int ms) : steps(n), maxMs(ms) { clock.start(); }
	bool spend()
	{
		exhausted |= --steps < 0 || clock.elapsed() >= maxMs;
		return !exhausted;
	}
};

inline bool enumerateChoices(TimelineModel &model, const QVector<ClipSlot> &slotsIn, int at, Video &video,
			     ChoiceBudget &budget,
			     const std::function<bool(const TimelineModel &, const Video &)> &leaf)
{
	if (!budget.spend()) {
		return true;
	}
	if (at == slotsIn.size()) {
		return leaf(model, video);
	}
	const auto &slot = slotsIn[at];
	const TlClip original = model.tracks[slot.track].clips[slot.clip];
	for (int option : slot.options) {
		clip_variations::show(model.tracks[slot.track].clips[slot.clip], option);
		video.choices.append({slot.track, slot.clip, option});
		const bool stop = enumerateChoices(model, slotsIn, at + 1, video, budget, leaf);
		video.choices.removeLast();
		model.tracks[slot.track].clips[slot.clip] = original;
		if (stop) {
			return true;
		}
	}
	return false;
}

inline void enumerateVideos(const Plan &plan, ChoiceBudget &budget,
			    const std::function<bool(const TimelineModel &, const Video &)> &leaf)
{
	const Setup setup = setupOf(plan);
	bool stop = false, gaveUp = false;
	forEachOpener(setup, [&](int, int opener) {
		if (stop || gaveUp) {
			return;
		}
		enumerate(
			setup, opener,
			[&](const Walk &walk) {
				Video video{walk.order(), {}};
				TimelineModel model = applyOrder(plan, video.order);
				stop = enumerateChoices(model, clipSlots(model), 0, video, budget, leaf);
				return stop;
			},
			budget.steps, &gaveUp);
	});
	budget.exhausted |= gaveUp;
}
} // namespace detail

inline void countRandomClipVideos(Plan &plan)
{
	QSet<QByteArray> keys;
	detail::ChoiceBudget budget(200000, 300);
	detail::enumerateVideos(plan, budget, [&](const TimelineModel &model, const Video &) {
		keys.insert(detail::videoKey(model));
		return keys.size() >= detail::kConfiguredCountLimit;
	});
	plan.uniqueOrders = std::max<qint64>(1, keys.size());
	plan.uniqueOrdersAtLeast = budget.exhausted || keys.size() >= detail::kConfiguredCountLimit;
}

// One selected option per surviving Random Clip, plus a block order. Choose
// both together: selecting footage only AFTER deduplicating orders would cap
// exports too early and could create repeated videos.
inline QVector<Video> generateVideos(const Plan &plan, int requested, quint64 seed = 0)
{
	QVector<Video> out;
	if (!plan.available()) {
		return out;
	}
	if (!plan.randomClips) {
		for (const auto &order : generateOrders(plan, requested, seed)) {
			out.append({order, {}});
		}
		return out;
	}
	const int wanted = std::clamp(requested, 0, kMaxExports);
	const int target = plan.uniqueOrdersAtLeast ? wanted : int(std::min<qint64>(wanted, plan.uniqueOrders));
	if (!target) {
		return out;
	}
	std::mt19937_64 rng(seed ? seed : QRandomGenerator::system()->generate64());
	const detail::Setup setup = detail::setupOf(plan);
	QSet<QByteArray> seen;
	const auto add = [&](const TimelineModel &model, const Video &video) {
		const auto key = detail::videoKey(model);
		if (!seen.contains(key)) {
			seen.insert(key);
			out.append(video);
		}
		return out.size() >= target;
	};
	detail::ChoiceBudget budget(2000000, 5000);
	int misses = 0;
	for (int tries = 0; tries < target * 20 + 100 && out.size() < target && budget.spend(); ++tries) {
		const int opener =
			setup.openers.isEmpty() ? -1 : setup.openers[(int(out.size()) + misses) % setup.openers.size()];
		Video video{detail::randomWalk(setup, opener, rng).order(), {}};
		TimelineModel model = applyOrder(plan, video.order);
		for (const auto &slot : detail::clipSlots(model)) {
			const int index = std::uniform_int_distribution<int>(0, int(slot.options.size()) - 1)(rng);
			const int option = slot.options[index];
			clip_variations::show(model.tracks[slot.track].clips[slot.clip], option);
			video.choices.append({slot.track, slot.clip, option});
		}
		const int before = int(out.size());
		add(model, video);
		misses = out.size() == before ? misses + 1 : 0;
	}
	if (out.size() < target && !budget.exhausted) {
		detail::enumerateVideos(plan, budget, add);
	}
	return out;
}

} // namespace harpia::spark
