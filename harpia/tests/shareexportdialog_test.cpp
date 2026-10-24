// The sharing dialog must expose the complete Discord result, not a path to
// a temporary transcode or just the first numbered part.
#include "ui/ShareExportDialog.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}

static QPushButton *button(QWidget &widget, const QString &text)
{
	for (QPushButton *button : widget.findChildren<QPushButton *>())
		if (button->text() == text)
			return button;
	return nullptr;
}

static bool waitReady(QWidget &widget, const QString &title)
{
	QElapsedTimer timer;
	timer.start();
	while (timer.elapsed() < 15000) {
		QApplication::processEvents();
		for (QLabel *label : widget.findChildren<QLabel *>())
			if (label->text() == title)
				return true;
		QThread::msleep(5);
	}
	return false;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QTemporaryDir work;
	ok(work.isValid(), "the test has an isolated output folder");
	if (!work.isValid())
		return 1;
	const QString source = work.filePath(QStringLiteral("recording.mp4"));
	QProcess ffmpeg;
	ffmpeg.start(QStringLiteral("ffmpeg"), {"-y", "-v", "error", "-f", "lavfi", "-i",
		"testsrc2=size=160x90:rate=15:duration=1", "-c:v", "libx264", "-pix_fmt", "yuv420p", source});
	ok(ffmpeg.waitForFinished(30000) && ffmpeg.exitCode() == 0, "a real silent video was generated");
	if (failures)
		return 1;

	std::printf("\n-- Discord result folder and clipboard --\n");
	const QString directory = work.filePath(QStringLiteral("recording_discord"));
	ShareExportDialog discord(source, directory, ShareExporter::Level::Discord);
	discord.show();
	ok(waitReady(discord, QStringLiteral("Discord parts ready")), "the dialog reaches its Discord success page");
	const QStringList names = QDir(directory).entryList({"*.mp4"}, QDir::Files, QDir::Name);
	ok(names == QStringList{QStringLiteral("part001.mp4")}, "a small video produces one complete part");
	QPushButton *copy = button(discord, QStringLiteral("Copy All Parts"));
	ok(copy != nullptr, "the summary offers Copy All Parts");
	if (copy) {
		copy->click();
		const QList<QUrl> urls = QApplication::clipboard()->mimeData()->urls();
		ok(urls.size() == names.size() && !urls.isEmpty() &&
			   urls[0].toLocalFile() == QDir(directory).filePath(names[0]),
		   "the clipboard contains the published numbered MP4 files");
	}
	QPushButton *path = button(discord, QStringLiteral("Copy Folder Path"));
	ok(path != nullptr, "the folder path can be copied");
	if (path) {
		path->click();
		ok(QApplication::clipboard()->text() == QDir::toNativeSeparators(directory),
		   "it points to the final Discord folder, not temporary data");
	}
	ok(button(discord, QStringLiteral("Open Folder")) != nullptr, "Open Folder is available");
	bool remainder = false;
	for (QLabel *label : discord.findChildren<QLabel *>())
		remainder = remainder || label->text().contains(QStringLiteral("each at most 49 MB"));
	ok(remainder, "the summary explains the per-part limit and smaller last part");
	discord.accept();

	std::printf("\n-- ordinary sharing remains a single-file workflow --\n");
	const QString shared = work.filePath(QStringLiteral("recording_shared.mp4"));
	ShareExportDialog standard(source, shared, ShareExporter::Level::Balanced);
	standard.show();
	ok(waitReady(standard, QStringLiteral("Sharing copy ready")), "the normal sharing summary still succeeds");
	copy = button(standard, QStringLiteral("Copy File"));
	ok(copy != nullptr && QFileInfo::exists(shared), "ordinary sharing still creates one MP4");
	if (copy) {
		copy->click();
		const QList<QUrl> urls = QApplication::clipboard()->mimeData()->urls();
		ok(urls.size() == 1 && urls[0].toLocalFile() == shared, "Copy File still copies that MP4");
	}
	standard.accept();

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
