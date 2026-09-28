#pragma once

// Presets: a project's tags and sound rules, saved under a name and merged
// into any other project ("YouTube tutorial", "Product demo").
//
// A preset never stores project-specific ids. A tag is written by name (and
// colour), a Tagged rule refers to its tag by name, and a rule's sound is
// written as a Sound Library slot when it came from one ("Whoosh"), or as the
// file's path otherwise. A lane restriction is dropped: lane 3 of one
// project means nothing in another.
//
// Loading MERGES: tags match by name (case-insensitive) so nothing doubles,
// and a rule identical to one the project already has is skipped.
//
// Pure over the model; the window supplies the two lookups between media-pool
// ids and sound references. The store is a folder of JSON files.

#include "TimelineModel.hpp"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace harpia {

// How a preset names a sound: a library slot, or else a file.
struct SoundRef {
	QString slot;
	QString file;
	bool isEmpty() const { return slot.isEmpty() && file.isEmpty(); }
};

struct PresetMergeResult {
	int tagsAdded = 0;
	int rulesAdded = 0;
	int rulesSkipped = 0;   // already in the project
	int soundsMissing = 0;  // a rule whose sound could not be found here
};

class SoundPresets {
public:
	static QJsonObject toJson(const TimelineModel &m, const QString &name,
				  const std::function<SoundRef(int sourceId)> &refOf);

	// Merge a preset into `m`. `sourceFor` turns a sound reference into a
	// media-pool id in this project (registering the file if needed), or -1.
	static PresetMergeResult merge(TimelineModel &m, const QJsonObject &preset,
				       const std::function<int(const SoundRef &)> &sourceFor);

	// The store: <app data>/presets/<name>.json.
	static QString dir();
	static QStringList names();                       // sorted, case-insensitively
	static bool save(const QString &name, const QJsonObject &preset);
	static QJsonObject load(const QString &name);     // empty when missing
	static bool remove(const QString &name);
	static QString fileFor(const QString &name);      // a safe file name for any preset name
	// For the test: point the store somewhere else. Empty = the default.
	static void setDirForTesting(const QString &dir);
};

} // namespace harpia
