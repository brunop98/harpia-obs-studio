#pragma once

#include "../core/RecorderSettings.hpp"

#include "ProjectPaths.hpp"

#include <QSettings>
#include <QStringList>

namespace harpia {
namespace recent_projects_detail {

inline QString projectPath(const QString &path)
{
	if (path.isEmpty())
		return {};
	const QFileInfo file(path);
	if (!file.isFile() || !file.isReadable() ||
	    file.suffix().compare(QStringLiteral("harpiaproj"), Qt::CaseInsensitive) != 0 ||
	    file.fileName().endsWith(QStringLiteral("_autosave.harpiaproj"), Qt::CaseInsensitive))
		return {};
	return file.canonicalFilePath();
}

inline void appendProject(QStringList &paths, const QString &path)
{
	if (paths.size() >= 3)
		return;
	const QString canonical = projectPath(path);
	if (canonical.isEmpty())
		return;
#ifdef Q_OS_WIN
	constexpr auto pathCase = Qt::CaseInsensitive;
#else
	constexpr auto pathCase = Qt::CaseSensitive;
#endif
	if (!paths.contains(canonical, pathCase))
		paths.append(canonical);
}

} // namespace recent_projects_detail

// Remember explicit saves and opens, including projects outside the default
// folder. Reading also finds existing projects from before this history existed;
// it never changes settings or makes a folder merely to show the opening screen.
inline QStringList recentProjects(const QString &folder = defaultProjectsFolder())
{
	QSettings settings = harpia::recorderSettings(QSettings::defaultFormat());
	QStringList paths;
	for (const QString &path : settings.value(QStringLiteral("editor/recentProjects")).toStringList())
		recent_projects_detail::appendProject(paths, path);
	if (paths.size() < 3 && !folder.isEmpty()) {
		const auto files = QDir(folder).entryInfoList({QStringLiteral("*.harpiaproj")},
						     QDir::Files | QDir::Readable, QDir::Time);
		for (const QFileInfo &file : files) {
			recent_projects_detail::appendProject(paths, file.absoluteFilePath());
			if (paths.size() == 3)
				break;
		}
	}
	return paths;
}

inline void rememberRecentProject(const QString &path)
{
	const QString canonical = recent_projects_detail::projectPath(path);
	if (canonical.isEmpty())
		return;
	QSettings settings = harpia::recorderSettings(QSettings::defaultFormat());
	QStringList paths{canonical};
	for (const QString &previous : settings.value(QStringLiteral("editor/recentProjects")).toStringList())
		recent_projects_detail::appendProject(paths, previous);
	settings.setValue(QStringLiteral("editor/recentProjects"), paths);
}

} // namespace harpia
