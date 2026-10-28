// The recorder and editor are independent working windows: editing, modal
// editor tools and closing a project must not strand a live tutorial take.
#include <QtWidgets>
#include <QTest>
#include <QSignalSpy>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#define private public
#include "ui/MainWindow.hpp"
#include "ui/RecorderControlsOverlay.hpp"
#include "editor/VideoEditorWindow.hpp"
#undef private
#include "core/ObsContext.hpp"
#include "model/PresetStore.hpp"
#include <obs.h>

#include <cstdio>

using namespace harpia;
namespace {
int failures = 0;
std::atomic<int> frames{0};
void check(bool condition, const char *description)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
	failures += !condition;
}
bool waitFor(const std::function<bool()> &condition, int timeoutMs = 3000)
{
	QElapsedTimer timeout;
	timeout.start();
	while (!condition() && timeout.elapsed() < timeoutMs)
		QTest::qWait(10);
	return condition();
}
void settle()
{
	QApplication::processEvents();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
void nativeClick(QWidget *widget)
{
	if (!widget || !widget->window()->windowHandle())
		return;
	// OBS schedules pause/resume at least two frames ahead. A callback can
	// still precede that timestamp, so allow four fresh frames before pausing.
	if (widget->toolTip() == QStringLiteral("Pause recording")) {
		const int previous = frames.load();
		waitFor([previous]() { return frames.load() > previous + 3; });
	}
	// A QWidget sendEvent bypasses Qt's modal-window dispatch. Send through the
	// native window instead, as a physical click reaches this floating panel.
	QWidget *window = widget->window();
	QTest::mouseClick(window->windowHandle(), Qt::LeftButton, Qt::NoModifier,
			  widget->mapTo(window, widget->rect().center()));
	settle();
}
void registerOutput()
{
	obs_output_info output{};
	output.id = "harpia_recorder_editor_test_output";
	output.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_CAN_PAUSE;
	output.get_name = [](void *) { return "Recorder/editor tutorial test"; };
	output.create = [](obs_data_t *, obs_output_t *self) -> void * { return self; };
	output.destroy = [](void *) {};
	output.start = [](void *self) { return obs_output_begin_data_capture(static_cast<obs_output_t *>(self), 0); };
	output.stop = [](void *self, uint64_t) { obs_output_end_data_capture(static_cast<obs_output_t *>(self)); };
	output.raw_video = [](void *, video_data *) { ++frames; };
	obs_register_output(&output);
}
bool startTake(MainWindow &window)
{
	window.recorder_.output_ = obs_output_create("harpia_recorder_editor_test_output", "tutorial-take", nullptr, nullptr);
	window.recStartMs_ = QDateTime::currentMSecsSinceEpoch();
	window.outputStopped_.store(false);
	const bool started = window.recorder_.output_ && obs_output_start(window.recorder_.output_);
	window.updateButtons();
	return started;
}
void releaseTake(MainWindow &window)
{
	if (obs_output_t *output = window.recorder_.output_) {
		obs_output_stop(output);
		waitFor([output]() { return !obs_output_active(output); });
		obs_output_release(output);
		window.recorder_.output_ = nullptr;
	}
	// This test's raw output has no muxed file to finalize. Production's
	// RecordingController callback/tick owns these flags for encoded outputs.
	window.stopping_ = false;
	window.outputStopped_.store(true);
	window.updateButtons();
}
QPointer<VideoEditorWindow> visibleEditor()
{
	for (QWidget *widget : QApplication::topLevelWidgets())
		if (auto *editor = qobject_cast<VideoEditorWindow *>(widget); editor && editor->isVisible())
			return editor;
	return {};
}
void buildProject(VideoEditorWindow &editor)
{
	editor.setEditMode(VideoEditorWindow::EditMode::Full);
	TlClip clip;
	clip.type = TlClip::Type::Text;
	clip.srcEndMs = 3000;
	clip.text.text = QStringLiteral("Recording an editing tutorial");
	TlTrack track;
	track.clips = {clip};
	TimelineModel timeline;
	timeline.tracks = {track};
	editor.timelineView_->setModel(timeline);
	editor.timelineView_->selectClip(0, 0);
	editor.syncClipInspector();
	editor.commitSnapshot();
}
bool cancelClose(const std::function<bool()> &close, bool &sawPrompt, const std::function<void(QDialog *)> &inspect = {})
{
	bool timedOut = false;
	QTimer driver, watchdog;
	driver.setInterval(10);
	watchdog.setSingleShot(true);
	QObject::connect(&driver, &QTimer::timeout, [&]() {
		auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
		if (!dialog || dialog->windowTitle() != QStringLiteral("Unsaved Changes"))
			return;
		sawPrompt = true;
		if (inspect)
			inspect(dialog);
		for (auto *button : dialog->findChildren<QPushButton *>())
			if (button->text() == QStringLiteral("Cancel")) {
				button->click();
				return;
			}
	});
	QObject::connect(&watchdog, &QTimer::timeout, [&]() {
		timedOut = true;
		if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
			dialog->done(0);
	});
	driver.start();
	watchdog.start(4000);
	const bool closed = close();
	return !closed && sawPrompt && !timedOut;
}
}

int main(int argc, char **argv)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
	Q_UNUSED(argc);
	Q_UNUSED(argv);
	std::fprintf(stderr, "This fixture requires a file-based native QSettings store.\n");
	return 77;
#endif
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir temporary;
	if (!temporary.isValid())
		return 2;
	qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
	qputenv("XDG_CACHE_HOME", temporary.filePath("cache").toUtf8());
	qputenv("XDG_DATA_HOME", temporary.filePath("data").toUtf8());
	QApplication app(argc, argv);
	QCoreApplication::setOrganizationName(QStringLiteral("HarpiaRecorderEditorTest"));
	QCoreApplication::setApplicationName(QStringLiteral("RecorderEditor"));
	app.setQuitOnLastWindowClosed(false);
	for (auto format : {QSettings::NativeFormat, QSettings::IniFormat}) {
		QSettings::setPath(format, QSettings::UserScope, temporary.path());
		QSettings::setPath(format, QSettings::SystemScope, temporary.filePath("system"));
	}
	ObsContext obs;
	check(obs.startup() && obs.resetVideo(320, 180, 30) == OBS_VIDEO_SUCCESS,
	      "real OBS video initializes for concurrent recorder/editor operation");
	if (failures)
		return 1;
	registerOutput();
	QDir().mkpath(temporary.filePath("config"));
	QDir().mkpath(temporary.filePath("Recordings"));
	PresetStore presets(temporary.filePath("config").toStdString());
	presets.load(temporary.filePath("Recordings").toStdString());
	{
		MainWindow recorder(obs, presets, temporary.filePath("Recordings"));
		for (auto *timer : recorder.findChildren<QTimer *>())
			timer->stop();
		recorder.show();
		settle();
		const bool recordAvailable = recorder.primaryButton_->isEnabled();
		bool nestedLoop = false;
		QTimer rescue;
		rescue.setSingleShot(true);
		QObject::connect(&rescue, &QTimer::timeout, [&]() {
			nestedLoop = true;
			if (auto editor = visibleEditor())
				editor->QDialog::reject();
		});
		rescue.start(1500);
		recorder.openBlankEditor();
		rescue.stop();
		settle();
		QPointer<VideoEditorWindow> editor = visibleEditor();
		check(!nestedLoop && editor, "opening the editor returns immediately and leaves an independent editor visible");
		if (!editor)
			return 1;
		check(editor->windowModality() == Qt::NonModal && !QApplication::activeModalWidget(),
		      "the editor does not block recorder input as an application-modal dialog");
		check(!editor->parentWidget() && editor->windowHandle() && !editor->windowHandle()->transientParent(),
		      "the editor has its own native window rather than an owned recorder dialog");
		check(recorder.isEnabled() && recorder.primaryButton_->isEnabled() == recordAvailable,
		      "opening the editor preserves recorder input and its existing readiness state");
		recorder.updateFloatingControls();
		check(recorder.floatingControls_->isVisible() && recorder.floatingControls_->startButton_->isVisible(),
		      "the idle floating Record button remains available over the editor");
		recorder.floatingDismissed_ = true;
		recorder.updateFloatingControls();
		check(!recorder.floatingControls_->isVisible(), "the editor respects the user's temporary idle-HUD dismissal");
		recorder.floatingDismissed_ = false;
		QSettings hud(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		hud.setValue(QStringLiteral("floatingControls/onlyWhileRecording"), true);
		recorder.updateFloatingControls();
		check(!recorder.floatingControls_->isVisible(), "Only show while recording still hides the idle HUD beside the editor");
		hud.setValue(QStringLiteral("floatingControls/onlyWhileRecording"), false);
		buildProject(*editor);
		check(startTake(recorder) && waitFor([]() { return frames.load() >= 2; }),
		      "a real recording output starts and receives frames with the editor open");
		if (!recorder.recorder_.isRecording())
			return 1;
		const auto before = editor->timelineView_->model();
		const int beforeFrames = frames.load();
		emit editor->componentPanel_->pinnedEdited(QStringLiteral("harpia.transform"), QStringLiteral("rotation"), 32);
		editor->commitSnapshot();
		check(editor->timelineView_->model().tracks[0].clips[0].rotation == 32 &&
		      waitFor([beforeFrames]() { return frames.load() > beforeFrames + 2; }),
		      "timeline edits apply while the OBS output continues capturing frames");
		editor->undo();
		check(editor->timelineView_->model() == before && recorder.recorder_.isRecording(),
		      "Undo works in the editor without affecting the live take");
		editor->redo();
		check(editor->timelineView_->model().tracks[0].clips[0].rotation == 32,
		      "Redo remains attached to the editor while recording controls coexist");
		recorder.showMinimized();
		settle();
		check(editor->isVisible() && !editor->isMinimized() && recorder.recorder_.isRecording(),
		      "minimizing the recorder leaves the tutorial editor and recording running");
		recorder.showNormal();
		recorder.updateFloatingControls();
		nativeClick(recorder.floatingControls_->pauseButton_);
		check(recorder.recorder_.isPaused(), "the native floating Pause button reaches the backend while the editor is open");
		nativeClick(recorder.floatingControls_->pauseButton_);
		check(!recorder.recorder_.isPaused(), "the floating Resume button resumes the same tutorial take");
		editor->activateWindow();
		QTest::qWait(80);
		const int resumeFrame = frames.load();
		check(waitFor([resumeFrame]() { return frames.load() > resumeFrame + 3; }),
		      "the raw output reaches its resume timestamp before testing the next pause gesture");
		QSignalSpy shortcutSpy(recorder.pauseShortcut_, &QShortcut::activated);
		QSignalSpy ambiguousSpy(recorder.pauseShortcut_, &QShortcut::activatedAmbiguously);
		QSignalSpy clickSpy(recorder.floatingControls_->pauseButton_, &QPushButton::clicked);
		QTest::keyClick(editor->windowHandle(), Qt::Key_F10);
		settle();
		check(shortcutSpy.count() == 1 && ambiguousSpy.isEmpty(),
		      "native F10 input reaches the recorder shortcut exactly once from the editor");
		check(recorder.recorder_.isPaused(), "the recorder's fallback pause shortcut works with the editor focused");
		nativeClick(recorder.floatingControls_->pauseButton_);
		check(!recorder.recorder_.isPaused(), "the HUD resumes a take paused by the editor-focused recording shortcut");
		{
			QDialog tool(editor);
			tool.setWindowTitle(QStringLiteral("Editor tool"));
			tool.setModal(true);
			tool.show();
			settle();
			check(tool.windowModality() == Qt::WindowModal,
			      "an application-modal editor tool is scoped to its editor window");
			const int beforeClicks = int(clickSpy.count());
			nativeClick(recorder.floatingControls_->pauseButton_);
			check(clickSpy.count() == beforeClicks + 1,
			      "native mouse input still reaches the floating button while an editor tool is modal");
			check(recorder.recorder_.isPaused(), "editor tool dialogs do not block the native floating Pause control");
			tool.reject();
			settle();
		}
		nativeClick(recorder.floatingControls_->pauseButton_);
		check(!recorder.recorder_.isPaused(), "recording resumes normally after an editor tool closes");
		bool sawPrompt = false, promptScoped = false, pausedThroughPrompt = false;
		check(cancelClose([&]() { return editor->close(); }, sawPrompt, [&](QDialog *prompt) {
			promptScoped = prompt->windowModality() == Qt::WindowModal;
			nativeClick(recorder.floatingControls_->pauseButton_);
			pausedThroughPrompt = recorder.recorder_.isPaused();
		}), "Cancel preserves an unsaved editor while a recording is active");
		check(promptScoped && pausedThroughPrompt && editor && editor->isVisible() && recorder.recorder_.isRecording(),
		      "the unsaved-changes prompt keeps recorder Pause reachable and retains both working windows");
		if (recorder.recorder_.isPaused())
			nativeClick(recorder.floatingControls_->pauseButton_);
		nativeClick(recorder.floatingControls_->stopButton_);
		check(waitFor([&]() { return !recorder.recorder_.isRecording(); }),
		      "the floating Stop button ends the actual output without closing the editor");
		check(editor && editor->isVisible() && !editor->timelineView_->model().isEmpty(),
		      "stopping a tutorial take preserves the edited project");
		releaseTake(recorder);
		sawPrompt = false;
		check(cancelClose([&]() { return recorder.close(); }, sawPrompt),
		      "closing the recorder asks about an open unsaved editor and respects Cancel");
		check(recorder.isVisible() && editor && editor->isVisible(),
		      "canceling application close preserves the recorder and project windows");
		if (editor) {
			editor->QDialog::reject();
			settle();
		}
		check(!editor && !VideoEditorWindow::anyOpen(), "closing an editor releases its independent window and open-editor count");
		recorder.close();
	}
	obs.shutdown();
	std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
	return failures ? 1 : 0;
}
