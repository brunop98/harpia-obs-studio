#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QStringList>

namespace harpia::project_assets {

// Check the lexical path as well as its canonical alias: a missing take still
// belongs to its session directory and must fail the save, not become a broken
// external-media reference. Canonical paths cover symlinked temp directories.
inline bool isWithinDirectory(const QString &path, const QString &directory)
{
	if (path.isEmpty() || directory.isEmpty())
		return false;
	const auto inside = [](QString file, QString folder) {
		if (file.isEmpty() || folder.isEmpty())
			return false;
		file = QDir::cleanPath(QDir::fromNativeSeparators(file));
		folder = QDir::cleanPath(QDir::fromNativeSeparators(folder));
#ifdef Q_OS_WIN
		file = file.toCaseFolded();
		folder = folder.toCaseFolded();
#endif
		if (file == folder)
			return true;
		if (!folder.endsWith(QLatin1Char('/')))
			folder += QLatin1Char('/');
		return file.startsWith(folder);
	};
	const QFileInfo file(path), folder(directory);
	return inside(file.absoluteFilePath(), folder.absoluteFilePath()) ||
	       inside(file.canonicalFilePath(), folder.canonicalFilePath());
}

// Publish immutable narration assets before committing the project JSON. If
// anything fails, discard only files created by this save; existing project
// assets may still be referenced by the loaded model, undo history or autosave.
class SaveTransaction {
public:
	explicit SaveTransaction(const QString &projectPath)
		: projectDir_(QFileInfo(projectPath).absolutePath()),
		  assetsRelative_(QFileInfo(projectPath).completeBaseName() + QStringLiteral("_assets")),
		  assetsDir_(QDir(projectDir_).filePath(assetsRelative_)),
		  assetsExisted_(QFileInfo::exists(assetsDir_))
	{
	}
	~SaveTransaction()
	{
		if (retained_)
			return;
		for (const QString &path : created_)
			QFile::remove(path);
		if (!assetsExisted_)
			QDir().rmdir(assetsDir_); // only removes an empty directory
	}
	SaveTransaction(const SaveTransaction &) = delete;
	SaveTransaction &operator=(const SaveTransaction &) = delete;

	// The returned relative path is portable with the project's assets folder.
	// Equal content is reused even when two clips refer to different files.
	QString voiceover(const QString &sourcePath, QString *relativePath)
	{
		const QFileInfo sourceInfo(sourcePath);
		QString sourceKey = sourceInfo.canonicalFilePath();
		if (sourceKey.isEmpty())
			sourceKey = sourceInfo.absoluteFilePath();
#ifdef Q_OS_WIN
		sourceKey = sourceKey.toCaseFolded();
#endif
		if (const auto found = stored_.constFind(sourceKey); found != stored_.cend()) {
			*relativePath = found.value();
			return {};
		}
		const auto failure = [&sourceInfo](const QString &reason) {
			return QStringLiteral("Could not save narration \"%1\": %2")
				.arg(sourceInfo.fileName(), reason);
		};
		QFile source(sourcePath);
		if (!sourceInfo.isFile() || !source.open(QIODevice::ReadOnly))
			return failure(QStringLiteral("the source file could not be read."));
		QCryptographicHash hash(QCryptographicHash::Sha256);
		if (!hash.addData(&source))
			return failure(QStringLiteral("the source file could not be read completely."));
		const QByteArray digest = hash.result();
		const QString relative = assetsRelative_ + QStringLiteral("/vo_") +
			QString::fromLatin1(digest.toHex()) + QStringLiteral(".wav");
		const QString destination = QDir(projectDir_).filePath(relative);
		if (QFileInfo::exists(destination)) {
			// This also handles a loaded project saving an asset onto itself.
			// Never delete/replace it: later clips may still read this path.
			QFile existing(destination);
			QCryptographicHash verify(QCryptographicHash::Sha256);
			if (!existing.open(QIODevice::ReadOnly) || !verify.addData(&existing) || verify.result() != digest)
				return failure(QStringLiteral("an existing project audio asset could not be verified."));
		} else {
			if (!QDir().mkpath(assetsDir_))
				return failure(QStringLiteral("the project's assets folder could not be created."));
			QSaveFile output(destination);
			if (!source.seek(0) || !output.open(QIODevice::WriteOnly))
				return failure(QStringLiteral("the project's audio asset could not be written."));
			QCryptographicHash copied(QCryptographicHash::Sha256);
			while (!source.atEnd()) {
				const QByteArray bytes = source.read(1024 * 1024);
				if (source.error() != QFileDevice::NoError || output.write(bytes) != bytes.size()) {
					output.cancelWriting();
					return failure(QStringLiteral("the audio file could not be copied completely."));
				}
				copied.addData(bytes);
			}
			if (copied.result() != digest) {
				output.cancelWriting();
				return failure(QStringLiteral("the source audio changed while it was being saved. Try saving again."));
			}
			if (!output.commit())
				return failure(QStringLiteral("the project's audio asset could not be finalized."));
			created_.append(destination);
		}
		stored_.insert(sourceKey, relative);
		*relativePath = relative;
		return {};
	}

	// Called only after the project JSON has been committed successfully.
	void retain() { retained_ = true; }

private:
	QString projectDir_, assetsRelative_, assetsDir_;
	QHash<QString, QString> stored_;
	QStringList created_;
	bool assetsExisted_ = false;
	bool retained_ = false;
};

} // namespace harpia::project_assets
