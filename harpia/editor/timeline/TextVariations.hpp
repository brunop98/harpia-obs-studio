#pragma once

// Text variations: the same video, several versions, only the words differ.
//
// A text clip keeps its own text and may carry a list of alternatives
// (TlText::variations). Every combination of every varying clip is one video:
// two clips with four and three options make twelve. Version 1 is always the
// project exactly as edited (every clip on its own text); after that the FIRST
// varying clip on the timeline changes slowest and the last one fastest, the
// way an odometer counts, so a single varying clip gives its options in the
// order they were typed.
//
// Everything here is pure -- a timeline in, a timeline (or a name) out -- so
// the counting, the substitution and the file names can be checked without
// rendering anything. The export renders applyCombination()'s timeline through
// the ordinary Full-editing path; nothing else about the video changes.

#include "TimelineModel.hpp"

#include <QRegularExpression>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <limits>

namespace harpia {

namespace text_variations {

// One text clip that has alternatives. options[0] is the clip's own text.
struct Slot {
	int track = -1;
	int clip = -1;
	QStringList options;
};

// A clip's options: its own text first, then every non-blank alternative.
// Blank lines are dropped, so a stray empty line in the list is not a version
// with no caption.
inline QStringList optionsOf(const TlText &t)
{
	QStringList out{t.text};
	for (const QString &v : t.variations)
		if (!v.trimmed().isEmpty())
			out << v;
	return out;
}

// The varying text clips, in timeline order: tracks top to bottom, then clips
// by where they start.
inline QVector<Slot> slotsOf(const TimelineModel &m)
{
	QVector<Slot> out;
	for (int ti = 0; ti < m.tracks.size(); ++ti) {
		const TlTrack &t = m.tracks[ti];
		QVector<int> order;
		for (int ci = 0; ci < t.clips.size(); ++ci)
			if (t.clips[ci].type == TlClip::Type::Text && optionsOf(t.clips[ci].text).size() > 1)
				order.push_back(ci);
		std::stable_sort(order.begin(), order.end(),
				 [&t](int a, int b) { return t.clips[a].outStartMs < t.clips[b].outStartMs; });
		for (int ci : order)
			out.push_back({ti, ci, optionsOf(t.clips[ci].text)});
	}
	return out;
}

// How many videos: the product of the option counts. Saturates instead of
// overflowing (the tab refuses anything that large long before it matters).
inline qint64 combinationCount(const QVector<Slot> &vs)
{
	if (vs.isEmpty())
		return 0;
	qint64 n = 1;
	for (const Slot &s : vs) {
		const qint64 k = s.options.size();
		if (n > std::numeric_limits<qint64>::max() / std::max<qint64>(1, k))
			return std::numeric_limits<qint64>::max();
		n *= k;
	}
	return n;
}

// Which option each slot uses in video `index` (0-based): mixed radix, the
// last slot fastest.
inline QVector<int> combinationAt(const QVector<Slot> &vs, qint64 index)
{
	QVector<int> pick(vs.size(), 0);
	for (int i = vs.size() - 1; i >= 0; --i) {
		const qint64 k = std::max<qint64>(1, vs[i].options.size());
		pick[i] = int(index % k);
		index /= k;
	}
	return pick;
}

// The timeline for one video: each varying clip's text replaced by its pick.
// Only `text` changes; style, position, keys, components and the alternatives
// list itself are untouched.
inline TimelineModel applyCombination(const TimelineModel &m, const QVector<Slot> &vs, const QVector<int> &pick)
{
	TimelineModel out = m;
	for (int i = 0; i < vs.size() && i < pick.size(); ++i) {
		const Slot &s = vs[i];
		if (s.track < 0 || s.track >= out.tracks.size() || s.clip < 0 ||
		    s.clip >= out.tracks[s.track].clips.size())
			continue;
		const int k = std::clamp(pick[i], 0, int(s.options.size()) - 1);
		TlClip &c = out.tracks[s.track].clips[s.clip];
		c.text.text = s.options[k];
		// Word timings belong to the text they were made for.
		if (k != 0)
			c.words.clear();
	}
	return out;
}

// The texts in one video, for the checklist and the file name.
inline QStringList textsAt(const QVector<Slot> &vs, const QVector<int> &pick)
{
	QStringList out;
	for (int i = 0; i < vs.size() && i < pick.size(); ++i)
		out << vs[i].options.value(pick[i]);
	return out;
}

// A piece of text made safe for a file name: one line, spaces as dashes, the
// characters Windows refuses dropped, cut to `maxLen` at a dash when it can.
inline QString slug(const QString &text, int maxLen = 24)
{
	QString s = rich_text::plainText(text).simplified();
	s.remove(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")));
	s.replace(QLatin1Char(' '), QLatin1Char('-'));
	while (s.contains(QStringLiteral("--")))
		s.replace(QStringLiteral("--"), QStringLiteral("-"));
	if (s.size() > maxLen) {
		s.truncate(maxLen);
		const int dash = s.lastIndexOf(QLatin1Char('-'));
		if (dash >= maxLen / 2)
			s.truncate(dash); // end on a whole word when that loses little
	}
	while (s.endsWith(QLatin1Char('-')) || s.endsWith(QLatin1Char('.')))
		s.chop(1);
	while (s.startsWith(QLatin1Char('-')) || s.startsWith(QLatin1Char('.')))
		s.remove(0, 1);
	return s;
}

// "Prefix_03_I-like-cats_Blue-sky": the prefix, the number (zero-padded to the
// batch's width, at least two digits), then each varying clip's text shortened.
// The whole name stays within `maxLen`; texts are dropped from the end first,
// so the number -- which keeps names unique -- is never cut.
inline QString fileName(const QString &safePrefixText, int index1, int count, const QStringList &texts,
			int maxLen = 120)
{
	int digits = 1;
	for (int n = std::max(1, count); n >= 10; n /= 10)
		++digits;
	digits = std::max(2, digits);
	QString name = QStringLiteral("%1_%2").arg(safePrefixText).arg(index1, digits, 10, QLatin1Char('0'));
	for (const QString &t : texts) {
		const QString part = slug(t);
		if (part.isEmpty())
			continue;
		if (name.size() + 1 + part.size() > maxLen)
			break;
		name += QLatin1Char('_') + part;
	}
	return name;
}

} // namespace text_variations

} // namespace harpia
