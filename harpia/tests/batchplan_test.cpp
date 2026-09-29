// Batch export naming and folders: what a folder of twelve clips is called,
// what a prefix typed with a slash in it becomes, and that a second batch
// never lands on top of the first.
#include "editor/BatchExport.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <cstdio>

using namespace harpia;
using namespace harpia::batch_export;

static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	std::printf("\n-- names --\n");
	ok(clipName("Tutorial", 1, 9) == "Tutorial_01", "two digits at least (sorts 01..09)");
	ok(clipName("Tutorial", 12, 12) == "Tutorial_12", "twelve of twelve");
	ok(clipName("Tutorial", 7, 120) == "Tutorial_007", "three digits for a batch over 99");
	ok(clipName("", 3, 5) == "clip_03", "no prefix: 'clip'");

	std::printf("\n-- a prefix that can be a file name --\n");
	ok(safePrefix("a/b\\c:d*e?f\"g<h>i|j") == "abcdefghij", "Windows' forbidden characters dropped");
	ok(safePrefix("  name. ") == "name", "trailing dots and spaces dropped");
	ok(safePrefix("...") == "clip", "nothing left: 'clip'");
	ok(safePrefix("Meu vídeo – 2") == "Meu vídeo – 2", "accents and dashes kept");

	std::printf("\n-- a folder per batch --\n");
	{
		QTemporaryDir tmp;
		const QString first = uniqueFolder(tmp.path(), "Tutorial");
		ok(first == QDir(tmp.path()).filePath("Tutorial"), "the prefix names the folder");
		QDir().mkpath(first);
		const QString second = uniqueFolder(tmp.path(), "Tutorial");
		ok(second == QDir(tmp.path()).filePath("Tutorial (2)"), "a second batch gets (2)");
		QDir().mkpath(second);
		ok(uniqueFolder(tmp.path(), "Tutorial").endsWith("Tutorial (3)"), "then (3)");
		ok(uniqueFolder(tmp.path(), "a/b").endsWith("/ab"), "the folder name is made safe too");
	}

	std::printf("\n-- what the end says --\n");
	ok(summary(12, 0, 12, false) == "12 clips exported", "all good");
	ok(summary(1, 0, 1, false) == "1 clip exported", "one");
	ok(summary(11, 1, 12, false) == "11 of 12 clips exported (1 failed)", "with a failure");
	ok(summary(4, 0, 12, true) == "Stopped after 4 of 12 clips", "cancelled");

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
