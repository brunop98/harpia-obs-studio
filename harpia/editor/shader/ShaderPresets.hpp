#pragma once

// Seed stock shaders without replacing user-authored files. Existing installs
// predate stock-version tracking, so the one known legacy CRT is recognized by
// its content rather than by its filename alone.

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSaveFile>
#include <QString>
#include <QStringList>

namespace harpia {

struct ShaderPresetSeedResult {
	int created = 0;
	int updated = 0;
	QStringList errors;
};

inline ShaderPresetSeedResult ensureDefaultShaderPresets(const QString &folder,
						       const QMap<QString, QByteArray> &bundled)
{
	ShaderPresetSeedResult result;
	// No resources, no writes: development/test builds may have no resource
	// collection while a user's real shaders still live in the chosen folder.
	if (bundled.isEmpty())
		return result;
	if (!QDir().mkpath(folder)) {
		result.errors << QStringLiteral("Could not create the shaders folder: %1").arg(folder);
		return result;
	}
	for (auto it = bundled.constBegin(); it != bundled.constEnd(); ++it) {
		const QString &name = it.key();
		if (QFileInfo(name).fileName() != name || name.contains(QLatin1Char('\\')) ||
		    !name.endsWith(QLatin1String(".frag")) || it.value().isEmpty()) {
			result.errors << QStringLiteral("Invalid or empty bundled shader: %1").arg(name);
			continue;
		}
		const QString path = QDir(folder).filePath(name);
		const QFileInfo existing(path);
		const bool present = existing.exists() || existing.isSymLink();
		if (present) {
			// A symlink is an explicit user-managed location. Leave the link
			// and its target untouched even when its contents happen to be stock.
			if (existing.isSymLink())
				continue;
			QFile file(path);
			if (!existing.isFile() || !file.open(QIODevice::ReadOnly)) {
				result.errors << QStringLiteral("Could not read the existing shader: %1").arg(path);
				continue;
			}
			QByteArray contents = file.readAll();
			if (file.error() != QFileDevice::NoError) {
				result.errors << QStringLiteral("Could not read the existing shader: %1").arg(path);
				continue;
			}
			file.close();
			contents.replace("\r\n", "\n");
			QByteArray shipped = it.value();
			shipped.replace("\r\n", "\n");
			if (contents == shipped)
				continue;
			const QByteArray hash = QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex();
			if (name != QLatin1String("crt.frag") ||
			    hash != QByteArrayLiteral("aa99007a0b3fb941144300504ee0ce2682de37a20efc96d2dd226f2c40400e52"))
				continue; // existing custom shaders belong to their author
		}
		QSaveFile file(path);
		file.setDirectWriteFallback(false);
		if (!file.open(QIODevice::WriteOnly) || file.write(it.value()) != it.value().size() || !file.commit()) {
			result.errors << QStringLiteral("Could not safely write the bundled shader: %1 (%2)")
				.arg(path, file.errorString());
			continue;
		}
		if (present)
			++result.updated;
		else
			++result.created;
	}
	return result;
}

} // namespace harpia
