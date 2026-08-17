#pragma once

// Where projects live.
//
// A saved project used to land wherever the source video happened to be --
// Save As suggested "<the folder your video is in>/<video>_edit.harpiaproj" and
// Open started there too. That is fine for one clip and wrong for everything
// else: a project drawing on four files from four folders has no such place,
// projects end up scattered through the user's video collection, and there is
// nowhere to look for "the thing I was working on last week".
//
// So there is one base folder, beside the recordings folder and made the same
// way: <Movies>/Harpia/Projects, falling back to the home directory on a system
// with no Movies location.
//
// Header-only and free of Qt widgets, so the naming rules below can be tested
// without a window and used from main() and the editor alike.

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>

#include <functional>

namespace harpia {

// The base folder for recordings AND projects: <Movies>/Harpia.
inline QString harpiaBaseFolder()
{
	QString base = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	if (base.isEmpty())
		base = QDir::homePath();
	return QDir(base).filePath(QStringLiteral("Harpia"));
}

// <Movies>/Harpia/Projects. Not created by this call -- see ensure() below --
// because the path is also wanted by code that must not touch the disk.
inline QString defaultProjectsFolder()
{
	return QDir(harpiaBaseFolder()).filePath(QStringLiteral("Projects"));
}

// The same, created if it is missing. Returns the path, or an empty string if
// it could not be made -- callers fall back to their old behaviour rather than
// handing a QFileDialog a directory that does not exist, which on Windows opens
// somewhere unrelated with no explanation.
inline QString ensureProjectsFolder()
{
	const QString dir = defaultProjectsFolder();
	if (QDir().mkpath(dir))
		return dir;
	return QString();
}

// What to call a project that has never been saved.
//
// Named after the media it was started from, because that is what the user
// recognises; "Untitled" when there is none, which is the case for a project
// begun from a blank editor.
inline QString suggestedProjectStem(const QString &sourcePath)
{
	const QString stem = QFileInfo(sourcePath).completeBaseName();
	return stem.isEmpty() ? QStringLiteral("Untitled") : stem;
}

// A path in `dir` for `stem` that does not exist yet: "Holiday.harpiaproj",
// then "Holiday 2.harpiaproj", and so on.
//
// `exists` is injected so the rule can be tested without a filesystem, and so a
// caller can ask about something other than the disk. The counter is bounded:
// a predicate that always says yes (a full disk, a permission problem, a stub
// in a test) must not spin forever -- past the limit it hands back the plain
// name and lets the save fail with a real error instead of hanging.
inline QString uniqueProjectPath(const QString &dir, const QString &stem,
				 const std::function<bool(const QString &)> &exists)
{
	const QString ext = QStringLiteral(".harpiaproj");
	const QString first = QDir(dir).filePath(stem + ext);
	if (!exists || !exists(first))
		return first;
	for (int n = 2; n <= 999; ++n) {
		const QString cand = QDir(dir).filePath(QStringLiteral("%1 %2%3").arg(stem).arg(n).arg(ext));
		if (!exists(cand))
			return cand;
	}
	return first;
}

// The same, against the real filesystem.
inline QString uniqueProjectPath(const QString &dir, const QString &stem)
{
	return uniqueProjectPath(dir, stem,
				 [](const QString &p) { return QFileInfo::exists(p); });
}

} // namespace harpia
