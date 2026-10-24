// The opening screen remembers saved projects across editor windows, and its
// recent-project buttons reopen the actual timeline without another picker.
#include "editor/RecentProjects.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineView.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void ok(bool c, const char *what)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", what);
	if (!c)
		++failures;
}

static void settle(int ms = 100)
{
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < ms) {
		QApplication::processEvents();
		QThread::msleep(5);
	}
}

static QPushButton *recentButton(QWidget &window, int n)
{
	return window.findChild<QPushButton *>(QStringLiteral("recentProject%1").arg(n));
}

static QString buttonPath(QWidget &window, int n)
{
	const QPushButton *b = recentButton(window, n);
	return b ? b->property("projectPath").toString() : QString();
}

static TimelineModel caption(const QString &text)
{
	TlClip c;
	c.type = TlClip::Type::Text;
	c.srcEndMs = 1000;
	c.text.text = text;
	TlTrack track;
	track.name = QStringLiteral("Caption");
	track.clips = {c};
	TimelineModel model;
	model.tracks = {track};
	return model;
}

static QString loadedCaption(QWidget &window)
{
	const TimelineView *tv = window.findChild<TimelineView *>();
	if (tv)
		for (const TlTrack &track : tv->model().tracks)
			for (const TlClip &c : track.clips)
				if (c.type == TlClip::Type::Text)
					return c.text.text;
	return {};
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	QTemporaryDir scratch;
	if (!scratch.isValid())
		return 2;
	QStandardPaths::setTestModeEnabled(true);
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch.filePath("settings"));
	QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
			   QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	settings.clear();
	settings.sync();

	// Only dismiss message boxes: a file picker or another unexpected modal
	// dialog must not silently pass this test.
	int messageCount = 0;
	QTimer dismissMessages;
	dismissMessages.setInterval(10);
	QObject::connect(&dismissMessages, &QTimer::timeout, [&]() {
		if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
			++messageCount;
			box->accept();
		}
	});
	dismissMessages.start();

	QStringList paths;
	{
		VideoEditorWindow editor{QString()};
		editor.resize(1200, 900);
		editor.show();
		settle();
		auto *empty = editor.findChild<QWidget *>(QStringLiteral("emptyProjectPanel"));
		auto *recent = editor.findChild<QWidget *>(QStringLiteral("recentProjectsPanel"));
		ok(empty && empty->isVisible(), "an empty editor displays its opening panel");
		ok(recent != nullptr, "the opening panel has a recent-project section");
		if (recent && recentProjects().isEmpty())
			ok(!recent->isVisible(), "the recent section is hidden when there are no projects");

		for (QPushButton *b : editor.findChildren<QPushButton *>())
			if (b->text() == QStringLiteral("Full Editing")) {
				b->click();
				break;
			}
		TimelineView *tv = editor.findChild<TimelineView *>();
		ok(tv != nullptr, "the editor has a timeline to save");
		if (!tv)
			return 1;
		for (int n = 1; n <= 4; ++n) {
			const QString path = scratch.filePath(QStringLiteral("Project %1.harpiaproj").arg(n));
			tv->setModelAndCommit(caption(QStringLiteral("Project %1 caption").arg(n)));
			const QString error = editor.saveProjectTo(path, false);
			ok(error.isEmpty() && QFileInfo::exists(path), "a regular save writes a project");
			paths << QFileInfo(path).canonicalFilePath();
		}
		ok(recentProjects(QString()) == QStringList({paths[3], paths[2], paths[1]}),
		   "four regular saves retain the last three in newest-first order");
		const QString sidecar = scratch.filePath(QStringLiteral("Project 4_autosave.harpiaproj"));
		ok(editor.saveProjectTo(sidecar, true).isEmpty(), "a quiet autosave still writes its sidecar");
		ok(recentProjects(QString()) == QStringList({paths[3], paths[2], paths[1]}),
		   "the autosave sidecar does not replace a recent project");
		const QString failed = scratch.filePath(QStringLiteral("missing/Failed.harpiaproj"));
		ok(!editor.saveProjectTo(failed, false).isEmpty(), "an unwritable destination reports a failed save");
		ok(!recentProjects(QString()).contains(failed), "failed saves do not enter recent projects");
	}

	{
		VideoEditorWindow editor{QString()};
		editor.resize(1200, 900);
		editor.show();
		settle();
		auto *empty = editor.findChild<QWidget *>(QStringLiteral("emptyProjectPanel"));
		auto *recent = editor.findChild<QWidget *>(QStringLiteral("recentProjectsPanel"));
		ok(recent && recent->isVisible(), "a new editor restores the recent-project list");
		ok(buttonPath(editor, 1) == paths[3] && buttonPath(editor, 2) == paths[2] &&
			   buttonPath(editor, 3) == paths[1],
		   "all three opening-screen buttons point to the persisted projects");
		ok(empty && empty->parentWidget()->rect().contains(empty->geometry()),
		   "the opening panel and its three projects fit inside the preview");
		if (empty && argc > 1)
			empty->grab().save(QString::fromLocal8Bit(argv[1]));
		if (empty) {
			QWidget *preview = empty->parentWidget();
			const QSize original = preview->size();
			preview->resize(original.width(), 240);
			settle();
			ok(preview->rect().contains(empty->geometry()),
			   "the opening panel also fits a short preview");
			auto *scroll = empty->findChild<QScrollArea *>(QStringLiteral("emptyProjectScroll"));
			ok(scroll && scroll->verticalScrollBar()->maximum() > 0,
			   "a short preview scrolls so all three projects remain accessible");
			if (scroll)
				scroll->ensureWidgetVisible(recentButton(editor, 3));
			settle();
			ok(scroll && recentButton(editor, 3) &&
				   scroll->viewport()->rect().intersects(QRect(
					   recentButton(editor, 3)->mapTo(scroll->viewport(), QPoint()),
					   recentButton(editor, 3)->size())),
			   "the third project can be reached by scrolling");
			preview->resize(original);
			settle();
		}
		auto *second = recentButton(editor, 2);
		ok(second && !second->toolTip().isEmpty(), "a recent project exposes its full location in a tooltip");
		const int beforeMessages = messageCount;
		if (second)
			second->click();
		settle();
		ok(loadedCaption(editor) == QStringLiteral("Project 3 caption"),
		   "clicking a recent project loads that project's timeline");
		ok(empty && !empty->isVisible(), "the opening panel disappears after loading the project");
		ok(messageCount == beforeMessages, "a successful recent-project click needs no confirmation");
		ok(recentProjects(QString()).value(0) == paths[2], "reopening a project moves it to the first position");
		ok(editor.openProjectAt(paths[1]), "opening an explicit project path still works");
		ok(recentProjects(QString()).value(0) == paths[1], "ordinary successful opens also refresh the list");
	}

	// A project can move after the panel was painted. The button must report
	// that failure and leave the current editor usable.
	{
		VideoEditorWindow editor{QString()};
		editor.resize(1200, 900);
		editor.show();
		settle();
		auto *first = recentButton(editor, 1);
		ok(first && buttonPath(editor, 1) == paths[1], "the newest project is ready to reopen");
		ok(QFile::remove(paths[1]), "the recent file can disappear after the screen is shown");
		const int beforeMessages = messageCount;
		if (first)
			first->click();
		settle();
		ok(messageCount > beforeMessages, "a missing recent project produces visible feedback");
		ok(loadedCaption(editor).isEmpty(), "a failed recent open does not create a timeline");
		auto *empty = editor.findChild<QWidget *>(QStringLiteral("emptyProjectPanel"));
		ok(empty && empty->isVisible(), "the opening screen remains usable after a failed open");
		ok(buttonPath(editor, 1) != paths[1], "the disappeared file is removed from the visible recent list");
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
