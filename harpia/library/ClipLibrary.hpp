#pragma once

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace harpia {

// Metadata for one recording that exists on disk.
struct ClipInfo {
	QString filePath;
	QString fileName;
	qint64 sizeBytes = 0;
	QDateTime modified;
	QString presetName; // best-effort; empty if unknown

	// Human "time ago" string, e.g. "5 minutes ago", "3 hours ago".
	QString relativeAge() const;
	// Human file size, e.g. "12.3 MB".
	QString humanSize() const;
};

// Scans recording output folders and returns the clips found there. Used both
// for the main window's recent-10 list and the full Clip Library window.
//
// Preset attribution: a clip's `presetName` is filled in from a
// folder -> preset-name map, since each preset records into its own folder.
// (A clip whose folder matches no preset is left with an empty presetName.)
class ClipLibrary {
public:
	// Maps an output folder (absolute path) to the name of the preset that
	// writes there.
	using PresetByFolder = QHash<QString, QString>;

	// File extensions considered recordings.
	static QStringList videoExtensions();

	// All clips across the given folders, newest first.
	static QVector<ClipInfo> scan(const QStringList &folders, const PresetByFolder &presetByFolder = {});

	// The `count` most recent clips across the given folders.
	static QVector<ClipInfo> recent(const QStringList &folders, int count,
					const PresetByFolder &presetByFolder = {});
};

} // namespace harpia
