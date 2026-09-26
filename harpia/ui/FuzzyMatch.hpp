#pragma once

// Fuzzy matching for the search pickers, the way Unity's Add Component and
// every editor's command palette do it: the query's characters have to appear
// in the text in order, not adjacently. "alwz" finds "Always Zoom"; "bl"
// finds Blur before Bullets; "zoom" puts Zoom above "Always Zoom".
//
// The score is what orders the results, and its parts are the things a
// person reading the list would use: a run of matching characters beats
// scattered ones, a match at the start of a word beats one in the middle,
// the query as a literal prefix or substring beats a subsequence, and a
// shorter name edges out a longer one that scores the same. Pure and small,
// so it is checked directly.

#include <QChar>
#include <QString>
#include <QVector>

#include <algorithm>

namespace harpia {

// Higher is a better match; negative means the query does not match at all.
// An empty query matches everything with a score of 0.
inline int fuzzyScore(const QString &queryIn, const QString &textIn)
{
	const QString q = queryIn.trimmed().toLower();
	if (q.isEmpty())
		return 0;
	const QString t = textIn.toLower();
	int score = 0;
	int from = 0;
	int prev = -2; // index of the previous match, -2 = none yet
	int gaps = 0;
	for (int qi = 0; qi < q.size(); ++qi) {
		const QChar c = q.at(qi);
		if (c.isSpace())
			continue; // "always zoom" and "alwayszoom" are the same ask
		const int at = t.indexOf(c, from);
		if (at < 0)
			return -1;
		if (at == prev + 1)
			score += 8; // consecutive: part of a run
		else if (prev >= 0)
			gaps += at - prev - 1;
		const bool wordStart = at == 0 || !t.at(at - 1).isLetterOrNumber() ||
				       (textIn.at(at).isUpper() && textIn.at(at - 1).isLower()); // camelCase
		if (wordStart)
			score += 10;
		score += 2;
		prev = at;
		from = at + 1;
	}
	const QString qNoSpace = QString(q).remove(QLatin1Char(' '));
	if (t.startsWith(qNoSpace))
		score += 30;
	else if (t.contains(qNoSpace))
		score += 15;
	score -= std::min(gaps, 20);
	score -= std::min(int(t.size()) / 8, 6); // a slight lean to the shorter name
	return std::max(score, 1);
}

// One thing a picker can pick. `category` groups the folder view; `pinned`
// puts it above the folders (a "Paste Blur" that applies right now).
struct PickItem {
	QString id;
	QString name;
	QString category;
	bool pinned = false;
};

// The items that match `query`, best first; ties by name. Every item when the
// query is empty, in the order given.
inline QVector<PickItem> rankItems(const QVector<PickItem> &items, const QString &query)
{
	if (query.trimmed().isEmpty())
		return items;
	QVector<QPair<int, int>> scored; // (score, index)
	for (int i = 0; i < items.size(); ++i) {
		const int s = fuzzyScore(query, items[i].name);
		if (s >= 0)
			scored.append({s, i});
	}
	std::stable_sort(scored.begin(), scored.end(), [&](const QPair<int, int> &a, const QPair<int, int> &b) {
		if (a.first != b.first)
			return a.first > b.first;
		return items[a.second].name.localeAwareCompare(items[b.second].name) < 0;
	});
	QVector<PickItem> out;
	out.reserve(scored.size());
	for (const auto &p : scored)
		out.append(items[p.second]);
	return out;
}

} // namespace harpia
