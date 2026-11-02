#pragma once

// A Spark export is a copy of the timeline with footage blocks sequenced in
// a unique order. Planning and generation never mutate the editor's model.
#include "TimelineJson.hpp"
#include "TimelineSlice.hpp"
#include "SparkLimits.hpp"
#include "TextVariations.hpp"

#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSet>

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
	TimelineModel timeline; // export snapshot, cut at the project end before planning
	QVector<Block> blocks;
	int favoriteCount = 0;
	qint64 uniqueOrders = 0;
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

inline Plan buildPlan(const TimelineModel &original)
{
	Plan plan;
	plan.timeline = original.hasVideoEnd() ? sliceTimeline(original, 0, original.videoEndMs) : original;
	// Range slicing omits project metadata; Spark still needs tags and sound
	// rules when the exporter rebuilds derived sounds after rearranging footage.
	plan.timeline.tags = original.tags;
	plan.timeline.soundRules = original.soundRules;
	const TimelineModel &m = plan.timeline;
	QHash<QPair<int, int>, QPair<int, int>> kept;
	for (int ti = 0; ti < original.tracks.size(); ++ti) {
		int next = 0;
		for (int ci = 0; ci < original.tracks[ti].clips.size(); ++ci) {
			const TlClip &c = original.tracks[ti].clips[ci];
			if (original.hasVideoEnd() && (c.outStartMs >= original.videoEndMs || c.outEndMs() <= 0))
				continue;
			kept.insert({ti, ci}, {ti, next++});
		}
	}
	QSet<QPair<int, int>> seen;
	for (int ti = 0; ti < original.tracks.size(); ++ti) {
		const TlTrack &track = original.tracks[ti];
		if (track.hidden || track.kind != TlTrack::Kind::Video) continue;
		for (int ci = 0; ci < track.clips.size(); ++ci) {
			const TlClip &clip = track.clips[ci];
			if (!kept.contains({ti, ci}) || seen.contains(kept.value({ti, ci})) || clip.soundRule ||
			    (clip.type != TlClip::Type::Video && clip.type != TlClip::Type::Image)) continue;
			Block b;
			// Resolve membership before cutting, so removing a linked member
			// beyond the marker cannot split the remaining part of its block.
			for (const auto &p : original.relatedClips({{ti, ci}}))
				if (kept.contains(p))
					b.members.append(kept.value(p));
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
	for (const Block &b : plan.blocks)
		(b.favorite ? favorites : ordinary).append(b.fingerprint);
	plan.uniqueOrders = plan.blocks.isEmpty() ? 0 :
		std::min(kOrderCountCap, permutationCount(favorites) * permutationCount(ordinary));
	if (plan.blocks.isEmpty())
		plan.error = original.hasVideoEnd()
			? QStringLiteral("Add visible video or image clips before the end marker to use Spark.")
			: QStringLiteral("Add visible video or image clips to use Spark.");
	return plan;
}

inline QVector<QVector<int>> generateOrders(const Plan &plan, int requested, quint64 seed = 0)
{
	QVector<QVector<int>> out;
	if (!plan.available()) return out;
	const int target = int(std::min<qint64>(std::clamp(requested, 0, kMaxExports), plan.uniqueOrders));
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
	QSet<QVector<int>> seen;
	const auto add = [&](const QVector<int> &f, const QVector<int> &o) {
		QVector<int> key = f + o;
		if (seen.contains(key)) return;
		seen.insert(key);
		QVector<int> order, used(members.size(), 0);
		for (int id : key) order.append(members[id][used[id]++]);
		out.append(order);
	};
	std::mt19937_64 rng(seed ? seed : QRandomGenerator::system()->generate64());
	for (int tries = 0; tries < target * 20 + 100 && out.size() < target; ++tries) {
		std::shuffle(favorites.begin(), favorites.end(), rng);
		std::shuffle(ordinary.begin(), ordinary.end(), rng);
		add(favorites, ordinary);
	}
	// Finite fallback near exhaustion: no endless retry loop for a tiny space.
	std::sort(favorites.begin(), favorites.end());
	std::sort(ordinary.begin(), ordinary.end());
	const auto firstOrdinary = ordinary;
	do {
		ordinary = firstOrdinary;
		do {
			if (out.size() >= target) return out;
			add(favorites, ordinary);
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
