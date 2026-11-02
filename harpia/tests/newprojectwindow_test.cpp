// New Project must release the old document only after Save/Discard succeeds.
#include "editor/VideoEditorWindow.hpp"
#include "editor/timeline/TimelineJson.hpp"
#include "editor/RecentProjects.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QJsonDocument>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>

using namespace harpia;
static int failures;
static void ok(bool pass, const char *why)
{
	std::printf("  %s %s\n", pass ? "PASS" : "FAIL", why);
	failures += !pass;
}
static void settle(int ms = 100)
{
	QElapsedTimer timer;
	timer.start();
	while (timer.elapsed() < ms) {
		QApplication::processEvents();
		QThread::msleep(5);
	}
}
static QByteArray readFile(const QString &path)
{
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
	QApplication app(argc, argv);
	QStandardPaths::setTestModeEnabled(true);
	QTemporaryDir scratch;
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch.filePath("settings"));
	QApplication::clipboard()->clear();
	const QString media = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
	VideoEditorWindow editor(media + QStringLiteral("/av_red.mp4"));
	editor.resize(1400, 900);
	editor.show();
	settle();
	ok(editor.activeSourceIdForTest() >= 0, "editor begins with real media loaded");
	for (auto *button : editor.findChildren<QPushButton *>())
		if (button->text() == QStringLiteral("Full Editing")) {
			button->click();
			break;
		}
	auto *view = editor.findChild<TimelineView *>();
	auto *action = editor.findChild<QAction *>(QStringLiteral("newProjectAction"));
	auto *bar = editor.findChild<QMenuBar *>();
	ok(view && action && bar && bar->actions().first()->menu()->actions().first() == action,
	   "New project is first in File menu");
	if (!view || !action)
		return 2;
	QShortcut *newKey = nullptr, *undoKey = nullptr, *copyKey = nullptr;
	for (auto *key : editor.findChildren<QShortcut *>()) {
		if (key->key() == QKeySequence(QKeySequence::New)) newKey = key;
		if (key->key() == QKeySequence(QKeySequence::Undo)) undoKey = key;
		if (key->key() == QKeySequence(QKeySequence::Copy)) copyKey = key;
	}
	ok(newKey && newKey->isEnabled(), "New project is registered with the standard Ctrl+N shortcut");

	QString choice;
	int prompts = 0, savePickers = 0;
	QTimer dialogs;
	dialogs.setInterval(10);
	QObject::connect(&dialogs, &QTimer::timeout, [&]() {
		auto *modal = QApplication::activeModalWidget();
		if (auto *picker = qobject_cast<QFileDialog *>(modal)) {
			++savePickers;
			picker->reject();
		} else if (auto *box = qobject_cast<QMessageBox *>(modal)) {
			box->accept();
		} else if (modal && modal->windowTitle() == QStringLiteral("Unsaved Changes")) {
			for (auto *button : modal->findChildren<QPushButton *>())
				if (button->text() == choice) {
					++prompts;
					button->click();
					break;
				}
		}
	});
	dialogs.start();
	const QString previousPath = scratch.filePath("previous.harpiaproj");
	ok(editor.saveProjectTo(previousPath, false).isEmpty(), "existing project is saved before resetting");
	view->selectClip(0, 0);
	view->applyToSelection([](TlClip &clip) { clip.rotation = 17; });
	if (copyKey) emit copyKey->activated();
	settle(400);
	const TimelineModel edited = view->model();
	const QStringList recents = recentProjects(QString());
	choice = QStringLiteral("Cancel");
	action->trigger();
	ok(prompts == 1 && view->model() == edited && editor.activeSourceIdForTest() >= 0,
	   "Cancel preserves timeline and loaded media");
	choice = QStringLiteral("Save Project");
	action->trigger();
	settle();
	const QByteArray saved = readFile(previousPath);
	QJsonObject expected;
	timelineToProject(edited, expected);
	ok(QJsonDocument::fromJson(saved).object().value(QStringLiteral("tracks")) ==
		   expected.value(QStringLiteral("tracks")),
	   "Save Project writes edits to the existing project before starting fresh");
	ok(view->model().tracks.isEmpty() && !view->hasSelection() && view->playhead() == 0,
	   "new project clears tracks, selection and playhead");
	ok(editor.activeSourceIdForTest() == -1 && editor.sourcePathForTest(edited.tracks[0].clips[0].sourceId).isEmpty(),
	   "new project releases old source media");
	auto *empty = editor.findChild<QWidget *>(QStringLiteral("emptyProjectPanel"));
	ok(empty && empty->isVisible() && editor.windowTitle() == QStringLiteral("Video Editor"),
	   "same editor returns to its blank opening screen and title");
	if (undoKey) emit undoKey->activated();
	ok(view->model().tracks.isEmpty(), "Undo cannot restore clips from the previous project");
	QMetaObject::invokeMethod(&editor, "pasteFromClipboard", Qt::DirectConnection);
	ok(view->model().tracks.isEmpty(), "old internal clip clipboard does not leak into the new project");
	ok(recentProjects(QString()) == recents, "starting fresh preserves saved recent-project entries");
	QMetaObject::invokeMethod(&editor, "onSaveProject", Qt::DirectConnection);
	ok(savePickers == 1 && readFile(previousPath) == saved,
	   "first Save uses Save As and cannot overwrite the previous project");

	TlClip text;
	text.type = TlClip::Type::Text;
	text.srcEndMs = 1000;
	text.text.text = QStringLiteral("Discard only this new edit");
	view->addClip(TlTrack::Kind::Video, text);
	choice = QStringLiteral("Don't Save");
	if (newKey) emit newKey->activated();
	ok(prompts == 3 && view->model().tracks.isEmpty() && readFile(previousPath) == saved,
	   "Don't Save starts fresh through Ctrl+N without changing an existing file");
	const int before = prompts;
	if (newKey) emit newKey->activated();
	ok(prompts == before && view->model().tracks.isEmpty(), "New project on a blank editor needs no discard prompt");
	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
