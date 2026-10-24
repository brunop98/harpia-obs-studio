// The opening screen must find existing projects, then keep the user's three
// most recently saved/opened projects across sessions without listing autosaves.
#include "editor/RecentProjects.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool condition, const char *what)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		++failures;
}

static QString project(const QString &folder, const QString &name, qint64 timestamp)
{
	const QString path = QDir(folder).filePath(name);
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly) || file.write("{}") != 2 || !file.flush() ||
	    !file.setFileTime(QDateTime::fromSecsSinceEpoch(timestamp), QFileDevice::FileModificationTime))
		return {};
	file.close();
	return QFileInfo(path).canonicalFilePath();
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	QTemporaryDir temp;
	if (!temp.isValid())
		return 2;
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.filePath("settings"));
	QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, temp.filePath("system-settings"));
	const QString folder = temp.filePath("projects");
	const QString elsewhere = temp.filePath("elsewhere");
	QDir().mkpath(folder);
	QDir().mkpath(elsewhere);
	const QString key = QStringLiteral("editor/recentProjects");
	const QString a = project(folder, "A.harpiaproj", 1700000001);
	const QString b = project(folder, "B.harpiaproj", 1700000002);
	const QString c = project(folder, "C.harpiaproj", 1700000003);
	const QString d = project(folder, "D.harpiaproj", 1700000004);
	const QString autosave = project(folder, "A_autosave.harpiaproj", 1700000099);
	const QString other = project(folder, "unrelated.txt", 1700000100);
	QDir().mkpath(QDir(folder).filePath("directory.harpiaproj"));
	ok(!a.isEmpty() && !b.isEmpty() && !c.isEmpty() && !d.isEmpty(), "test project files exist");
	ok(recentProjects(folder) == QStringList{d, c, b}, "existing projects seed newest first, capped at three");
	{
		QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Harpia", "Recorder");
		ok(!settings.contains(key), "reading the opening screen does not write history");
		ok(!QFileInfo::exists(settings.fileName()), "reading creates no settings file");
	}
	ok(recentProjects(temp.filePath("absent")).isEmpty(), "missing projects folder is harmless");
	ok(!QDir(temp.filePath("absent")).exists(), "reading does not create the projects folder");

	rememberRecentProject(a);
	ok(recentProjects(folder) == QStringList{a, d, c}, "remembered projects precede folder suggestions");
	{
		QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Harpia", "Recorder");
		settings.sync();
		ok(settings.value(key).toStringList() == QStringList{a}, "only explicit activity is persisted");
	}
	rememberRecentProject(b);
	rememberRecentProject(c);
	rememberRecentProject(d);
	ok(recentProjects(QString()) == QStringList{d, c, b}, "a fourth project evicts the oldest remembered entry");
	rememberRecentProject(b);
	ok(recentProjects(QString()) == QStringList{b, d, c}, "continuing an older project moves it to the front");
	rememberRecentProject(QDir(folder).filePath("../projects/B.harpiaproj"));
	ok(recentProjects(QString()) == QStringList{b, d, c}, "different spellings of one path do not duplicate it");
#ifdef Q_OS_WIN
	rememberRecentProject(b.toUpper());
	ok(recentProjects(QString()) == QStringList{b, d, c}, "Windows paths deduplicate without regard to case");
#endif
	rememberRecentProject(autosave);
	rememberRecentProject(other);
	rememberRecentProject(QDir(folder).filePath("directory.harpiaproj"));
	rememberRecentProject(temp.filePath("missing.harpiaproj"));
	ok(recentProjects(QString()) == QStringList{b, d, c}, "autosaves, nonprojects, directories and missing files are ignored");

	const QString outside = project(elsewhere, "Outside.HARPIAPROJ", 1700000000);
	rememberRecentProject(outside);
	ok(recentProjects(folder) == QStringList{outside, b, d}, "custom folders and uppercase project extensions are supported");
	{
		QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Harpia", "Recorder");
		settings.sync();
	}
	{
		QSettings fresh(QSettings::IniFormat, QSettings::UserScope, "Harpia", "Recorder");
		ok(fresh.value(key).toStringList() == QStringList{outside, b, d}, "history persists in a fresh settings instance");
	}
	QFile::remove(b);
	ok(recentProjects(folder) == QStringList{outside, d, c}, "deleted history entries are skipped and replaced by a folder suggestion");
	{
		QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Harpia", "Recorder");
		settings.setValue(key, QStringList{autosave, outside, outside, d, c});
	}
	ok(recentProjects(folder) == QStringList{outside, d, c}, "old settings are filtered and deduplicated before the three-project limit");
	rememberRecentProject(a);
	{
		QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Harpia", "Recorder");
		ok(settings.value(key).toStringList() == QStringList{a, outside, d}, "remembering also cleans stale duplicate settings entries");
	}

	std::printf("\n%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
