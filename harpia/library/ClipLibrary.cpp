#include "ClipLibrary.hpp"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QSet>

#include <algorithm>

namespace harpia {

QString ClipInfo::relativeAge() const
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

QString ClipInfo::humanSize() const
{
	return QLocale().formattedDataSize(sizeBytes, 1, QLocale::DataSizeTraditionalFormat);
}

QStringList ClipLibrary::videoExtensions()
{
	return {QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov"), QStringLiteral("gif")};
}

QVector<ClipInfo> ClipLibrary::scan(const QStringList &folders)
{
	QStringList nameFilters;
	for (const QString &ext : videoExtensions())
		nameFilters << QStringLiteral("*.%1").arg(ext);

	// De-duplicate by absolute path in case folders overlap.
	QVector<ClipInfo> clips;
	QSet<QString> seen;

	for (const QString &folder : folders) {
		if (folder.isEmpty())
			continue;
		QDir dir(folder);
		if (!dir.exists())
			continue;

		const QFileInfoList entries =
			dir.entryInfoList(nameFilters, QDir::Files | QDir::NoSymLinks, QDir::Time);
		for (const QFileInfo &fi : entries) {
			const QString abs = fi.absoluteFilePath();
			if (seen.contains(abs))
				continue;
			seen.insert(abs);

			ClipInfo info;
			info.filePath = abs;
			info.fileName = fi.fileName();
			info.sizeBytes = fi.size();
			info.modified = fi.lastModified();
			clips.push_back(info);
		}
	}

	std::sort(clips.begin(), clips.end(),
		  [](const ClipInfo &a, const ClipInfo &b) { return a.modified > b.modified; });
	return clips;
}

QVector<ClipInfo> ClipLibrary::recent(const QStringList &folders, int count)
{
	QVector<ClipInfo> all = scan(folders);
	if (all.size() > count)
		all.resize(count);
	return all;
}

} // namespace harpia
