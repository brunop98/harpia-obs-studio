#pragma once

// The Sound Library: named sound entries the whole app shares.
//
// Seven built-in entries (Whoosh, Pop, Click...) come with a synthesised sound
// so the feature works out of the box, and each can be pointed at a file of
// your own. You can add entries of your own too ("Riser", "Notification").
//
// Projects refer to a slot by NAME (EditorSource::librarySlot), not to the
// file behind it, so swapping Whoosh for your own recording changes it in
// every rule, every Sound component and every project that uses Whoosh.
//
// Kept in QSettings, as JSON. Pure apart from that and the synthesiser.

#include <QString>
#include <QStringList>
#include <QVector>

namespace harpia {

struct SoundSlot {
	QString name;
	QString file;         // your file; "" = the built-in sound (or nothing, for a custom slot)
	bool builtin = false; // one of SoundRules::builtinSounds(): cannot be removed, can be reset
	bool operator==(const SoundSlot &o) const
	{
		return name == o.name && file == o.file && builtin == o.builtin;
	}
};

class SoundLibrary {
public:
	// Every slot, built-ins first in their fixed order, then yours in the
	// order you added them. Built-ins are always present.
	static QVector<SoundSlot> load();
	static void save(const QVector<SoundSlot> &entries);

	// The same, from and to JSON, for the test and for save().
	static QVector<SoundSlot> fromJson(const QByteArray &json);
	static QByteArray toJson(const QVector<SoundSlot> &entries);

	static const SoundSlot *find(const QVector<SoundSlot> &entries, const QString &name);

	// The audio file a slot plays right now: your file when it is set and
	// exists; otherwise, for a built-in, its synthesised WAV (written into
	// `cacheDir` on first use). "" when the slot has nothing to play.
	static QString resolve(const QVector<SoundSlot> &entries, const QString &name, const QString &cacheDir);

	// Where the synthesised built-ins are kept.
	static QString defaultCacheDir();
};

} // namespace harpia
