#include "core/RecordingParts.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

using namespace harpia::recording_parts;
static int failures = 0;
static void check(bool value, const char *message)
{
	std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
	if (!value)
		++failures;
}

static bool createFile(const QString &path)
{
	QFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write("keep this recording") == 19;
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	QTemporaryDir temporary;
	check(temporary.isValid(), "test recordings live in a private temporary folder");
	const QDir output(temporary.path());
	const QString base = output.filePath(QStringLiteral("Tutorial.final.take.mp4"));
	check(numberedPath(base, 1) == output.filePath(QStringLiteral("Tutorial.final.take_part001.mp4")),
	      "part numbers preserve a filename with multiple dots and its final extension");
	check(numberedPath(base, 12) == output.filePath(QStringLiteral("Tutorial.final.take_part012.mp4")),
	      "part numbers below 100 are zero padded for alphabetical ordering");
	check(numberedPath(base, 999) == output.filePath(QStringLiteral("Tutorial.final.take_part999.mp4")),
	      "part 999 keeps all three digits");
	check(numberedPath(base, 1001) == output.filePath(QStringLiteral("Tutorial.final.take_part1001.mp4")),
	      "recordings with more than 999 parts retain every digit");
	const QString unicode = output.filePath(QStringLiteral("Férias – São Paulo 🎬.MOV"));
	check(numberedPath(unicode, 2) == output.filePath(QStringLiteral("Férias – São Paulo 🎬_part002.MOV")),
	      "part filenames preserve spaces, unicode and the container extension's case");
	check(QFileInfo(numberedPath(base, 1)).absolutePath() == output.absolutePath(),
	      "part filenames remain in the chosen output folder");

	const QString requested = numberedPath(base, 1);
	check(availablePath(requested) == requested, "an unused target keeps the requested name");
	check(!QFileInfo::exists(requested), "finding an available name does not create or touch a recording");
	check(createFile(requested), "existing recording fixture is written");
	const QString second = output.filePath(QStringLiteral("Tutorial.final.take_part001 (2).mp4"));
	const QString third = output.filePath(QStringLiteral("Tutorial.final.take_part001 (3).mp4"));
	const QString fourth = output.filePath(QStringLiteral("Tutorial.final.take_part001 (4).mp4"));
	check(availablePath(requested) == second, "an existing recording is preserved by choosing a numbered alternative");
	check(createFile(second), "a second existing recording fixture is written");
	check(availablePath(requested) == third, "several existing recordings are skipped without overwriting");
	check(availablePath(requested, QSet<QString>{third}) == fourth,
	      "a target reserved by an in-flight remux is skipped together with existing files");
	const QString pendingOnly = numberedPath(base, 2);
	check(availablePath(pendingOnly, QSet<QString>{pendingOnly}) ==
		      output.filePath(QStringLiteral("Tutorial.final.take_part002 (2).mp4")),
	      "pending targets are protected even before the background writer creates them");
	check(availablePath(pendingOnly, QSet<QString>{output.filePath(QStringLiteral("unrelated.mp4"))}) == pendingOnly,
	      "an unrelated reservation does not rename a free target");
	QFile preserved(requested);
	check(preserved.open(QIODevice::ReadOnly) && preserved.readAll() == QByteArray("keep this recording"),
	      "collision resolution preserves the contents of the original recording");
	preserved.close();
	const QString occupiedDirectory = output.filePath(QStringLiteral("folder.mp4"));
	check(QDir().mkpath(occupiedDirectory) &&
		      availablePath(occupiedDirectory) == output.filePath(QStringLiteral("folder (2).mp4")),
	      "a directory at the destination is also treated as occupied");

	const QString flatFolder = output.filePath(QStringLiteral(".harpia_tmp"));
	const QString nestedFolder = output.filePath(QStringLiteral(".harpia_tmp/take-uuid"));
	const QString deeplyNestedFolder = output.filePath(QStringLiteral(".harpia_tmp/another-take/segments"));
	check(QDir().mkpath(nestedFolder) && QDir().mkpath(deeplyNestedFolder),
	      "flat and nested recovery fixtures exist");
	check(recoveryFolder(QDir(flatFolder).filePath(QStringLiteral("old-recording.mkv"))) == output.absolutePath(),
	      "older flat temporary recordings recover to the output folder");
	check(recoveryFolder(QDir(nestedFolder).filePath(QStringLiteral("part001.mkv"))) == output.absolutePath(),
	      "split recordings recover above their per-take directory into the output folder");
	check(recoveryFolder(QDir(deeplyNestedFolder).filePath(QStringLiteral("part002.mkv"))) == output.absolutePath(),
	      "recovery finds the output folder even below additional nested directories");
	const QString regularFolder = output.filePath(QStringLiteral("normal folder"));
	check(QDir().mkpath(regularFolder) &&
		      recoveryFolder(QDir(regularFolder).filePath(QStringLiteral("saved.mp4"))) == QDir(regularFolder).absolutePath(),
	      "recordings outside the temporary tree keep their own directory");
	const QString similarFolder = output.filePath(QStringLiteral(".harpia_tmp_backup"));
	check(QDir().mkpath(similarFolder) &&
		      recoveryFolder(QDir(similarFolder).filePath(QStringLiteral("saved.mp4"))) == QDir(similarFolder).absolutePath(),
	      "a similarly named folder is not mistaken for the temporary recording tree");

	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
