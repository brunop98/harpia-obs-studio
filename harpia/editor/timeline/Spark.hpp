#pragma once

// A Spark export is a copy of the timeline with footage blocks sequenced in
// a unique order. Planning and generation never mutate the editor's model.
#include "TimelineJson.hpp"
#include "SparkLimits.hpp"

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

inline Plan buildPlan(const TimelineModel &m)
{
	Plan plan;
	QSet<QPair<int, int>> seen;
	for (int ti = 0; ti < m.tracks.size(); ++ti) {
		const TlTrack &track = m.tracks[ti];
		if (track.hidden || track.kind != TlTrack::Kind::Video) continue;
		for (int ci = 0; ci < track.clips.size(); ++ci) {
			const TlClip &clip = track.clips[ci];
			if (seen.contains({ti, ci}) || clip.soundRule ||
			    (clip.type != TlClip::Type::Video && clip.type != TlClip::Type::Image)) continue;
			Block b;
			b.members = m.relatedClips({{ti, ci}});
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
	if (plan.blocks.isEmpty()) plan.error = QStringLiteral("Add visible video or image clips to use Spark.");
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

inline TimelineModel applyOrder(const TimelineModel &original, const Plan &plan, const QVector<int> &order)
{
	TimelineModel copy = original;
	qint64 at = 0;
	for (int index : order) {
		const Block &b = plan.blocks[index];
		for (const auto &p : b.members)
			copy.tracks[p.first].clips[p.second].outStartMs =
				original.tracks[p.first].clips[p.second].outStartMs - b.startMs + at;
		at += b.durationMs;
	}
	if (copy.hasVideoEnd())
		copy.videoEndMs = std::max(copy.videoEndMs, copy.contentDurationMs());
	return copy;
}

} // namespace harpia::spark
