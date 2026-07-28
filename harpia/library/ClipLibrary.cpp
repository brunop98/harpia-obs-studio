#include "ClipLibrary.hpp"

#include "../editor/TimeText.hpp"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QSet>

#include <algorithm>

namespace harpia {

QString ClipInfo::relativeAge() const
{
	return harpia::relativeAge(modified);
}

QString relativeAge(const QDateTime &modified)
{
	if (!modified.isValid())
		return QStringLiteral("unknown");

	const qint64 secs = modified.secsTo(QDateTime::currentDateTime());
	if (secs < 0)
		return QStringLiteral("just now");
	if (secs < 60)
		return QStringLiteral("just now");

	const qint64 mins = secs / 60;
	if (mins < 60)
		return QStringLiteral("%1 minute%2 ago").arg(mins).arg(mins == 1 ? "" : "s");

	const qint64 hours = mins / 60;
	if (hours < 24)
		return QStringLiteral("%1 hour%2 ago").arg(hours).arg(hours == 1 ? "" : "s");

	const qint64 days = hours / 24;
	if (days < 7)
		return QStringLiteral("%1 day%2 ago").arg(days).arg(days == 1 ? "" : "s");

	const qint64 weeks = days / 7;
	if (weeks < 5)
		return QStringLiteral("%1 week%2 ago").arg(weeks).arg(weeks == 1 ? "" : "s");

	const qint64 months = days / 30;
	if (months < 12)
		return QStringLiteral("%1 month%2 ago").arg(months).arg(months == 1 ? "" : "s");

	const qint64 years = days / 365;
	return QStringLiteral("%1 year%2 ago").arg(years).arg(years == 1 ? "" : "s");
}

QString ClipInfo::exactDate() const
{
	if (!modified.isValid())
		return QStringLiteral("unknown");
	return modified.toString(QStringLiteral("d MMM yyyy HH:mm"));
}

QString ClipInfo::humanSize() const
{
	return QLocale().formattedDataSize(sizeBytes, 1, QLocale::DataSizeTraditionalFormat);
}

QString ClipInfo::durationString(qint64 ms)
{
	if (ms <= 0)
		return {};
	return timeTextDuration(ms);
}

QStringList ClipLibrary::videoExtensions()
{
	return {QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov"), QStringLiteral("gif")};
}

QVector<ClipInfo> ClipLibrary::scan(const QStringList &folders, const PresetByFolder &presetByFolder)
{
	QStringList nameFilters;
	for (const QString &ext : videoExtensions())
		nameFilters << QStringLiteral("*.%1").arg(ext);

	// Normalize the preset-folder keys to absolute paths for reliable matching.
	PresetByFolder presetByAbsFolder;
	for (auto it = presetByFolder.constBegin(); it != presetByFolder.constEnd(); ++it)
		presetByAbsFolder.insert(QDir(it.key()).absolutePath(), it.value());

	// De-duplicate by absolute path in case folders overlap.
	QVector<ClipInfo> clips;
	QSet<QString> seen;

	for (const QString &folder : folders) {
		if (folder.isEmpty())
			continue;
		QDir dir(folder);
		if (!dir.exists())
			continue;

		const QString folderKey = dir.absolutePath();
		const QString presetName = presetByAbsFolder.value(folderKey);

		const QFileInfoList entries =
			dir.entryInfoList(nameFilters, QDir::Files | QDir::NoSymLinks, QDir::Time);
		for (const QFileInfo &fi : entries) {
			const QString abs = fi.absoluteFilePath();
			if (seen.contains(abs))
				continue;
			// Webcam companion files (<base>_webcam.mp4) belong to a listed
			// clip — showing them as clips of their own duplicates every
			// webcam recording in the strip and library.
			if (fi.completeBaseName().endsWith(QStringLiteral("_webcam"), Qt::CaseInsensitive))
				continue;
			seen.insert(abs);

			ClipInfo info;
			info.filePath = abs;
			info.fileName = fi.fileName();
			info.sizeBytes = fi.size();
			info.modified = fi.lastModified();
			info.presetName = presetName;
			info.isGif = fi.suffix().compare(QStringLiteral("gif"), Qt::CaseInsensitive) == 0;
			clips.push_back(info);
		}
	}

	std::sort(clips.begin(), clips.end(),
		  [](const ClipInfo &a, const ClipInfo &b) { return a.modified > b.modified; });
	return clips;
}

QVector<ClipInfo> ClipLibrary::recent(const QStringList &folders, int count, const PresetByFolder &presetByFolder)
{
	QVector<ClipInfo> all = scan(folders, presetByFolder);
	if (all.size() > count)
		all.resize(count);
	return all;
}

} // namespace harpia
