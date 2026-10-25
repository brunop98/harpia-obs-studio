#pragma once

// Helper discovery is separate from process execution so a Finder-style
// environment (without Homebrew on PATH) can be tested on any platform.
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace harpia::yt_helpers {

inline QStringList macInstallDirectories(const QString &cpuArchitecture)
{
	const QString arm = QStringLiteral("/opt/homebrew/bin");
	const QString intel = QStringLiteral("/usr/local/bin");
	return cpuArchitecture == QLatin1String("arm64") || cpuArchitecture == QLatin1String("aarch64")
		? QStringList{arm, intel} : QStringList{intel, arm};
}

inline QStringList macSearchDirectories(const QString &applicationDir, const QString &downloaderDir,
				       const QStringList &pathDirectories,
				       const QStringList &installDirectories)
{
	QStringList directories;
	const auto add = [&directories](const QString &directory) {
		if (directory.isEmpty())
			return;
		const QString clean = QDir::cleanPath(directory);
		if (!directories.contains(clean))
			directories << clean;
	};
	// A user-selected downloader may carry matching ffmpeg and JS helpers.
	add(downloaderDir);
	if (!applicationDir.isEmpty()) {
		add(applicationDir + QStringLiteral("/../Helpers")); // Contents/Helpers
		add(applicationDir + QStringLiteral("/helpers"));
		add(applicationDir);
	}
	for (const QString &directory : pathDirectories)
		add(directory);
	for (const QString &directory : installDirectories)
		add(directory);
	return directories;
}

inline QString findExecutable(const QString &name, const QStringList &directories)
{
	// macOS helpers have no .exe suffix. Checking execute permission also keeps
	// a downloaded archive or a non-executable script from hiding a usable copy.
	for (const QString &directory : directories) {
		const QFileInfo file(QDir(directory).filePath(name));
		if (file.isFile() && file.isExecutable())
			return file.absoluteFilePath();
	}
	return {};
}

inline bool hasOption(const QStringList &arguments, const QString &option)
{
	for (const QString &argument : arguments)
		if (argument == option || argument.startsWith(option + QLatin1Char('=')))
			return true;
	return false;
}

inline QStringList argumentOverrides(const QString &ffmpeg, const QString &deno,
				     const QStringList &userArguments)
{
	QStringList arguments;
	if (!ffmpeg.isEmpty() && !hasOption(userArguments, QStringLiteral("--ffmpeg-location")))
		arguments << QStringLiteral("--ffmpeg-location") << ffmpeg;
	if (!deno.isEmpty() && !hasOption(userArguments, QStringLiteral("--js-runtimes")) &&
	    !hasOption(userArguments, QStringLiteral("--no-js-runtimes")))
		arguments << QStringLiteral("--js-runtimes") << QStringLiteral("deno:") + deno;
	return arguments;
}

} // namespace harpia::yt_helpers
