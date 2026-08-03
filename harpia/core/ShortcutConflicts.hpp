#pragma once

// Which shortcuts are assigned to the same key, and therefore which of them
// will silently not work.
//
// A duplicate keyboard shortcut is the worst kind of broken setting: nothing
// reports it, nothing looks wrong, and the feature you just bound simply never
// fires -- or, on the window-scoped Qt side, NEITHER of the two fires, because
// Qt refuses to guess. So the rule this file exists to support is that a
// duplicate is never allowed to be saved: it is caught at the moment of
// assignment and has to be resolved before anything is applied.
//
// Pure and window-free, so the rule can be tested without opening a dialog --
// and so the recorder's four hotkeys and the editor's several dozen can share
// one answer instead of each having their own nearly-right version.

#include <QKeySequence>
#include <QString>
#include <QVector>

#include <map>

namespace harpia {

struct ShortcutBinding {
	QString id;      // stable identity, for writing the answer back
	QString label;   // what to call it in front of a person
	QKeySequence key; // empty means unassigned, which never conflicts
};

// Compare by portable text rather than by QKeySequence equality: two sequences
// built by different routes (parsed from a stored string, captured from the
// keyboard) must count as the same key, because they will behave as the same
// key.
inline QString shortcutKeyId(const QKeySequence &k)
{
	return k.toString(QKeySequence::PortableText);
}

// Groups of INDICES into `all` that share a key, one group per contested key.
// Only groups of two or more are returned, and unassigned bindings are never in
// one -- "nothing" is not a shortcut two things can both have.
//
// Order is deterministic: groups come out in the order their key was first
// seen, and indices within a group ascend. A dialog built from this therefore
// lists the same things in the same order every time it opens.
inline QVector<QVector<int>> shortcutConflicts(const QVector<ShortcutBinding> &all)
{
	std::map<QString, QVector<int>> byKey;
	QVector<QString> order;
	for (int i = 0; i < all.size(); ++i) {
		if (all[i].key.isEmpty())
			continue;
		const QString k = shortcutKeyId(all[i].key);
		if (byKey.find(k) == byKey.end())
			order.append(k);
		byKey[k].append(i);
	}
	QVector<QVector<int>> out;
	for (const QString &k : order) {
		const QVector<int> &g = byKey[k];
		if (g.size() > 1)
			out.append(g);
	}
	return out;
}

inline bool hasShortcutConflict(const QVector<ShortcutBinding> &all)
{
	return !shortcutConflicts(all).isEmpty();
}

// The label of whatever else already holds `k`, or an empty string when the key
// is free. `exceptId` lets a binding keep its own key without being reported as
// clashing with itself.
inline QString shortcutOwner(const QVector<ShortcutBinding> &all, const QKeySequence &k,
			     const QString &exceptId = QString())
{
	if (k.isEmpty())
		return QString();
	const QString want = shortcutKeyId(k);
	for (const ShortcutBinding &b : all)
		if (b.id != exceptId && !b.key.isEmpty() && shortcutKeyId(b.key) == want)
			return b.label;
	return QString();
}

} // namespace harpia
