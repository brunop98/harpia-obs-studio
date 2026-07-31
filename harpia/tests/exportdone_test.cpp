// The window that appears when an export finishes.
//
// It used to be a message box whose every button dismissed it, so "export, then
// go and use the file" meant exporting, dismissing, and hunting the file down.
// Now the thumbnail is a drag source and there are five actions that do not
// close anything.
//
// Two things are worth checking, and they are different in kind:
//
//   1. WHAT THE FILE HANDOFF CARRIES. A drag that carries a picture of the
//      video instead of the video is useless -- it would drop a bitmap into
//      Discord. The mime data has to carry the file's URL, which is what a file
//      manager, a chat client or a mail composer reads as "here is a file", and
//      what Qt turns into CF_HDROP on Windows so Explorer pastes the file.
//
//   2. THAT NOTHING DISMISSES THE WINDOW. That is the entire point of the
//      change, and it is the kind of thing a later edit breaks by accident, so
//      every action is clicked and the dialog is checked afterwards.
#include "ui/ExportDoneDialog.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QMimeData>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <cstdio>
#include <memory>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static QPushButton *buttonNamed(QWidget *w, const QString &text)
{
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (b->text() == text)
			return b;
	return nullptr;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	// A real file on disk, because half of this is about paths.
	QTemporaryDir tmp;
	const QString path = tmp.path() + QStringLiteral("/my clip_clip.mp4");
	{
		QFile f(path);
		f.open(QIODevice::WriteOnly);
		f.write(QByteArray(4096, 'x'));
	}

	std::printf("\n-- what a drag hands over --\n");
	{
		std::unique_ptr<QMimeData> m(ExportDoneDialog::fileMimeData(path));
		ok(m != nullptr, "there is something to drag");
		ok(m && m->hasUrls(), "and it carries URLs, which is what makes it a FILE drop");
		if (m && m->hasUrls()) {
			const QList<QUrl> urls = m->urls();
			ok(urls.size() == 1, "exactly one of them");
			ok(urls.first().isLocalFile(), "a local file");
			ok(urls.first().toLocalFile() == QFileInfo(path).absoluteFilePath(),
			   "and it is the file that was exported");
		}
		// Text too, so a terminal or a text field gets the path rather than
		// nothing. NOT instead of the URLs: text alone would paste a string into
		// Explorer, not the file.
		ok(m && m->hasText(), "it also carries the path as text");
		ok(m && !m->hasImage(),
		   "and NOT an image -- dropping a picture of the video would be the wrong file");

		ok(ExportDoneDialog::fileMimeData(QString()) == nullptr, "an empty path drags nothing");
	}

	std::printf("\n-- the two paths --\n");
	{
		ok(ExportDoneDialog::nativePathFor(path) ==
			   QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()),
		   "the file path is the file, in the platform's own spelling");
		ok(ExportDoneDialog::folderPathFor(path) ==
			   QDir::toNativeSeparators(QFileInfo(path).absolutePath()),
		   "the folder path is its folder");
		ok(ExportDoneDialog::folderPathFor(path) != ExportDoneDialog::nativePathFor(path),
		   "and the two are genuinely different answers");
		ok(ExportDoneDialog::folderPathFor(QString()).isEmpty(), "an empty path has no folder");
	}

	std::printf("\n-- the buttons are all there --\n");
	QImage thumb(64, 36, QImage::Format_RGBA8888);
	thumb.fill(Qt::darkCyan);
	ExportDoneDialog dlg(path, thumb);
	dlg.show();
	{
		for (const char *label : {"Copy File", "Copy File Path", "Copy Folder Path", "Open File",
					  "Open Folder", "Close"})
			ok(buttonNamed(&dlg, QString::fromLatin1(label)) != nullptr, label);
		ok(buttonNamed(&dlg, QStringLiteral("Done")) == nullptr, "and Done is gone");
	}

	std::printf("\n-- copying puts the right thing on the clipboard --\n");
	{
		QClipboard *cb = QApplication::clipboard();

		cb->clear();
		buttonNamed(&dlg, QStringLiteral("Copy File Path"))->click();
		std::printf("     after Copy File Path: \"%s\"\n", qUtf8Printable(cb->text()));
		ok(cb->text() == ExportDoneDialog::nativePathFor(path), "the file path is on it");
		const bool pathHadUrls = cb->mimeData() && cb->mimeData()->hasUrls();

		cb->clear();
		buttonNamed(&dlg, QStringLiteral("Copy Folder Path"))->click();
		std::printf("     after Copy Folder Path: \"%s\"\n", qUtf8Printable(cb->text()));
		ok(cb->text() == ExportDoneDialog::folderPathFor(path), "and now the folder path");

		cb->clear();
		buttonNamed(&dlg, QStringLiteral("Copy File"))->click();
		const QMimeData *m = cb->mimeData();
		ok(m && m->hasUrls(), "Copy File puts the FILE on it, as URLs");
		ok(m && m->hasUrls() && m->urls().first().toLocalFile() ==
					       QFileInfo(path).absoluteFilePath(),
		   "and it is the right file");
		// The distinction the three buttons exist for. Copy File must paste a
		// FILE; Copy File Path must paste a string. If both put URLs on the
		// clipboard the buttons would be the same button, and the check above
		// would pass while the feature was pointless.
		std::printf("     urls on clipboard: after Copy File Path %s, after Copy File %s\n",
			    pathHadUrls ? "yes" : "no", (m && m->hasUrls()) ? "yes" : "no");
		ok(!pathHadUrls,
		   "CONTROL: Copy File Path leaves only text -- the two buttons really differ");
	}

	std::printf("\n-- and nothing dismisses the window --\n");
	{
		QSignalSpy acted(&dlg, &ExportDoneDialog::actionTriggered);
		int stillUp = 0;
		for (const char *label :
		     {"Copy File", "Copy File Path", "Copy Folder Path", "Open File", "Open Folder"}) {
			buttonNamed(&dlg, QString::fromLatin1(label))->click();
			QApplication::processEvents();
			if (dlg.isVisible())
				++stillUp;
			else
				std::printf("     CLOSED BY: %s\n", label);
		}
		std::printf("     5 actions clicked, dialog still up after %d of them\n", stillUp);
		ok(stillUp == 5, "every action leaves the window open");
		ok(acted.count() == 5, "CONTROL: and all five really ran, rather than doing nothing");

		// Close is the only thing that ends it.
		buttonNamed(&dlg, QStringLiteral("Close"))->click();
		QApplication::processEvents();
		ok(!dlg.isVisible(), "Close closes it");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
