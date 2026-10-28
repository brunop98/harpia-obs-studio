#pragma once

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QString>

namespace harpia::recording_parts {

inline QString numberedPath(const QString &basePath, int part)
{
	const QFileInfo file(basePath);
	return file.dir().filePath(file.completeBaseName() +
		QStringLiteral("_part%1.").arg(part, 3, 10, QLatin1Char('0')) + file.suffix());
}

// Reserve names on the GUI thread before handing any work to the remux pool.
// QFile::rename also refuses an occupied destination if another process creates
// it after this check; the captured source must survive that failure.
inline QString availablePath(const QString &requested, const QSet<QString> &pending = {})
{
	const QFileInfo file(requested);
	QString candidate = file.absoluteFilePath();
	int suffix = 2;
	while (QFileInfo::exists(candidate) || pending.contains(candidate)) {
		candidate = file.dir().absoluteFilePath(file.completeBaseName() +
			QStringLiteral(" (%1).").arg(suffix++) + file.suffix());
	}
	return candidate;
}

// Split takes have their own directory below .harpia_tmp. Recover them into
// the actual output folder, just like the older flat temporary recordings.
inline QString recoveryFolder(const QString &recordedPath)
{
	QDir dir = QFileInfo(recordedPath).dir();
	while (true) {
		if (dir.dirName() == QStringLiteral(".harpia_tmp")) {
			dir.cdUp();
			return dir.absolutePath();
		}
		if (!dir.cdUp())
			return QFileInfo(recordedPath).absolutePath();
	}
}

} // namespace harpia::recording_parts
