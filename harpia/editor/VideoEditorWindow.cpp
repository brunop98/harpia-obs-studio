#include "VideoEditorWindow.hpp"
#include "AppName.hpp"
#include "../ui/EditorLog.hpp"
#include "timeline/SoundLibrary.hpp"
#include "timeline/ClipShuffle.hpp"
#include "timeline/SoundPresets.hpp"

#include <QTextCursor>
#include <QUuid>

#include "../ui/ExportDoneDialog.hpp"
#include "../ui/ExportProgressDialog.hpp"
#include "../ui/LoadingDialog.hpp"
#include "../ui/RecordingVideoPicker.hpp"
#include "../ui/MediaFileDetailsDialog.hpp"
#include "../ui/WindowScreen.hpp"
#include "LoadingTask.hpp"
#include <QScopedValueRollback>
#include "../ui/ColorField.hpp"
#include "../ui/UiIcons.hpp"
#include "component/BuiltinComponents.hpp"
#include "component/ComponentRegistry.hpp"
#include "component/ScriptComponent.hpp"
#include "../ui/UiText.hpp"
#include "../ui/InfoHint.hpp"
#include "KeyList.hpp"
#include "Parallel.hpp"
#include "ParamSlider.hpp"
#include "StillImage.hpp"
#include "TimeText.hpp"

#include "AudioRecorder.hpp"
#include "SlipDialog.hpp"
#include "SystemAudioCapture.hpp"
#include "WavSilence.hpp"
#include "ClipExporter.hpp"
#include "DevPanel.hpp"
#include "EditorWidgets.hpp"
#include "ExportOptionsDialog.hpp"
#include "BatchExport.hpp"
#include "FlowLayout.hpp"
#include "InspectorSection.hpp"
#include "ToolbarLayout.hpp"
#include "timeline/TextVariations.hpp"
#include "timeline/ClipVariations.hpp"
#include "timeline/VariationPresets.hpp"
#include "FrameSeeker.hpp"
#include "PreviewDecoder.hpp"
#include "PreviewRenderWarning.hpp"
#include "ProjectPaths.hpp"
#include "RecentProjects.hpp"
#include "ProxyMedia.hpp"
#include "LevelMeter.hpp"
#include "SceneDetector.hpp"
#include "AudioPreview.hpp"
#include "TimelineAudio.hpp"
#include "TimelineOverview.hpp"
#include "TimelineThumbs.hpp"
#include "timeline/TextStyleJson.hpp"
#include "timeline/TextStylePresets.hpp"
#include "SilenceTrim.hpp"
#include "SilenceTrimDialog.hpp"
#include "BeatDetectionDialog.hpp"
#include "timeline/FrameSignature.hpp"
#include "TrackEditor.hpp"
#include "VoiceoverMixer.hpp"
#include "VoiceoverTrack.hpp"
#include "MediaFiles.hpp"
#include "ClipboardPaste.hpp"
#include "LinkDownload.hpp"
#include "../ui/WheelGuard.hpp"
#include "script/TransformScript.hpp"
#include "component/ShaderComponent.hpp"
#include "component/TransformScriptComponent.hpp"
#include "shader/ShaderRenderer.hpp"
#include "timeline/TimelineCompositor.hpp"
#include "ShortcutPanel.hpp"
#include "ShortcutRegistry.hpp"
#include "EditConsole.hpp"
#include <QRegularExpression>
#include "../ui/MenuHints.hpp"
#include "StillWeight.hpp"
#include "shader/GpuFxGl.hpp"
#include "subtitles/SubtitleDialog.hpp"
#include "tts/SpeechPlacement.hpp"
#include "tts/TtsDialog.hpp"
#include "tts/TtsEngine.hpp"
#include "tts/TtsText.hpp"
#include "timeline/MotionPath.hpp"
#include "ytdlp/UrlDownloadDialog.hpp"
#include "ytdlp/YtDlp.hpp"
#include "../ui/UrlDragList.hpp"
#include "timeline/KeyframeEditor.hpp"
#include "CanvasFit.hpp"
#include "DeleteRouting.hpp"
#include "timeline/TimelineSlice.hpp"
#include "timeline/TimelineView.hpp"
#include "timeline/Transitions.hpp"

#include "../Version.hpp"
#include "../core/Logger.hpp"
#include "../ui/ErrorLogsPanel.hpp"

#include <QCheckBox>
#include <QRadioButton>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <random>
#include <QRandomGenerator>
#include <QGridLayout>
#include <QStringListModel>
#include <QCompleter>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QButtonGroup>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <QApplication>
#include <QDateTime>
#include <QClipboard>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QScrollBar>
#include <QScreen>
#include <QFileSystemWatcher>
#include <QFormLayout>
#include <QTabBar>
#include <QFrame>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QAction>
#include <QActionGroup>
#include <QColorDialog>
#include <QColorDialog>
#include <QFontComboBox>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QProcess>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QProgressDialog>
#include <QPushButton>
#include <QImageReader>
#include <QScrollArea>
#include <QCryptographicHash>
#include <QSet>
#include <QSettings>
#include <QStyle>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QKeyEvent>
#include <QIcon>
#include <QListWidget>
#include <QMimeData>
#include <QPixmap>
#include <QTabWidget>

#include "../library/ClipLibrary.hpp"
#include "../library/ThumbnailCache.hpp"
#include <QPointer>
#include <QMouseEvent>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace harpia {

namespace {
// An application filter sees child controls before they consume or propagate
// side-button events. Keep it separate from the editor's local event filter,
// whose canvas/console handlers must still run just once.
class EditorMouseHistory final : public QObject {
public:
	EditorMouseHistory(QWidget *editor, std::function<void()> undo, std::function<void()> redo)
		: QObject(editor), editor_(editor), undo_(std::move(undo)), redo_(std::move(redo))
	{
		qApp->installEventFilter(this);
	}
	~EditorMouseHistory() override
	{
		if (qApp)
			qApp->removeEventFilter(this);
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		const auto type = event->type();
		if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonDblClick &&
		    type != QEvent::MouseButtonRelease)
			return false;
		auto *mouse = static_cast<QMouseEvent *>(event);
		if (mouse->button() != Qt::BackButton && mouse->button() != Qt::ForwardButton)
			return false;
		auto *receiver = qobject_cast<QWidget *>(watched);
		const QWidget *modal = QApplication::activeModalWidget();
		if (!editor_->isVisible() || !editor_->isEnabled() || !belongs(receiver) ||
		    !belongs(QApplication::activeWindow()) || QApplication::activePopupWidget() ||
		    (modal && modal != editor_))
			return false;
		mouse->accept();
		// Swallow release as well: some preview/track handlers finish their
		// current edit on any release. Chording a side button with a drag must
		// neither restore history under that drag nor end it prematurely.
		if (type != QEvent::MouseButtonRelease &&
		    !(mouse->buttons() & (Qt::LeftButton | Qt::MiddleButton | Qt::RightButton))) {
			const auto run = mouse->button() == Qt::BackButton ? undo_ : redo_;
			run(); // a double-click event represents the second physical press
		}
		return true;
	}

private:
	bool belongs(const QWidget *widget) const
	{
		// QWidget::isAncestorOf stops at a window boundary. Owned floating
		// tools have their own window but still belong to this editor.
		for (auto *owner = widget; owner; owner = owner->parentWidget())
			if (owner == editor_)
				return true;
		return false;
	}
	QWidget *editor_;
	std::function<void()> undo_;
	std::function<void()> redo_;
};

// Shown on every caption text field: the tags it takes.
const QString kRichTextTip = QStringLiteral(
	"Rich text, as in Unity:\n"
	"  <b>bold</b>   <i>italic</i>   <u>underline</u>   <s>strike</s>\n"
	"  <color=#ff8800>colour</color>   (#rgb, #rrggbb, #rrggbbaa or a name: red, yellow…)\n"
	"  <size=90>size</size>   (also <size=150%> and <size=+20>)\n"
	"  <alpha=#80>see-through   <mark=#ffff0080>highlight</mark>   <br> new line\n"
	"  <noparse><b></noparse> shows a tag as typed");
} // namespace



namespace {
QString previewTimeText(qint64 ms)
{
	return timeTextMs(ms);
}

// Speed control: 0.1×..50×, mapped exponentially onto the slider so each step
// multiplies by a constant factor (fine control at low speeds, like CapCut).
constexpr double kMinSpeed = 0.1;
constexpr double kMaxSpeed = 50.0;
constexpr int kSpeedTicks = 1000;
double sliderToSpeed(int v)
{
	return kMinSpeed * std::pow(kMaxSpeed / kMinSpeed, double(v) / kSpeedTicks);
}
int speedToSlider(double sp)
{
	sp = std::clamp(sp, kMinSpeed, kMaxSpeed);
	return int(std::lround(kSpeedTicks * std::log(sp / kMinSpeed) / std::log(kMaxSpeed / kMinSpeed)));
}

void revealInFolder(const QString &path)
{
#ifdef Q_OS_WIN
	// explorer's "/select,<path>" must reach it verbatim. Passed through the
	// normal argument list, Qt quotes the whole "/select,C:\…\clip.mp4" token,
	// which Explorer can't parse — it then opens a default location instead of
	// the file's folder. setNativeArguments bypasses Qt's quoting.
	const QString native = QDir::toNativeSeparators(path);
	QProcess p;
	p.setProgram(QStringLiteral("explorer.exe"));
	p.setNativeArguments(QStringLiteral("/select,\"%1\"").arg(native));
	if (!p.startDetached()) // fall back to just opening the containing folder
		QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#else
	QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}
} // namespace


// A top-level operation owns one dialog. Nested media imports reuse it, so a
// project or a multi-file drop does not flash a separate window for each file.
// Only independent preparation runs with an event loop; GUI registration and
// applying a project snapshot stay on the GUI thread without event pumping.
class VideoEditorWindow::LoadingScope {
public:
	LoadingScope(VideoEditorWindow &editor, const QString &title) : editor_(editor)
	{
		if (editor_.loadingDialog_)
			return;
		if (editor_.voCountdownTimer_ && editor_.voCountdownTimer_->isActive())
			editor_.onVoiceoverRecordClicked();
		if (editor_.voRecording_)
			editor_.finishVoiceover();
		if (editor_.playing_)
			editor_.stopPlayback();
		if (editor_.histTimer_ && editor_.histTimer_->isActive()) {
			editor_.histTimer_->stop();
			editor_.captureSnapshot();
		}
		if (editor_.previewTimer_)
			editor_.previewTimer_->stop();
		editor_.pendingMs_ = -1;
		if (editor_.audioPremixTimer_)
			editor_.audioPremixTimer_->stop();
		if (editor_.autosaveTimer_ && editor_.autosaveTimer_->isActive()) {
			autosaveActive_ = true;
			editor_.autosaveTimer_->stop();
		}
		owned_ = std::make_unique<LoadingDialog>(title, &editor_);
		editor_.loadingDialog_ = owned_.get();
	}
	~LoadingScope()
	{
		if (!owned_)
			return;
		owned_->finish();
		editor_.loadingDialog_ = nullptr;
		editor_.flushPendingPlaybackProxies();
		if (autosaveActive_ && editor_.autosaveTimer_)
			editor_.autosaveTimer_->start();
		const auto callbacks = std::move(editor_.afterLoading_);
		editor_.afterLoading_.clear();
		for (const auto &callback : callbacks)
			QTimer::singleShot(0, &editor_, callback);
		editor_.schedulePremix();
		editor_.scheduleSnapshot();
		// A ready frame may have arrived while requests were paused.
		if (editor_.valid_ && !editor_.shownExact_ && editor_.shownMs_ >= 0)
			editor_.requestPreview(editor_.shownSource_, editor_.shownMs_);
	}
	void stage(const QString &message, const QString &detail = QString())
	{
		if (auto *dialog = editor_.loadingDialog_) {
			dialog->setStage(message, detail);
			if (editor_.loadingTotal_ > 0)
				dialog->setItemProgress(editor_.loadingCompleted_, editor_.loadingTotal_);
			dialog->show();
			dialog->repaint();
		}
	}
	void hide()
	{
		if (editor_.loadingDialog_)
			editor_.loadingDialog_->hide();
	}
private:
	VideoEditorWindow &editor_;
	std::unique_ptr<LoadingDialog> owned_;
	bool autosaveActive_ = false;
};

// Modeless tools can finish while a file is being prepared. Run their timeline
// edits after loading completes; results for a replaced project are discarded.
bool VideoEditorWindow::deferUntilLoaded(std::function<void()> callback)
{
	if (!loadingDialog_)
		return false;
	if (!loadingProject_) {
		const quint64 generation = projectLoadGeneration_;
		afterLoading_.append([this, generation, callback = std::move(callback)]() {
			if (generation == projectLoadGeneration_)
				callback();
		});
	}
	return true;
}

namespace {
struct ProjectRead {
	QJsonObject root;
	QString error;
};
ProjectRead readProjectFile(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {{}, QStringLiteral("Could not read the project file.")};
	QJsonParseError error;
	const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
	if (error.error != QJsonParseError::NoError || !doc.isObject() ||
	    !doc.object().contains(QStringLiteral("harpiaProject")))
		return {{}, QStringLiteral("This is not a valid Harpia project file.")};
	return {doc.object(), {}};
}
} // namespace

int VideoEditorWindow::openCount_ = 0;

VideoEditorWindow::VideoEditorWindow(const QString &inPath, const QStringList &libraryFolders,
				     QWidget *parent)
	: QDialog(parent), inPath_(inPath), libraryFolders_(libraryFolders)
{
	++openCount_; // the recorder hides its region overlay while one is up
	// Effects on the GPU from here on, for the preview and the exporter alike
	// (each thread gets its own engine, or the CPU where GL is unusable).
	installGpuFx();
	setWindowTitle(inPath.isEmpty() ? QStringLiteral("Video Editor")
					: QStringLiteral("Edit — %1").arg(QFileInfo(inPath).fileName()));
	// A real window with minimize/maximize (QDialog hides them by default), so
	// the editor can use the full screen — the preview canvas takes the extra
	// space and the timelines widen for finer control.
	setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
		       Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
	setSizeGripEnabled(true);
	connect(this, &QDialog::finished, this, [this]() {
		if (shownOnce_)
			window_screen::save(*this, QStringLiteral("editor"));
	});
	// The size it RESTORES to (the title bar's restore button); the editor
	// itself opens MAXIMISED (see showEvent), because every part of it wants
	// the room -- the preview, the track stack and the Inspector are all things
	// you immediately drag bigger otherwise.
	resize(1040, 680);
	// NOT maximised here. Asked for before the window is on screen, Windows
	// could come up with the window maximised in its own books but drawn at
	// this restore size: the title bar showed Restore, so the first click on
	// it moved the window to the middle and only the second one maximised it
	// (v0.1.443). showEvent maximises once the window is showing -- the same
	// thing as that second click, which always worked.
	setAcceptDrops(true); // drop video files to add them as sources

	auto *root = new QVBoxLayout(this);
	rootLayout_ = root;

	// A draggable vertical splitter lets you trade preview height for timeline
	// height: more preview when reviewing, more timeline for detailed cut work.
	auto *splitter = new QSplitter(Qt::Vertical, this);
	splitter->setChildrenCollapsible(false);
	splitter->setHandleWidth(6);

	// ---- Top pane: the preview and a transport bar directly beneath it ----
	auto *topPane = new QWidget(this);
	auto *topLayout = new QVBoxLayout(topPane);
	topLayout->setContentsMargins(0, 0, 0, 0);
	topPane_ = topPane;
	topLayout_ = topLayout;
	canvas_ = new PreviewCanvas(this);
	// A child of the preview, so it floats over the picture and is clipped to
	// it, and it occupies no layout space -- layOutOverview() places it by hand
	// whenever the preview resizes. It does take the mouse, but only while it is
	// on screen: hidden widgets get no mouse events, so the preview keeps every
	// click except during the couple of seconds this is up.
	overview_ = new TimelineOverview(canvas_);
	canvas_->installEventFilter(this);
	topLayout->addWidget(canvas_, 1);
	previewRenderWarning_ = new PreviewRenderWarning(topPane);
	topLayout->addWidget(previewRenderWarning_);

	// One toolbar under the preview, in three zones (see ToolbarLayout):
	//   Left   the mode switch and the editing tools (Add, Snap, Fit, ...)
	//   Center undo/redo, the transport and timecode, speed
	//   Right  preview quality, monitor, voiceover, the size/length readout
	// It used to be two rows -- this one and a second above the timeline --
	// and in a narrow window the first squeezed its buttons until their labels
	// were cut off. Now it is one line when there is room and wraps when not.
	auto *barHost = new QWidget(this);
	barHost_ = barHost;
	toolbar_ = new ToolbarLayout(barHost, 6, 18, 4);
	auto *bar = toolbar_;
	// A group of controls that belong together, wrapped as one item so it
	// moves to the next line as a unit.
	const auto cluster = [this](QLayout *l) {
		auto *w = new QWidget(this);
		l->setContentsMargins(0, 0, 0, 0);
		w->setLayout(l);
		return w;
	};
	using Zone = ToolbarLayout::Zone;

	// Mode switch as a segmented control: the two modes are mutually exclusive,
	// so join them visually and enforce exclusivity with a button group.
	trimModeBtn_ = new QPushButton(QStringLiteral("Simple Trim"), this);
	trimModeBtn_->setCheckable(true);
	trimModeBtn_->setChecked(true);
	trimModeBtn_->setToolTip(QStringLiteral(
		"Keep one section of one video. The quickest path when that is all you need."));
	cutModeBtn_ = new QPushButton(QStringLiteral("Multi-Cut"), this);
	cutModeBtn_->setCheckable(true);
	cutModeBtn_->setToolTip(QStringLiteral(
		"Keep sections from one or more videos, joined in order, each at its own speed.\n\n"
		"Full Editing can do all of this too — Multi-Cut is the faster route when you "
		"only need to drop parts out."));
	fullModeBtn_ = new QPushButton(QStringLiteral("Full Editing"), this);
	fullModeBtn_->setCheckable(true);
	fullModeBtn_->setToolTip(QStringLiteral(
		"Everything the other two modes do, plus stacked tracks: overlays, "
		"picture-in-picture, captions, effect clips, transitions and separate audio.\n\n"
		"Use this whenever more than one thing has to be on screen at once."));
	auto *modeGroup = new QButtonGroup(this);
	modeGroup->setExclusive(true); // only one mode active; can't un-check both
	modeGroup->addButton(trimModeBtn_);
	modeGroup->addButton(cutModeBtn_);
	modeGroup->addButton(fullModeBtn_);
	// Three buttons a little apart, each with its own border, sharing a fill,
	// with the accent on the active one. (They used to touch, as one segmented
	// control, and read as a single cramped block.)
	const QString segBase = QStringLiteral(
		"QPushButton{background:#2b2f36;color:#c8ccd4;border:1px solid #3a3f47;border-radius:6px;"
		"padding:5px 14px;}"
		"QPushButton:checked{background:#3d7eff;color:#ffffff;border-color:#3d7eff;}");
	trimModeBtn_->setStyleSheet(segBase);
	cutModeBtn_->setStyleSheet(segBase);
	fullModeBtn_->setStyleSheet(segBase);
	auto *segBox = new QHBoxLayout;
	segBox->setSpacing(6);
	segBox->setContentsMargins(0, 0, 0, 0);
	segBox->addWidget(trimModeBtn_);
	segBox->addWidget(cutModeBtn_);
	segBox->addWidget(fullModeBtn_);
	bar->addWidget(cluster(segBox), Zone::Left);

	// The bridge between the fast modes and the powerful one. Shown only in
	// Trim and Multi-Cut, where there is something to send.
	sendToFullBtn_ = new QPushButton(QStringLiteral("Send to Full Editing →"), this);
	sendToFullBtn_->setToolTip(QStringLiteral(
		"Put this result on the Full Editing timeline as clips — one per cut, keeping "
		"every edit point — and switch to it.\n\nNothing is lost: the clips are ADDED "
		"after whatever is already there, and this mode keeps its own work too."));
	connect(sendToFullBtn_, &QPushButton::clicked, this, &VideoEditorWindow::sendToFullEditing);
	bar->addWidget(sendToFullBtn_, Zone::Left);

	// Undo/redo, grouped tightly as one cluster.
	auto *urBox = new QHBoxLayout;
	urBox->setSpacing(2);
	urBox->setContentsMargins(0, 0, 0, 0);
	undoBtn_ = new QPushButton(this);
	undoBtn_->setIcon(uiIcon(Glyph::Undo));
	undoBtn_->setToolTip(QStringLiteral("Undo (Ctrl+Z)"));
	undoBtn_->setFixedWidth(34);
	undoBtn_->setEnabled(false);
	connect(undoBtn_, &QPushButton::clicked, this, &VideoEditorWindow::undo);
	urBox->addWidget(undoBtn_);
	redoBtn_ = new QPushButton(this);
	redoBtn_->setIcon(uiIcon(Glyph::Redo));
	redoBtn_->setToolTip(QStringLiteral("Redo (Ctrl+Shift+Z)"));
	redoBtn_->setFixedWidth(34);
	redoBtn_->setEnabled(false);
	connect(redoBtn_, &QPushButton::clicked, this, &VideoEditorWindow::redo);
	urBox->addWidget(redoBtn_);
	bar->addWidget(cluster(urBox), Zone::Center);

	// Transport: reset · play · a big monospace timecode.
	auto *resetBtn = new QPushButton(this);
	resetBtn->setToolTip(QStringLiteral("Move the playhead back to the start"));
	resetBtn->setIcon(uiIcon(Glyph::SkipStart));
	resetBtn->setFixedWidth(40);
	connect(resetBtn, &QPushButton::clicked, this, &VideoEditorWindow::onResetMarker);
	auto *transport = new QHBoxLayout;
	transport->setSpacing(4);
	transport->addWidget(resetBtn);
	playBtn_ = new QPushButton(this);
	playBtn_->setObjectName(QStringLiteral("playButton"));
	playBtn_->setIcon(uiIcon(Glyph::Play));
	playBtn_->setToolTip(QStringLiteral("Play from the marker at the current speed"));
	playBtn_->setFixedWidth(40);
	transport->addWidget(playBtn_);
	transport->addSpacing(6);
	// Live cursor readout: the source time of the frame being previewed — large
	// and monospace so it reads as the editor's primary timecode.
	cursorTimeLabel_ = new QLabel(QStringLiteral("0:00.000"), this);
	cursorTimeLabel_->setStyleSheet(QStringLiteral(
		"color:#e8eaed; font-family:monospace; font-weight:bold; font-size:%1px;")
						.arg(uiTimecodePx()));
	cursorTimeLabel_->setToolTip(QStringLiteral("Time of the frame shown in the preview"));
	transport->addWidget(cursorTimeLabel_);
	bar->addWidget(cluster(transport), Zone::Center);

	// Preview quality. Everything in a preview frame — decode, composite, text,
	// shaders — scales with the rendered size, so this is the one knob that
	// speeds all of them up at once.
	previewQualityCombo_ = new QComboBox(this);
	previewQualityCombo_->addItem(QStringLiteral("Preview: Auto"), int(PreviewQuality::Auto));
	previewQualityCombo_->addItem(QStringLiteral("Preview: Full"), int(PreviewQuality::Full));
	previewQualityCombo_->addItem(QStringLiteral("Preview: ½"), int(PreviewQuality::Half));
	previewQualityCombo_->addItem(QStringLiteral("Preview: ¼"), int(PreviewQuality::Quarter));
	previewQualityCombo_->setToolTip(QStringLiteral(
		"How large a frame the preview renders. Auto matches the preview area, so it looks "
		"identical and skips work you cannot see. Lower settings scrub faster on heavy "
		"timelines. The export is never affected."));
	{
		QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		const int q = st.value(QStringLiteral("editor/previewQuality"), 0).toInt();
		previewQuality_ = PreviewQuality(std::clamp(q, 0, 3));
		QSignalBlocker b(previewQualityCombo_);
		previewQualityCombo_->setCurrentIndex(int(previewQuality_));
	}
	connect(previewQualityCombo_, &QComboBox::currentIndexChanged, this, [this](int i) {
		previewQuality_ = PreviewQuality(std::clamp(i, 0, 3));
		QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		st.setValue(QStringLiteral("editor/previewQuality"), i);
		refreshPreviewAtPlayhead();
	});
	bar->addWidget(previewQualityCombo_, Zone::Right);

	// Where the preview sits. A vertical video in a wide preview is a thin
	// strip of picture between two black bars, so one layout gives it a tall
	// column of its own. Remembered per mode (setEditMode applies it).
	layoutCombo_ = new QComboBox(this);
	layoutCombo_->setObjectName(QStringLiteral("editorLayout"));
	layoutCombo_->addItem(QStringLiteral("Layout: Standard"), int(EditorLayout::Standard));
	layoutCombo_->addItem(QStringLiteral("Layout: Vertical"), int(EditorLayout::Vertical));
	layoutCombo_->setToolTip(QStringLiteral(
		"Standard: the preview above the timeline. Vertical: the timeline, the Inspector and a "
		"tall preview side by side, for vertical videos. For every mode."));
	connect(layoutCombo_, &QComboBox::activated, this, [this](int i) {
		const auto l = EditorLayout(layoutCombo_->itemData(i).toInt());
		QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"))
			.setValue(QStringLiteral("editor/layout"), int(l));
		setEditorLayout(l);
	});
	bar->addWidget(layoutCombo_, Zone::Right);

	// Monitor toggle. Preview audio is Full-editing only for now, so it hides
	// with the other Full-only controls.
	audioPreview_ = new AudioPreview(this);
	// Debounced: a dragged slider or a run of edits mixes once, after it stops.
	audioPremixTimer_ = new QTimer(this);
	audioPremixTimer_->setSingleShot(true);
	audioPremixTimer_->setInterval(300);
	connect(audioPremixTimer_, &QTimer::timeout, this, &VideoEditorWindow::premixAudio);
	muteBtn_ = new QPushButton(this);
	muteBtn_->setIcon(uiIcon(Glyph::Speaker));
	muteBtn_->setCheckable(true);
	muteBtn_->setFixedWidth(34);
	const bool canHear = AudioPreview::available();
	muteBtn_->setEnabled(canHear);
	muteBtn_->setToolTip(canHear ? QStringLiteral("Mute the preview (the export is unaffected)")
				     : QStringLiteral("No audio output device was found"));
	connect(muteBtn_, &QPushButton::toggled, this, [this](bool off) {
		muteBtn_->setIcon(uiIcon(off ? Glyph::SpeakerMuted : Glyph::Speaker));
		audioPreview_->setMuted(off);
		if (!off && playing_ && fullEdit())
			startPreviewAudio(timelinePlayheadMs()); // catch up to the picture
		else if (!off)
			schedulePremix(); // muted, nothing was mixing: be ready for Play
	});
	bar->addWidget(muteBtn_, Zone::Right);
	// Voiceover: one button, in every mode, opening the voiceover window.
	// It turns red while a take is recording, so a running mic is never
	// hidden behind a closed window.
	voOpenBtn_ = new QPushButton(QStringLiteral("Voiceover"), this);
	voOpenBtn_->setIcon(uiIcon(Glyph::Record, 12, QColor(0xe5, 0x48, 0x4d)));
	voOpenBtn_->setToolTip(QStringLiteral("Record narration over the video: microphone, countdown, play-along"));
	bar->addWidget(voOpenBtn_, Zone::Right);

	// Speed: label · slider (stretches) · editable value · per-cut count.
	// Full editing has per-clip speed in the Inspector instead, so the whole group
	// hides there rather than sitting greyed out taking up the toolbar.
	speedCaption_ = new QLabel(QStringLiteral("Speed"), this);
	auto *speedBox = new QHBoxLayout;
	speedBox->setSpacing(4);
	speedBox->addWidget(speedCaption_);
	speedSlider_ = new QSlider(Qt::Horizontal, this);
	speedSlider_->setRange(0, kSpeedTicks); // exponential 0.1×..50×
	speedSlider_->setPageStep(kSpeedTicks / 20);
	speedSlider_->setValue(speedToSlider(1.0));
	speedSlider_->setMinimumWidth(96); // see EditorChromeParams::speedSliderMinW
	speedSlider_->setToolTip(QStringLiteral("Playback speed (0.1×–50×); scaled so low speeds are easy to fine-tune"));
	speedBox->addWidget(speedSlider_); // compact, fixed width (see applyChrome)
	speedSpin_ = new QDoubleSpinBox(this);
	speedSpin_->setRange(kMinSpeed, kMaxSpeed);
	speedSpin_->setDecimals(2);
	speedSpin_->setSingleStep(0.1);
	speedSpin_->setSuffix(QStringLiteral("×"));
	speedSpin_->setValue(1.0);
	speedSpin_->setKeyboardTracking(false); // apply on Enter/focus-out, not each digit
	speedSpin_->setFixedWidth(72);
	speedBox->addWidget(speedSpin_);
	speedLabel_ = new QLabel(QString(), this); // "(N cuts)" / "—" status
	// No reserved width: it is empty in Simple Trim and hidden in Full editing,
	// and 56px of nothing was pushing the toolbar's minimum width up.
	speedLabel_->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	speedBox->addWidget(speedLabel_);
	bar->addWidget(cluster(speedBox), Zone::Center);

	// Sources toggle — show/hide the floating Sources panel.
	sourcesBtn_ = new QPushButton(QStringLiteral("Sources"), this);
	sourcesBtn_->setCheckable(true);
	sourcesBtn_->setChecked(false); // closed until the user opens it
	sourcesBtn_->setToolTip(QStringLiteral("Show/hide the floating Sources panel"));
	connect(sourcesBtn_, &QPushButton::toggled, this, [this](bool on) {
		if (!sourcesPanel_)
			return;
		QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		if (on) {
			showSourcesPanel();
		} else {
			st.setValue(QStringLiteral("editor/sourcesGeom"), sourcesPanel_->saveGeometry());
			sourcesPanel_->hide();
		}
		st.setValue(QStringLiteral("editor/sourcesShown"), on);
	});
	sourcesBtn_->setVisible(false); // View > Sources; the button holds the state

	// Effects toggle — opens the right panel, where a clip's components live.
	effectsBtn_ = new QPushButton(QStringLiteral("Effects"), this);
	effectsBtn_->setCheckable(true);
	effectsBtn_->setChecked(false);
	effectsBtn_->setToolTip(QStringLiteral(
		"Show/hide the Inspector — select a clip and add a Shader, Script or Effect "
		"component to it"));
	connect(effectsBtn_, &QPushButton::toggled, this, [this](bool on) {
		showInspector(on);
		if (inspectorBtn_) {
			QSignalBlocker b(inspectorBtn_);
			inspectorBtn_->setChecked(on);
		}
	});
	effectsBtn_->setVisible(false); // View > Effects

	// Inspector toggle — show/hide the right-side properties panel.
	inspectorBtn_ = new QPushButton(QStringLiteral("Inspector"), this);
	inspectorBtn_->setCheckable(true);
	inspectorBtn_->setChecked(false); // collapsed until the user opens it
	inspectorBtn_->setToolTip(QStringLiteral(
		"Show/hide the properties panel for the selected cut (uses the space beside portrait previews)"));
	connect(inspectorBtn_, &QPushButton::toggled, this, [this](bool on) {
		showInspector(on);
		if (effectsBtn_) {
			QSignalBlocker b(effectsBtn_);
			effectsBtn_->setChecked(on);
		}
	});
	inspectorBtn_->setVisible(false); // View > Inspector
	// Developer Panel: live-tweak every timeline layout variable to find the
	// best UI configuration (editor-only tool, values are not persisted).
	auto *devBtn = new QPushButton(QStringLiteral("Dev"), this);
	devBtn_ = devBtn;
	devBtn->setFlat(true);
	devBtn->setToolTip(QStringLiteral(
		"Developer Panel — tweak timeline spacing, thumbnail size, padding and zoom live"));
	connect(devBtn, &QPushButton::clicked, this, [this]() {
		if (!devPanel_) {
			devPanel_ = new DevPanel(timeline_, tracks_, voTrack_, canvas_,
						 timelineView_, this);
			connect(devPanel_, &DevPanel::inspectorChanged, this,
				&VideoEditorWindow::applyInspectorParams);
			connect(devPanel_, &DevPanel::chromeChanged, this,
				&VideoEditorWindow::applyChrome);
			connect(devPanel_, &DevPanel::panelChanged, this,
				&VideoEditorWindow::applyPanelParams);
			connect(devPanel_, &DevPanel::keyframeChanged, this,
				[this](const KeyframeLayoutParams &p) {
					keyframeLayout_ = p;
					if (keyEditor_)
						keyEditor_->setLayoutParams(p);
				});
		}
		devPanel_->show();
		devPanel_->raise();
		devPanel_->activateWindow();
	});
	devBtn->setVisible(false); // Tools > Developer panel
	topLayout->addWidget(barHost);
	splitter->addWidget(topPane);

	// ---- Bottom pane: timelines, voiceover and file controls ----
	auto *bottomPane = new QWidget(this);
	bottomPane_ = bottomPane;
	auto *bottomLayout = new QVBoxLayout(bottomPane);
	bottomLayout->setContentsMargins(0, 0, 0, 0);
	bottomLayout_ = bottomLayout;

	timeline_ = new Timeline(this);
	tracks_ = new TrackEditor(this);
	timelineView_ = new TimelineView(this);
	multiCutSourceRow_ = new QWidget(bottomPane);
	multiCutSourceRow_->setVisible(false);
	auto *sourcePickerLayout = new QHBoxLayout(multiCutSourceRow_);
	sourcePickerLayout->setContentsMargins(0, 0, 0, 0);
	auto *sourcePickerLabel = new QLabel(QStringLiteral("Source video"), multiCutSourceRow_);
	sourcePickerLayout->addWidget(sourcePickerLabel);
	multiCutSourceCombo_ = new QComboBox(multiCutSourceRow_);
	multiCutSourceCombo_->setObjectName(QStringLiteral("multiCutSourceSelector"));
	multiCutSourceCombo_->setPlaceholderText(QStringLiteral("Choose a video"));
	multiCutSourceCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	multiCutSourceCombo_->setMinimumContentsLength(24);
	multiCutSourceCombo_->setToolTip(QStringLiteral(
		"Choose the video shown in the Source track. New cuts use this video; "
		"cuts already in the Output track stay in place."));
	sourcePickerLabel->setBuddy(multiCutSourceCombo_);
	sourcePickerLayout->addWidget(multiCutSourceCombo_, 1);
	connect(multiCutSourceCombo_, &QComboBox::activated, this, [this](int index) {
		if (!multiCut() || index < 0)
			return;
		const int id = multiCutSourceCombo_->itemData(index).toInt();
		if (id != activeSourceId_)
			setActiveSource(id);
	});
	auto *addCutSource = new QPushButton(QStringLiteral("Add video…"), multiCutSourceRow_);
	addCutSource->setObjectName(QStringLiteral("addMultiCutVideoButton"));
	addCutSource->setToolTip(QStringLiteral("Add another video to cut from, keeping your existing cuts."));
	connect(addCutSource, &QPushButton::clicked, this, &VideoEditorWindow::onAddSource);
	sourcePickerLayout->addWidget(addCutSource);
	bottomLayout->addWidget(multiCutSourceRow_);
	stack_ = new QStackedWidget(this);
	stack_->addWidget(timeline_);      // index 0 = Simple Trim
	stack_->addWidget(tracks_);        // index 1 = Multi-Cut
	stack_->addWidget(timelineView_);  // index 2 = Full Editing

	// Long projects: zooming in is the only way to work precisely and also how
	// you lose track of where you are. Both of these report their own view, and
	// the strip over the preview shows it until you stop moving.
	connect(timelineView_, &TimelineView::viewChanged, this,
		&VideoEditorWindow::onTimelineViewChanged);
	connect(tracks_, &TrackEditor::viewChanged, this, &VideoEditorWindow::onTimelineViewChanged);
	// Dragging the red box moves the timeline it stands for. The overview does
	// not know which widget that is, and does not need to: it reports a new
	// start and the window routes it to whatever is on screen.
	connect(overview_, &TimelineOverview::viewStartRequested, this, [this](qint64 startMs) {
		if (fullEdit())
			timelineView_->setViewStart(startMs);
		else if (multiCut())
			tracks_->setViewStart(startMs);
	});
	bottomLayout->addWidget(stack_);

	connect(timelineView_, &TimelineView::scrub, this, &VideoEditorWindow::onTimelineScrub);
	connect(timelineView_, &TimelineView::hoverScrub, this, &VideoEditorWindow::onTimelineHoverScrub);
	connect(timelineView_, &TimelineView::hoverScrubEnded, this,
		&VideoEditorWindow::showPlayheadFrame);
	connect(timelineView_, &TimelineView::clipsChanged, this, [this]() {
		bool stoppedAtEnd = false;
		if (fullEdit() && playing_ && timelineView_->model().hasVideoEnd()) {
			qint64 playingAt = playAnchorMs_ + playClock_.elapsed();
			if (audioPreview_ && audioPreview_->isPlaying()) {
				const qint64 audioAt = audioPreview_->positionMs();
				if (audioAt >= 0)
					playingAt = audioAt;
			}
			const qint64 endMs = timelineView_->durationMs();
			if (playingAt >= endMs || timelinePlayheadMs() >= endMs) {
				stopAtTimelineEnd(endMs);
				stoppedAtEnd = true;
			}
		}
		// Sounds for events first: the rules' clips are derived from the
		// timeline, so they are rebuilt before anything below reads it (the
		// mix, the preview, the undo snapshot).
		applySoundRules();
		// A fade drag rounds to the project frame, which only exists once a
		// source is loaded — refresh it whenever the timeline changes.
		timelineView_->setFrameRate(timelineFps());
		updateInfoLabel();
		updateVoiceoverAxis();
		syncPreviewTransformTarget();
		scriptScanDirty_ = true;
		// Chapter markers and their labels do not change the sound. Keep a
		// matching mix playing while those annotations are edited.
		if (!audioMixValid_ || audioMixKey() != audioMixKeyBuilt_)
			invalidateAudioMix();
		// Moving or trimming a clip on the timeline changes where the playhead
		// falls inside it, so the pose and keyframe readout the Inspector shows
		// have to follow along live.
		// A clip can arrive without a source -- a caption, a still, an effect --
		// so whether the project is empty is a question about the TIMELINE too,
		// and it is only ever answered here for those.
		updateEmptyState();
		// Only when it can be seen: this handler runs on every mouse move of a
		// drag, and the Clip tab re-syncs itself when it is shown again.
		if (insTabShowing(kInsTabClip))
			syncClipInspector();
		else
			clipTabDirty_ = true;
		// The composed picture depends on the clips, so a change to them has to
		// re-render it. Dragging and trimming got away without this because the
		// mouse is also scrubbing, which refreshes the preview as a side effect
		// — but DELETING does not scrub, so removing a clip (an effect clip
		// most visibly, since it grades everything under it) left the old frame
		// on screen until something else happened to repaint it.
		//
		// Through requestPreview rather than rendering here: this signal fires
		// on every mouse-move of a drag, and requestPreview renders the first
		// one at once, paces the rest, and keeps only the newest.
		if (fullEdit())
			requestPreview(-1, stoppedAtEnd ? std::max<qint64>(0, timelinePlayheadMs() - 1)
						      : timelinePlayheadMs());
		scheduleSnapshot();
		rebuildSoundsTab();
		rebuildTagsSection();
	});
	// A finished timeline action closes its undo entry at once. Without this,
	// two edits inside the 350ms coalescing window share one entry -- and a move
	// followed by a move back collapses to no change at all, which is what made
	// Ctrl+Z look broken.
	connect(timelineView_, &TimelineView::editCommitted, this,
		&VideoEditorWindow::commitSnapshot);
	connect(timelineView_, &TimelineView::selectionChanged, this, [this](int track, int clip) {
		// A change of selection is an undo step. Not while a selection box is
		// still being drawn -- only where it settles, which the release says
		// again -- and not while undo itself is putting one back.
		if (!restoring_ && !timelineView_->selectingWithBox())
			commitSnapshot();
		pathKeyPicked_ = -1; // a new selection: Delete means the selection again
		// The preview shows a new selection's own words, not a variation
		// picked on the clip before it.
		if (previewVariant_ != 0) {
			previewVariant_ = 0;
			requestPreview(-1, timelinePlayheadMs());
		}
		syncPreviewTransformTarget();
		updateInspector();
		refreshKeyframeEditor(); // it follows the selection
		// With the console open, a click on a clip tells you what to call it:
		// the clip becomes `clip`, and its V1[3] name is how a template would
		// reach it without a mouse.
		if (consolePanel_ && consolePanel_->isVisible() && track >= 0 && clip >= 0) {
			const TimelineModel &m = timelineView_->model();
			QString name;
			if (track < m.tracks.size() && clip < m.tracks[track].clips.size()) {
				const TlClip &c = m.tracks[track].clips[clip];
				for (const EditorSource &s : sources_)
					if (s.id == c.sourceId)
						name = s.name;
				if (c.type == TlClip::Type::Text)
					name = rich_text::plainText(c.text.text).left(40);
			}
			consolePrint(QStringLiteral("// clip -> %1  %2   (also tracks.%3)")
					     .arg(EditConsole::refName(m, track, clip), name,
						  EditConsole::refName(m, track, clip)),
				     ConsoleTone::Comment);
		}
		// Clicking a clip is a statement that you want to work on it, so the
		// panel follows. Only on SELECT: switching back on a deselect would
		// yank the tab away every time you clicked empty timeline, and going
		// back to Project is one click that then stays put.
		if (insTabs_ && track >= 0 && clip >= 0)
			insTabs_->setCurrentIndex(kInsTabClip);
		// So does selecting a caption lane: its Clip tab styles every caption.
		else if (insTabs_ && track < 0 && fullEdit() && captionLaneForBatch() >= 0)
			insTabs_->setCurrentIndex(kInsTabClip);
	});
	// "Show in inspector" from a clip's right-click menu (any mode).
	connect(timelineView_, &TimelineView::filesDropped, this,
		&VideoEditorWindow::onFilesDroppedOnTimeline);
	connect(timelineView_, &TimelineView::randomizeSelectionRequested, this,
		[this]() { randomizeClips(/*selectedOnly=*/true); });
	// Sounds for events, from the clip and lane menus.
	connect(timelineView_, &TimelineView::soundForClipRequested, this,
		&VideoEditorWindow::onSoundForClip);
	connect(timelineView_, &TimelineView::soundForTransitionRequested, this,
		&VideoEditorWindow::onSoundForTransition);
	connect(timelineView_, &TimelineView::soundRulesRequested, this, &VideoEditorWindow::showSoundsTab);
	connect(timelineView_, &TimelineView::subtitlesRequested, this, &VideoEditorWindow::openSubtitles);
	connect(timelineView_, &TimelineView::beatsRequested, this, &VideoEditorWindow::openBeatDetection);
	connect(timelineView_, &TimelineView::respeakRequested, this, &VideoEditorWindow::respeakSentence);
	connect(timelineView_, &TimelineView::slipRequested, this, &VideoEditorWindow::openSlip);
	// Fit to selection (Text to speech) follows what is selected.
	connect(timelineView_, &TimelineView::selectionChanged, this, [this]() { updateTtsFitTarget(); });
	connect(timelineView_, &TimelineView::freezeSoundsRequested, this, &VideoEditorWindow::freezeSounds);
	connect(timelineView_, &TimelineView::inspectClipRequested, this,
		&VideoEditorWindow::revealInspector);
	connect(timelineView_, &TimelineView::keyframeEditorRequested, this,
		&VideoEditorWindow::openKeyframeEditor);
	// "Export this clip…" — the ordinary Export window, aimed at one stretch.
	connect(timelineView_, &TimelineView::exportRangeRequested, this,
		&VideoEditorWindow::onExportRangeRequested);
	// Right-click the picture: "Zoom here". Full-editing only, since it writes
	// keyframes onto a timeline clip and the other two modes have none.
	connect(canvas_, &PreviewCanvas::contextRequested, this,
		[this](double xn, double yn, const QPoint &globalPos) {
			if (!fullEdit())
				return;
			QMenu menu(this);
			QAction *zoom = menu.addAction(QStringLiteral("Zoom here"));
			const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
			zoom->setToolTip(QStringLiteral(
				"Push in to this point at the playhead and back out again. It is written "
				"as ordinary keyframes, so you can drag, retime or delete it afterwards."));
			const QString needClip = QStringLiteral(
				"Select a clip first: click one on the timeline (or on the picture), then "
				"right-click here.");
			disableBecause(zoom, sel != nullptr, needClip);
			// Aligning the selected clip's rect to the canvas. The pose is
			// written the way a drag writes it (a key at the playhead on a
			// keyed clip, the base pose otherwise), so it undoes as one step.
			menu.addSeparator();
			QMenu *align = menu.addMenu(QStringLiteral("Align"));
			struct Entry {
				int how;
				const char *text;
			};
			static const Entry entries[] = {
				{0, "Centre on canvas"},
				{1, "Centre horizontally"},
				{2, "Centre vertically"},
				{-1, nullptr},
				{3, "Align left edge"},
				{4, "Align right edge"},
				{5, "Align top edge"},
				{6, "Align bottom edge"},
				{-1, nullptr},
				{7, "Fit inside canvas"},
				{8, "Fill canvas"},
				{-1, nullptr},
				{9, "Reset position"},
				{10, "Reset scale"},
				{11, "Reset rotation"},
				{12, "Reset all"},
			};
			QHash<QAction *, int> alignHow;
			for (const Entry &en : entries) {
				if (!en.text) {
					align->addSeparator();
					continue;
				}
				QAction *a = align->addAction(QString::fromUtf8(en.text));
				disableBecause(a, sel != nullptr, needClip);
				alignHow.insert(a, en.how);
			}
			explainDisabled(&menu);
			explainDisabled(align);
			QAction *chosen = menu.exec(globalPos);
			if (chosen == zoom && sel)
				addZoomAtPreviewPoint(xn, yn);
			else if (chosen && alignHow.contains(chosen) && sel)
				alignSelectedClip(alignHow.value(chosen));
		});

	// Direct manipulation of the selected clip straight in the preview.
	connect(canvas_, &PreviewCanvas::transformDragged, this,
		&VideoEditorWindow::onPreviewTransformDrag);
	connect(canvas_, &PreviewCanvas::transformZoomed, this,
		&VideoEditorWindow::onPreviewTransformZoom);
	// The grips. Scaling is relative to the pose at the START of the gesture --
	// the canvas reports how far the grip has been dragged from where it was
	// pressed, not since the last mouse-move -- so the factor is applied to a
	// remembered pose rather than compounding into a runaway.
	connect(canvas_, &PreviewCanvas::transformScaled, this,
		[this](double factor, double, double) {
			const TlClip *sel =
				timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
			if (!sel)
				return;
			if (playing_) // see onPreviewTransformDrag
				stopPlayback();
			if (!xfGestureActive_) {
				xfGestureActive_ = true;
				xfGestureBase_ = sel->transformAt(timelinePlayheadMs());
			}
			TlTransform tf = xfGestureBase_;
			tf.scale = std::clamp(xfGestureBase_.scale * factor, 0.05, 20.0);
			applySelectedClipTransform(tf);
		});
	connect(canvas_, &PreviewCanvas::transformRotated, this, [this](double deg) {
		const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
		if (!sel)
			return;
		if (playing_)
			stopPlayback();
		if (!xfGestureActive_) {
			xfGestureActive_ = true;
			xfGestureBase_ = sel->transformAt(timelinePlayheadMs());
		}
		TlTransform tf = xfGestureBase_;
		tf.rotation = deg;
		applySelectedClipTransform(tf);
	});
	// One undo entry per gesture, like the spotlight masks: the drag itself only
	// wrote the model.
	// Dragging a mask's grips writes the component's own properties, through the
	// same path the Inspector's sliders use -- so it keyframes, multi-clip edits
	// and undoes identically, and the panel follows the drag live.
	connect(canvas_, &PreviewCanvas::maskPoseChanged, this,
		[this](double cx, double cy, double w, double h, double rot) {
			if (!timelineView_ || !timelineView_->selectedClipPtr())
				return;
			if (playing_) // see onPreviewTransformDrag: reframe against a still playhead
				stopPlayback();
			editSharedComponent(QStringLiteral("harpia.mask"), 0,
					    [&](ComponentInstance &ci) {
						    ci.props[QStringLiteral("centreX")] = cx;
						    ci.props[QStringLiteral("centreY")] = cy;
						    ci.props[QStringLiteral("width")] = w;
						    ci.props[QStringLiteral("height")] = h;
						    ci.props[QStringLiteral("rotation")] = rot;
					    });
			afterComponentEdit();
		});
	// One undo entry per gesture, like the clip grips and the spotlight masks.
	connect(canvas_, &PreviewCanvas::maskEditFinished, this, [this]() { commitSnapshot(); });
	// The motion path: pick a key (the playhead goes there), drag it, add one
	// where the path runs, or right-click it. See MotionPath.hpp.
	connect(canvas_, &PreviewCanvas::pathKeyPressed, this, [this](int keyId) {
		const TlClip *c = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
		if (!c || keyId < 0 || keyId >= c->keys.size())
			return;
		if (playing_)
			stopPlayback();
		pathKeyPicked_ = keyId;
		onTimelineScrub(c->outStartMs + c->keys[keyId].tMs);
		syncClipInspector();
		refreshKeyframeEditor();
	});
	connect(canvas_, &PreviewCanvas::pathKeyDragged, this, [this](int keyId, double nx, double ny) {
		if (playing_)
			stopPlayback();
		pathKeyPicked_ = keyId;
		editSelectedClip([keyId, nx, ny](TlClip &c) {
			if (keyId < 0 || keyId >= c.keys.size())
				return;
			c.keys[keyId].tf.posX = nx;
			c.keys[keyId].tf.posY = ny;
		});
	});
	// A curve handle dragged on the preview. The first drag turns the key's
	// automatic handles into its own (starting from where the automatic ones
	// were, so nothing jumps); they stay mirrored -- a smooth corner -- unless
	// Alt is held, which breaks them for good, as in any drawing program.
	connect(canvas_, &PreviewCanvas::pathHandleDragged, this,
		[this](int keyId, bool outHandle, double nx, double ny, bool breakHandles) {
			if (playing_)
				stopPlayback();
			pathKeyPicked_ = keyId;
			editSelectedClip([=](TlClip &c) {
				if (keyId < 0 || keyId >= c.keys.size())
					return;
				QPointF in, out;
				c.pathHandles(keyId, in, out);
				TlKeyframe &k = c.keys[keyId];
				if (!k.handlesManual) {
					k.handlesManual = true;
					k.inX = in.x();
					k.inY = in.y();
					k.outX = out.x();
					k.outY = out.y();
				}
				k.handlesBroken = k.handlesBroken || breakHandles;
				const QPointF off = QPointF(nx, ny) - QPointF(k.tf.posX, k.tf.posY);
				if (outHandle) {
					k.outX = off.x();
					k.outY = off.y();
				} else {
					k.inX = off.x();
					k.inY = off.y();
				}
				if (!k.handlesBroken) {
					(outHandle ? k.inX : k.outX) = -off.x();
					(outHandle ? k.inY : k.outY) = -off.y();
				}
			});
		});
	connect(canvas_, &PreviewCanvas::pathKeyDragFinished, this, [this]() {
		commitSnapshot();
		refreshKeyframeEditor();
	});
	connect(canvas_, &PreviewCanvas::pathAddRequested, this, [this](double nx, double ny) {
		const TlClip *c = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
		if (!c)
			return;
		const qint64 t = motionPathTimeAt(*c, QPointF(nx, ny), canvas_->displaySize(), 12.0);
		if (t < 0)
			return;
		if (playing_)
			stopPlayback();
		const qint64 at = c->outStartMs + t;
		// Position only, at the pose the clip already has there: the path
		// does not move, it just gains a point you can now drag.
		editSelectedClip([at](TlClip &cl) {
			cl.setKeyframeAt(at, cl.transformAt(at), TlEase::EaseInOut, 1 << TlLanePos);
		});
		commitSnapshot();
		if (const TlClip *now = timelineView_->selectedClipPtr())
			pathKeyPicked_ = now->keyframeIndexAt(at, 1);
		onTimelineScrub(at);
		syncClipInspector();
		refreshKeyframeEditor();
	});
	connect(canvas_, &PreviewCanvas::pathKeyMenuRequested, this, &VideoEditorWindow::motionPathKeyMenu);
	connect(canvas_, &PreviewCanvas::transformEditFinished, this, [this]() {
		xfGestureActive_ = false;
		// The guides say "the drag you are doing is snapped"; with the drag over
		// they would just be lines nobody asked for.
		canvas_->setCentreGuides(false, false);
		commitSnapshot();
	});
	// Spotlight masks, placed by looking at the picture rather than by typing
	// four numbers into the panel.
	connect(canvas_, &PreviewCanvas::spotlightPoseChanged, this,
		&VideoEditorWindow::onSpotlightPoseDragged);
	connect(canvas_, &PreviewCanvas::spotlightSelected, this, [this](int i) {
		if (spotList_ && i >= 0 && i < spotList_->count())
			spotList_->setCurrentRow(i);
	});
	// One undo entry per gesture: the drag itself only updated the model.
	connect(canvas_, &PreviewCanvas::spotlightEditFinished, this,
		[this]() { commitSnapshot(); });

	// ---- Voiceover: a collapsible narration section (record over the video).
	// Collapsed by default so detailed cut work keeps the vertical space; the
	// expand/collapse state is remembered across launches.
	auto *audioHeader = new QPushButton(this);
	audioHeader_ = audioHeader;
	audioHeader->setFlat(true);
	audioHeader->setCursor(Qt::PointingHandCursor);
	audioHeader->setStyleSheet(QStringLiteral(
		"QPushButton{text-align:left; padding:2px; color:#c8ccd4; font-weight:bold; border:none;}"
		"QPushButton:hover{color:#e8eaed;}"));
	bottomLayout->addWidget(audioHeader);

	auto *audioBody = new QWidget(this);
	audioBody_ = audioBody;
	auto *audioLayout = new QVBoxLayout(audioBody);
	audioLayout->setContentsMargins(0, 0, 0, 0);

	voTrack_ = new VoiceoverTrack(this);
	audioLayout->addWidget(voTrack_);

	// Recording itself lives in the voiceover panel, the same one Full editing
	// opens, so the two modes record the same way with the same settings.
	auto *voRow = new QHBoxLayout;
	voImportBtn_ = new QPushButton(QStringLiteral("Import audio…"), this);
	voImportBtn_->setToolTip(QStringLiteral("Add an existing audio file (mp3/wav/m4a/…) to the Voiceover track"));
	voRow->addWidget(voImportBtn_);
	voRow->addStretch(1);
	audioLayout->addLayout(voRow);
	buildVoiceoverPanel();

	// Mixing controls (applied at export): original-audio level + auto-duck.
	auto *voMixRow = new QHBoxLayout;
	voMixRow->addWidget(new QLabel(QStringLiteral("Original audio"), this));
	voOrigVol_ = new QSlider(Qt::Horizontal, this);
	voOrigVol_->setRange(0, 150); // 0..150% of the source audio
	voOrigVol_->setValue(100);
	voOrigVol_->setMaximumWidth(200);
	voOrigVol_->setToolTip(QStringLiteral("Volume of the video's own audio in the export (0 = mute)"));
	voMixRow->addWidget(voOrigVol_);
	voOrigVolLabel_ = new QLabel(QStringLiteral("100%"), this);
	voOrigVolLabel_->setMinimumWidth(40);
	voMixRow->addWidget(voOrigVolLabel_);
	connect(voOrigVol_, &QSlider::valueChanged, this, [this](int v) {
		voOrigVolLabel_->setText(QStringLiteral("%1%").arg(v));
	});
	voDuck_ = new QCheckBox(QStringLiteral("Duck original under narration"), this);
	voDuck_->setToolTip(QStringLiteral("Automatically dip the video's audio while narration plays"));
	voMixRow->addWidget(voDuck_);
	voMixRow->addStretch(1);
	audioLayout->addLayout(voMixRow);

	bottomLayout->addWidget(audioBody);
	// Nothing until the Vertical layout's tall column needs it (adaptEditingArea).
	bottomFill_ = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Fixed);
	bottomLayout->addSpacerItem(bottomFill_);

	// Wire the disclosure header: toggle the body, swap the arrow, persist state.
	{
		QSettings s(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		const bool expanded = s.value(QStringLiteral("editor/audioExpanded"), false).toBool();
		auto apply = [audioHeader, audioBody](bool on) {
			audioBody->setVisible(on);
			audioHeader->setText(
				QStringLiteral("Audio — voiceover track and mix"));
			audioHeader->setIcon(
				uiIcon(on ? Glyph::ChevronDown : Glyph::ChevronRight, 12));
		};
		apply(expanded);
		connect(audioHeader, &QPushButton::clicked, this, [audioBody, apply]() {
			const bool on = !audioBody->isVisible();
			apply(on);
			QSettings s2(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
			s2.setValue(QStringLiteral("editor/audioExpanded"), on);
		});
	}

	voRecorder_ = new AudioRecorder(this);
	connect(voRecorder_, &AudioRecorder::level, this, [this](qreal rms, qreal peak) {
		voMeter_->setLevel(rms, peak);
		if (!voRecording_)
			return;
		voStatus_->setText(QStringLiteral("\u25CF Recording  %1").arg(timeTextTenths(voRecorder_->capturedMs())));
		// One waveform bucket per kLiveBucketMs of audio, holding the loudest
		// moment in it -- the same measure the finished take's waveform uses.
		voLiveMax_ = std::max(voLiveMax_, float(peak));
		const qint64 cap = voRecorder_->capturedMs();
		bool closed = false;
		while (cap >= voLiveBucketEnd_ + kLiveBucketMs) {
			voLivePeaks_.append(voLiveMax_);
			voLiveBucketEnd_ += kLiveBucketMs;
			closed = true;
		}
		// A finished bucket: the next one starts from what this chunk
		// carried past the boundary, not from silence.
		if (closed)
			voLiveMax_ = cap > voLiveBucketEnd_ ? float(peak) : 0.f;
		if (!voLiveTick_.isValid() || voLiveTick_.elapsed() >= 90) {
			voLiveTick_.restart();
			updateLiveTake();
		}
	});
	connect(voRecorder_, &AudioRecorder::error, this, [this](const QString &msg) {
		warnAndLog(this, voRecorder_->isSystemSource() ? QStringLiteral("Windows audio")
							       : QStringLiteral("Microphone"),
			   msg);
	});
	voCountdownTimer_ = new QTimer(this);
	voCountdownTimer_->setInterval(1000);
	connect(voCountdownTimer_, &QTimer::timeout, this, [this]() {
		if (--voCountdownLeft_ <= 0) {
			voCountdownTimer_->stop();
			startVoiceoverCapture();
		} else {
			voStatus_->setText(QStringLiteral("Starting in %1…").arg(voCountdownLeft_));
		}
	});
	connect(voRecordBtn_, &QPushButton::clicked, this,
		&VideoEditorWindow::onVoiceoverRecordClicked);
	connect(voOpenBtn_, &QPushButton::clicked, this, &VideoEditorWindow::openVoiceoverPanel);
	connect(voImportBtn_, &QPushButton::clicked, this, &VideoEditorWindow::onImportAudioClicked);
	connect(voTrack_, &VoiceoverTrack::clipsChanged, this, [this]() {
		updateInfoLabel();
		scheduleSnapshot();
	});

	// The rest of the toolbar (see ToolbarLayout above): editing tools on the
	// left, the size/length readout on the right.
	cropToggle_ = new QCheckBox(QStringLiteral("Crop"), this);
	cropToggle_->setToolTip(QStringLiteral("Drag the rectangle to crop the image (great for smaller GIFs)"));
	auto *resetCrop = new QPushButton(QStringLiteral("Reset crop"), this);
	resetCropBtn_ = resetCrop;
	// Everything that puts something NEW on the timeline, in one menu. Six
	// buttons in a row read as six unrelated things; one "Add" reads as the
	// place to go when you want more on the timeline, and leaves the row room
	// for the controls you use while editing what is already there.
	addMenuBtn_ = new QPushButton(QStringLiteral("Add  \u25BE"), this);
	addMenuBtn_->setToolTip(QStringLiteral("Put something new on the timeline: a library recording, text, subtitles, "
						"audio, an image, an effect clip"));
	addMenuBtn_->setVisible(false); // Full editing only
	auto *addMenu = new QMenu(addMenuBtn_);
	addMenu->setToolTipsVisible(true);
	addLibraryAct_ = addMenu->addAction(QStringLiteral("Add from Library…"));
	addLibraryAct_->setObjectName(QStringLiteral("addFromLibraryAction"));
	addLibraryAct_->setVisible(false); // Full editing only, including the shared menubar entry
	addLibraryAct_->setToolTip(QStringLiteral(
		"Open your recorded videos. Drag a recording onto a timeline lane, or double-click "
		"to add it at the playhead in this project."));
	connect(addLibraryAct_, &QAction::triggered, this, &VideoEditorWindow::openRecordingLibrary);
	addMenu->addSeparator();
	addTextAct_ = addMenu->addAction(QStringLiteral("Text"));
	addTextAct_->setToolTip(
		QStringLiteral("Add a text clip at the playhead (font, colour, outline and background "
			       "box are set in the Inspector)"));
	connect(addTextAct_, &QAction::triggered, this, &VideoEditorWindow::addTextClip);
	addRandomTextAct_ = addMenu->addAction(QStringLiteral("Random text"));
	addRandomTextAct_->setToolTip(QStringLiteral(
		"Add a text with several texts to choose from: the video is exported once per text "
		"(Export → Variations), the footage the same in every one"));
	connect(addRandomTextAct_, &QAction::triggered, this, &VideoEditorWindow::addRandomTextClip);
	subtitleAct_ = addMenu->addAction(QStringLiteral("Subtitles from speech\u2026"));
	subtitleAct_->setToolTip(QStringLiteral(
		"Transcribe the selected clips' speech (OpenAI) and put caption clips on a "
		"Subtitles lane, lined up with the audio. Edit the text in the Inspector afterwards."));
	connect(subtitleAct_, &QAction::triggered, this, &VideoEditorWindow::openSubtitles);
	ttsAct_ = addMenu->addAction(QStringLiteral("Text to speech…"));
	ttsAct_->setToolTip(QStringLiteral(
		"Type or open a text and a voice reads it: a narration and its captions at the "
		"playhead, linked so they move together (OpenAI, ElevenLabs, Google or Deepgram)."));
	connect(ttsAct_, &QAction::triggered, this, &VideoEditorWindow::openTextToSpeech);
	addMenu->addSeparator();
	addAudioAct_ = addMenu->addAction(QStringLiteral("Audio\u2026"));
	addAudioAct_->setToolTip(QStringLiteral(
		"Put audio on its own timeline track \u2014 a source's own audio, an imported file, "
		"or an empty track"));
	connect(addAudioAct_, &QAction::triggered, this, &VideoEditorWindow::onAddAudioClicked);
	addImageAct_ = addMenu->addAction(QStringLiteral("Image\u2026"));
	addImageAct_->setToolTip(
		QStringLiteral("Place a still image (logo, arrow, callout) on the timeline"));
	connect(addImageAct_, &QAction::triggered, this, &VideoEditorWindow::addImageClip);
	// Only when yt-dlp was found at startup: an entry that can only fail is
	// worse than none. Settings has a Downloads page to point at a copy.
	if (YtDlp::available()) {
		addUrlAct_ = addMenu->addAction(QStringLiteral("Video from URL\u2026"));
		addUrlAct_->setToolTip(QStringLiteral(
			"Paste a link, check what it is, pick a quality, and yt-dlp downloads it onto the "
			"timeline at the playhead."));
		connect(addUrlAct_, &QAction::triggered, this, &VideoEditorWindow::openUrlDownload);
	}
	// Ctrl+V does this too, but pasting a screenshot, a song or a link straight
	// onto the timeline is not something anyone tries unprompted -- so it gets
	// an entry, greyed out when there is nothing on the clipboard to place.
	pasteAct_ = addMenu->addAction(QStringLiteral("Paste"));
	pasteAct_->setToolTip(QStringLiteral(
		"Put what you copied on the timeline at the playhead \u2014 a picture or "
		"screenshot, a sound, files from Explorer, a link to a video, picture or "
		"sound, or text \u2014 on the first track free for it, or a new one on top"));
	connect(pasteAct_, &QAction::triggered, this, [this]() { pasteSystemClipboard(timelinePlayheadMs()); });
	connect(QApplication::clipboard(), &QClipboard::dataChanged, this, [this]() {
		if (pasteAct_)
			pasteAct_->setEnabled(systemClipboardHasPaste());
	});
	pasteAct_->setEnabled(systemClipboardHasPaste());
	addMenu->addSeparator();
	// Empty lanes, for laying out a project before there is anything to
	// put on it. A new picture lane goes at the top of the picture group, a
	// new audio lane at the top of the audio group, like the lane menu's
	// "Add track above" does.
	QAction *newVideoTrack = addMenu->addAction(QStringLiteral("New video track"));
	newVideoTrack->setToolTip(QStringLiteral("An empty picture lane at the top of the timeline."));
	connect(newVideoTrack, &QAction::triggered, this, [this]() {
		if (!fullEdit())
			setEditMode(EditMode::Full);
		if (timelineView_)
			timelineView_->addTrack(TlTrack::Kind::Video);
	});
	QAction *newAudioTrack = addMenu->addAction(QStringLiteral("New audio track"));
	newAudioTrack->setToolTip(QStringLiteral("An empty audio lane, above the other audio lanes."));
	connect(newAudioTrack, &QAction::triggered, this, [this]() {
		if (!fullEdit())
			setEditMode(EditMode::Full);
		if (timelineView_)
			timelineView_->addTrack(TlTrack::Kind::Audio);
	});
	QAction *recVo = addMenu->addAction(QStringLiteral("Record voiceover…"));
	recVo->setToolTip(QStringLiteral("Record narration from your microphone onto a free audio lane, "
					 "with a countdown and play-along."));
	connect(recVo, &QAction::triggered, this, [this]() {
		if (!fullEdit())
			setEditMode(EditMode::Full);
		openVoiceoverPanel();
	});
	QAction *soundRules = addMenu->addAction(QStringLiteral("Sounds for events…"));
	soundRules->setToolTip(QStringLiteral(
		"A sound that plays at every transition, every image or caption that appears, "
		"every typing caption… Rules, not clips: add one and every such event on the "
		"timeline gets the sound, now and later."));
	connect(soundRules, &QAction::triggered, this, [this]() {
		if (!fullEdit())
			setEditMode(EditMode::Full);
		showSoundsTab();
	});
	addMenu->addSeparator();
	addFxClipAct_ = addMenu->addAction(QStringLiteral("Effect clip"));
	addFxClipAct_->setToolTip(
		QStringLiteral("Drop an effect clip on its own track. It grades every track "
			       "below it, for as long as the clip lasts."));
	connect(addFxClipAct_, &QAction::triggered, this, &VideoEditorWindow::addEffectClip);
	addMenuBtn_->setMenu(addMenu);
	toolbar_->addWidget(addMenuBtn_, ToolbarLayout::Zone::Left);
	// Magnet: snap dragged clips to the playhead, 0 and other clips' edges.
	// A sticky toggle — whichever way you leave it is how the next session opens.
	snapBtn_ = new QPushButton(QStringLiteral("Snap"), this);
	snapBtn_->setCheckable(true);
	snapBtn_->setVisible(false); // Full editing only
	auto applySnap = [this](bool on) {
		if (timelineView_)
			timelineView_->setSnapEnabled(on);
		snapBtn_->setText(on ? QStringLiteral("Snap on")
				     : QStringLiteral("Snap off"));
		snapBtn_->setToolTip(
			on ? QStringLiteral("Magnet ON (N) — dragging a clip or trimming an "
					    "edge sticks to the playhead, to 0 and to other "
					    "clips' edges. A white guide shows the hold.")
			   : QStringLiteral("Magnet OFF (N) — clips and trim edges follow the "
					    "cursor exactly."));
		QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"))
			.setValue(QStringLiteral("editor/timelineSnap"), on);
	};
	connect(snapBtn_, &QPushButton::toggled, this, applySnap);
	{
		// setChecked() only signals on a change, so apply the stored state by
		// hand as well — otherwise "on" would leave the label unset.
		const bool on = QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"))
					.value(QStringLiteral("editor/timelineSnap"), true)
					.toBool();
		snapBtn_->setChecked(on);
		applySnap(on);
	}
	toolbar_->addWidget(snapBtn_, ToolbarLayout::Zone::Left);
	fitBtn_ = new QPushButton(QStringLiteral("Fit"), this);
	fitBtn_->setToolTip(QStringLiteral("Zoom the timeline out so the whole edit fits"));
	fitBtn_->setVisible(false); // Full editing only
	connect(fitBtn_, &QPushButton::clicked, this, [this]() {
		if (timelineView_)
			timelineView_->zoomToFit();
	});
	toolbar_->addWidget(fitBtn_, ToolbarLayout::Zone::Left);
	// Randomise the clip order: many versions of one edit, each a Ctrl+Z from
	// the last, to find the sequence that plays best.
	randomBtn_ = new QPushButton(QStringLiteral("Randomize"), this);
	randomBtn_->setToolTip(QStringLiteral(
		"Shuffle the order of the clips (Ctrl+R). Opens the options; Ctrl+Z brings the "
		"previous order back, so try as many as you like."));
	randomBtn_->setVisible(false); // Multi-Cut and Full editing
	connect(randomBtn_, &QPushButton::clicked, this, &VideoEditorWindow::openRandomizePanel);
	toolbar_->addWidget(randomBtn_, ToolbarLayout::Zone::Left);
	// The editing console: exact numbers and whole-timeline edits by typing,
	// and the same lines saved as templates for the next project.
	consoleBtn_ = new QPushButton(QStringLiteral("Console"), this);
	consoleBtn_->setToolTip(QStringLiteral(
		"A line of JavaScript against the timeline (Ctrl+`): clip.position = [500, 300], "
		"clips.forEach(c => c.scale = 1.1), run(\"template\"). Each line is one undo step."));
	consoleBtn_->setVisible(false); // Full editing only, and only with an engine
	connect(consoleBtn_, &QPushButton::clicked, this, &VideoEditorWindow::openConsole);
	toolbar_->addWidget(consoleBtn_, ToolbarLayout::Zone::Left);
	toolbar_->addWidget(cropToggle_, ToolbarLayout::Zone::Left);
	toolbar_->addWidget(resetCrop, ToolbarLayout::Zone::Left);
	// What the output is: size and length. Read while editing, so it is as
	// legible as the controls around it rather than a faint footnote.
	infoLabel_ = new QLabel(this);
	infoLabel_->setStyleSheet(QStringLiteral("color:#c8ccd4; font-family:monospace;"));
	infoLabel_->setToolTip(QStringLiteral("Output size and length"));
	toolbar_->addWidget(infoLabel_, ToolbarLayout::Zone::Right);
	// Load / Save project, Export and Close live in the File menu now (and
	// Keyboard shortcuts in Tools). The buttons stay as hidden slots the menu
	// items click, so every connection below is unchanged.
	auto *openProjBtn = new QPushButton(QStringLiteral("Load project"), this);
	auto *saveProjBtn = new QPushButton(QStringLiteral("Save project"), this);
	auto *saveBtn = new QPushButton(QStringLiteral("Export…"), this);
	saveBtn->setDefault(true);
	auto *cancelBtn = new QPushButton(QStringLiteral("Close"), this);
	for (QPushButton *b : {openProjBtn, saveProjBtn, saveBtn, cancelBtn})
		b->setVisible(false);
	openProjBtn_ = openProjBtn;
	saveProjBtn_ = saveProjBtn;
	exportBtn_ = saveBtn;
	closeBtn_ = cancelBtn;

	splitter->addWidget(bottomPane);
	splitter->setStretchFactor(0, 3); // preview grows more than the editing area
	splitter->setStretchFactor(1, 2);
	vsplit_ = splitter;
	// Where you drag the divider is remembered for the mode you are in: Simple
	// Trim wants a big preview over one short bar, Full Editing a tall track
	// stack. splitterMoved is only the user's own drags, never setSizes.
	connect(splitter, &QSplitter::splitterMoved, this, [this]() {
		if (!vsplit_ || vsplit_->sizes().size() != 2)
			return;
		const int bottom = vsplit_->sizes().at(1);
		QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"))
			.setValue(QStringLiteral("editor/splitBottom/%1").arg(int(mode())), bottom);
	});

	// ---- Right-side inspector: properties of the selected cut / trim range.
	// Portrait clips leave wide black bars beside the preview; this panel puts
	// that space to work as a live clip inspector.
	inspector_ = new QWidget(this);
	// 180 was narrow enough to clip the value fields, the Save/Save As row and
	// several hints, leaving a horizontal scrollbar to reach them.
	inspector_->setMinimumWidth(inspectorParams_.minWidth);
	inspector_->setVisible(false); // collapsed until the Inspector button opens it
	auto *insOuter = new QVBoxLayout(inspector_);
	insOuter->setContentsMargins(0, 0, 0, 0);
	insOuter->setSpacing(0);

	// The header stays put; only the sections below it scroll.
	auto *insHeader = new QLabel(QStringLiteral("Inspector"), this);
	insHeader->setStyleSheet(
		QStringLiteral("font-weight:bold; color:#e8eaed; padding:8px 10px 2px 10px;"));
	insOuter->addWidget(insHeader);

	// Two tabs, because the panel holds two different KINDS of thing and they
	// were stacked in one column: the project's own facts and settings, which
	// are true whatever is selected, and the properties of the clip you are
	// working on. Stacked, the clip you had just clicked was below a screenful
	// of project metadata, and every selection meant scrolling past it again.
	insTabs_ = new QTabWidget(inspector_);
	insTabs_->setDocumentMode(true);
	// Four tabs in the narrowest panel: tighter padding instead of scroll
	// arrows, and an elided label as the last resort rather than a tab you
	// have to scroll the strip to reach.
	insTabs_->tabBar()->setUsesScrollButtons(false);
	insTabs_->tabBar()->setElideMode(Qt::ElideRight);
	insTabs_->tabBar()->setExpanding(true);
	insTabs_->setStyleSheet(QStringLiteral("QTabBar::tab { padding: 5px 8px; min-width: 0px; }"));
	insOuter->addWidget(insTabs_, 1);

	// Each tab scrolls on its own. One shared scroll area would carry the
	// project tab's position into the clip tab, which lands you half-way down a
	// panel you have just opened.
	//
	// Project, Effects, clip transform, keyframes, text and script add up to far
	// more than a window's height. Without a scroll area the layout squeezes the
	// sections past their minimums and they overlap, so the content lives in one.
	auto mkTab = [this](const QString &title) {
		auto *scroll = new QScrollArea(insTabs_);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		// AsNeeded, not AlwaysOff: the panel can be dragged narrower than a
		// row's minimum width, and with no bar those controls would just be cut
		// off.
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		scroll->viewport()->setAutoFillBackground(false);
		scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
		// The wheel scrolls this panel; it never edits what happens to be under
		// the pointer. See ui/WheelGuard.hpp -- a spin box quietly taking a
		// scroll meant for the panel is a value changed without anyone deciding
		// to change it.
		new WheelGuard(scroll, this);
		auto *content = new QWidget(scroll);
		content->setAutoFillBackground(false);
		scroll->setWidget(content);
		auto *lay = new QVBoxLayout(content);
		lay->setContentsMargins(10, 6, 10, 8);
		lay->setSpacing(6);
		insTabs_->addTab(scroll, title);
		return lay;
	};

	QVBoxLayout *projLayout = mkTab(QStringLiteral("Project"));
	QVBoxLayout *insLayout = mkTab(QStringLiteral("Clip"));
	// A third tab for the clip's SOURCE range -- where in the file it comes
	// from, how fast it plays, how long it ends up. Read-only facts, and a
	// different question from the editable properties beside them: "what part
	// of the file is this" rather than "what does it look like". Together in
	// one tab they were a block of numbers you scrolled past to reach the
	// controls, in every mode, whether or not you were trimming.
	QVBoxLayout *srcLayout = mkTab(QStringLiteral("Source"));
	// Sounds for events: a rule per kind of thing, not a clip per event.
	QVBoxLayout *sndLayout = mkTab(QStringLiteral("Sounds"));
	// The tab stays there with nothing selected rather than disappearing: a tab
	// strip that changes shape as you work is a strip you cannot aim at.
	clipEmpty_ = new QLabel(QStringLiteral("Select a clip to see its properties."), this);
	clipEmpty_->setWordWrap(true);
	clipEmpty_->setStyleSheet(QStringLiteral("color:#7f858e; padding:10px 2px;"));
	insLayout->addWidget(clipEmpty_);
	// The Dev panel tunes the inspector's margins through this; the clip tab is
	// the one with the rows it is talking about.
	insContentLayout_ = insLayout;

	// Come back to the tab you were last on, like the Dev panel does.
	{
		QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		const int want = st.value(QStringLiteral("editor/inspectorTab"), kInsTabProject).toInt();
		if (want >= 0 && want < insTabs_->count())
			insTabs_->setCurrentIndex(want);
	}
	connect(insTabs_, &QTabWidget::currentChanged, this, [this](int i) {
		QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		st.setValue(QStringLiteral("editor/inspectorTab"), i);
		// The clip tab's panels are only synced when something asks; arriving on
		// it by hand is one of those times. The other tabs catch up the same way.
		if (i == kInsTabClip) {
			clipTabDirty_ = false;
			syncClipInspector();
		}
		refreshShownInspectorTab();
	});

	// Project-level metadata + actions.
	buildProjectInspector(projLayout);
	buildTagsSection(projLayout);
	projLayout->addStretch(1);
	// The project-level "Shader effects" panel used to live here. A shader is a
	// component now ("Add Component -> Shader"), so it is added to a clip -- or to
	// an effect clip spanning the timeline, which is what the old whole-output
	// chain was -- and gets keyframes and multi-clip editing it never had.

	// Components. Built-ins first, then whatever the user has written — through
	// the same registry, so a scripted component is not a second-class citizen.
	// The folder is watched, so saving a file re-registers it and the next frame
	// renders the new version.
	registerBuiltinComponents(ComponentRegistry::instance());
	reloadComponentsFromDisk();
	componentWatch_ = new QFileSystemWatcher(this);
	componentWatch_->addPath(componentsDirPath());
	connect(componentWatch_, &QFileSystemWatcher::directoryChanged, this,
		[this](const QString &) { reloadComponentsFromDisk(); });
	connect(componentWatch_, &QFileSystemWatcher::fileChanged, this, [this](const QString &f) {
		if (QFile::exists(f) && !componentWatch_->files().contains(f))
			componentWatch_->addPath(f); // editors replace files on save
		reloadComponentsFromDisk();
	});

	// Transform scripts: GUI-thread evaluator + live reload of the folder.
	scriptEval_ = std::make_unique<TransformEvaluator>();
	scriptsDirPath(); // create + seed the examples
	refreshScriptList();
	scriptWatch_ = new QFileSystemWatcher(this);
	scriptWatch_->addPath(scriptsDir_);
	connect(scriptWatch_, &QFileSystemWatcher::directoryChanged, this, [this](const QString &) {
		refreshScriptList();
		reloadScriptsFromDisk();
		reloadComponentsFromDisk(); // the same file is also a component type
	});
	connect(scriptWatch_, &QFileSystemWatcher::fileChanged, this, [this](const QString &p) {
		if (QFile::exists(p) && !scriptWatch_->files().contains(p))
			scriptWatch_->addPath(p); // editors replace files on save
		reloadScriptsFromDisk();
		reloadComponentsFromDisk();
	});

	// ---- Source range (its own tab) --------------------------------------
	inspTitle_ = new QLabel(QString(), this);
	inspTitle_->setStyleSheet(QStringLiteral("color:#c8ccd4;"));
	inspTitle_->setWordWrap(true);
	srcLayout->addWidget(inspTitle_);
	srcForm_ = new QWidget(this);
	auto *insForm = new QFormLayout(srcForm_);
	insForm->setLabelAlignment(Qt::AlignLeft);
	insForm->setContentsMargins(0, 4, 0, 0);
	insForm->setHorizontalSpacing(10);
	insForm->setVerticalSpacing(4);
	auto mkVal = [this]() {
		auto *l = new QLabel(QStringLiteral("—"), this);
		l->setStyleSheet(QStringLiteral("color:#e8eaed; font-family:monospace;"));
		l->setTextInteractionFlags(Qt::TextSelectableByMouse);
		return l;
	};
	auto mkKey = [this](const QString &t) {
		auto *l = new QLabel(t, this);
		l->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
		return l;
	};
	inspInMs_ = mkVal();
	inspOutMs_ = mkVal();
	inspSrcLen_ = mkVal();
	inspSpeed_ = mkVal();
	inspOutLen_ = mkVal();
	insForm->addRow(mkKey(QStringLiteral("Source in")), inspInMs_);
	insForm->addRow(mkKey(QStringLiteral("Source out")), inspOutMs_);
	insForm->addRow(mkKey(QStringLiteral("Source length")), inspSrcLen_);
	insForm->addRow(mkKey(QStringLiteral("Speed")), inspSpeed_);
	insForm->addRow(mkKey(QStringLiteral("Output length")), inspOutLen_);
	srcLayout->addWidget(srcForm_);
	inspHint_ = new QLabel(QString(), this);
	inspHint_->setWordWrap(true);
	inspHint_->setStyleSheet(QStringLiteral("color:#7f858e;"));
	srcLayout->addWidget(inspHint_);
	// Nothing selected is a state this tab has to say out loud too: five dashes
	// under a heading look like a clip whose numbers failed to load.
	srcEmpty_ = new QLabel(QStringLiteral("Select a clip to see where it comes from."), this);
	srcEmpty_->setWordWrap(true);
	srcEmpty_->setStyleSheet(QStringLiteral("color:#7f858e; padding:10px 2px;"));
	srcLayout->addWidget(srcEmpty_);
	srcLayout->addStretch(1);
	buildSoundsTab(sndLayout);

	// Full-editing: per-clip zoom/position/keyframes and text styling.
	buildClipInspector(insLayout);
	buildTransitionInspector(insLayout);
	buildSpotlightInspector(insLayout);

	// The component list. It sits below the panels it is replacing rather than
	// instead of them: the pose, the effects and the spotlight are still their
	// own systems until each is ported to a component, and hiding a working
	// control before its replacement exists would just remove a feature.
	componentPanel_ = new ComponentPanel(ComponentRegistry::instance(), this);
	componentPanel_->setVisible(false);
	// Every operation is applied to EVERY selected clip, in one edit and so one
	// undo step. There is no single-clip path: one clip is the case where the
	// selection has one member, which is what stops the two drifting apart.
	connect(componentPanel_, &ComponentPanel::propertyEdited, this,
		[this](const QString &t, int o, const QString &key, const QVariant &v) {
			editSharedComponent(t, o, [&](ComponentInstance &ci) {
				// An ANIMATED property is driven by its keys, so writing the
				// static value would be overwritten on the next frame and the
				// control would look dead. Write the key at the playhead.
				auto k = ci.keys.find(key);
				if (k != ci.keys.end() && !k->isEmpty()) {
					const qint64 t0 = componentEditTimeFor(ci);
					const ComponentType *ty = ComponentRegistry::instance().find(t);
					const PropDef *pd = ty ? defFor(*ty, key) : nullptr;
					const double kv = pd ? propKeyValue(*pd, v) : v.toDouble();
					for (PropKey &pk : *k)
						if (std::abs(pk.tMs - t0) <= 1) {
							pk.v = kv;
							return;
						}
					PropKey pk;
					pk.tMs = t0;
					pk.v = kv;
					k->append(pk);
					std::sort(k->begin(), k->end(),
						  [](const PropKey &a, const PropKey &b) {
							  return a.tMs < b.tMs;
						  });
					return;
				}
				ci.props.insert(key, v);
			});
		});
	connect(componentPanel_, &ComponentPanel::componentEnableChanged, this,
		[this](const QString &t, int o, bool on) {
			editSharedComponent(t, o, [on](ComponentInstance &ci) { ci.enabled = on; });
		});
	connect(componentPanel_, &ComponentPanel::componentTimingChanged, this,
		[this](const QString &t, int o, qint64 in, qint64 out) {
			// -1 means "this box did not change". Without that, editing In on a
			// mixed selection would flatten everyone's Out to whatever this
			// panel happened to be showing.
			editSharedComponent(t, o, [in, out](ComponentInstance &ci) {
				if (in >= 0)
					ci.inMs = in;
				if (out >= 0)
					ci.outMs = out;
			});
		});
	connect(componentPanel_, &ComponentPanel::componentRemoved, this,
		[this](const QString &t, int o) {
			timelineView_->applyToSelection([&](TlClip &c) {
				int seen = 0;
				for (int i = 0; i < c.components.size(); ++i)
					if (c.components[i].typeId == t && seen++ == o) {
						c.components.remove(i);
						return;
					}
			});
			afterComponentEdit();
		});
	connect(componentPanel_, &ComponentPanel::componentMoved, this,
		[this](const QString &t, int o, int delta) {
			timelineView_->applyToSelection([&](TlClip &c) {
				int seen = 0;
				for (int i = 0; i < c.components.size(); ++i)
					if (c.components[i].typeId == t && seen++ == o) {
						const int j = i + delta;
						if (j >= 0 && j < c.components.size())
							c.components.swapItemsAt(i, j);
						return;
					}
			});
			afterComponentEdit();
		});
	connect(componentPanel_, &ComponentPanel::componentAdded, this, [this](const QString &t) {
		const ComponentType *type = ComponentRegistry::instance().find(t);
		timelineView_->applyToSelection([&](TlClip &c) {
			ComponentInstance ci;
			ci.typeId = t;
			ci.instanceId = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
			if (type)
				for (const PropDef &d : type->props)
					ci.props.insert(d.key, d.def);
			c.components.append(ci);
		});
		afterComponentEdit();
	});
	connect(componentPanel_, &ComponentPanel::componentReset, this,
		[this](const QString &t, int o) {
			const ComponentType *type = ComponentRegistry::instance().find(t);
			if (!type)
				return;
			editSharedComponent(t, o, [type](ComponentInstance &ci) {
				ci.props.clear();
				ci.keys.clear(); // a reset that left the animation behind is not one
				for (const PropDef &d : type->props)
					ci.props.insert(d.key, d.def);
			});
		});
	connect(componentPanel_, &ComponentPanel::componentCopied, this,
		[this](const QString &t, int o) {
			// From the PRIMARY clip: with several selected there is no one set
			// of values to copy, and the primary is the one the user clicked.
			const TlClip *c = timelineView_->selectedClipPtr();
			if (!c)
				return;
			int seen = 0;
			for (const ComponentInstance &ci : c->components)
				if (ci.typeId == t && seen++ == o) {
					componentClipboard_ = ci;
					componentClipboardValid_ = true;
					// So the Add menu can offer to paste it by name, onto
					// clips that do not have one yet.
					const ComponentType *ty =
						ComponentRegistry::instance().find(t);
					componentPanel_->setClipboard(
						t, ty ? ty->displayName : t,
						ty ? ty->clipKinds : unsigned(ClipKindAll));
					return;
				}
		});
	connect(componentPanel_, &ComponentPanel::componentPasteRequested, this, [this]() {
		if (!componentClipboardValid_)
			return;
		// Onto EVERY selected clip, in one edit and so one undo step, like
		// every other component operation here. Each clip gets its own fresh
		// instance id when the component is new to it: two clips sharing one id
		// would be two rows the Inspector cannot tell apart.
		timelineView_->applyToSelection([this](TlClip &c) {
			pasteComponentInto(c.components, componentClipboard_,
					   QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
		});
		afterComponentEdit();
	});
	connect(componentPanel_, &ComponentPanel::componentPasted, this,
		[this](const QString &t, int o) {
			if (!componentClipboardValid_ || componentClipboard_.typeId != t)
				return; // pasting a Blur's values onto a Speed means nothing
			editSharedComponent(t, o, [this](ComponentInstance &ci) {
				ci.props = componentClipboard_.props;
				ci.keys = componentClipboard_.keys;
				ci.enabled = componentClipboard_.enabled;
			});
		});
	connect(componentPanel_, &ComponentPanel::componentDuplicated, this,
		[this](const QString &t, int o) {
			timelineView_->applyToSelection([&](TlClip &c) {
				int seen = 0;
				for (int i = 0; i < c.components.size(); ++i)
					if (c.components[i].typeId == t && seen++ == o) {
						ComponentInstance copy = c.components[i];
						copy.instanceId =
							QUuid::createUuid()
								.toString(QUuid::WithoutBraces)
								.left(8);
						c.components.insert(i + 1, copy);
						return;
					}
			});
			afterComponentEdit();
		});
	connect(componentPanel_, &ComponentPanel::keyframeToggled, this,
		[this](const QString &t, int o, const QString &key) {
			const ComponentType *type = ComponentRegistry::instance().find(t);
			editSharedComponent(t, o, [&](ComponentInstance &ci) {
				const qint64 t0 = componentEditTimeFor(ci);
				auto &keys = ci.keys[key];
				for (int i = 0; i < keys.size(); ++i)
					if (std::abs(keys[i].tMs - t0) <= 1) {
						keys.remove(i);
						// No keys left is not animated; an empty list
						// would keep the control reading as keyed.
						if (keys.isEmpty())
							ci.keys.remove(key);
						return;
					}
				PropKey pk;
				pk.tMs = t0;
				const PropDef *pd = type ? defFor(*type, key) : nullptr;
				pk.v = pd ? propKeyValue(*pd, propAt(*type, ci, key, t0)) : 0.0;
				keys.append(pk);
				std::sort(keys.begin(), keys.end(),
					  [](const PropKey &a, const PropKey &b) { return a.tMs < b.tMs; });
			});
		});
	// A pinned property maps back to the clip field it came from, on every
	// selected clip. The pose goes through the same path the preview drag uses,
	// which keeps auto-keyframe and the keyframe editor working as they did.
	connect(componentPanel_, &ComponentPanel::pinnedEdited, this,
		[this](const QString &typeId, const QString &key, double v) {
			if (typeId == QStringLiteral("harpia.speed")) {
				timelineView_->applyToSelection(
					[v](TlClip &c) { c.speed = std::clamp(v, 0.1, 20.0); });
				afterComponentEdit();
				return;
			}
			// Several texts selected: they keep their layout. Each takes the
			// clicked one's CHANGE -- the same offset, factor or turn -- as a
			// drag in the preview does, rather than all landing on one value.
			const QVector<QPair<int, int>> texts = selectedTextBatch();
			const TlClip *clicked = timelineView_->selectedClipPtr();
			if (texts.size() >= 2 && clicked && key != QStringLiteral("opacity")) {
				const TlTransform was = clicked->transformAt(timelinePlayheadMs());
				const double from = key == QStringLiteral("posX")   ? was.posX
						    : key == QStringLiteral("posY") ? was.posY
						    : key == QStringLiteral("scale")
							    ? was.scale
							    : was.rotation;
				timelineView_->editClips(texts, [&](TlClip &c) {
					TlTransform tf = c.transformAt(timelinePlayheadMs());
					if (key == QStringLiteral("posX"))
						tf.posX += v - from;
					else if (key == QStringLiteral("posY"))
						tf.posY += v - from;
					else if (key == QStringLiteral("scale"))
						tf.scale = std::clamp(tf.scale * v / std::max(0.0001, from), 0.05, 20.0);
					else if (key == QStringLiteral("rotation"))
						tf.rotation += v - from;
					c.setBaseTransform(tf);
				});
				afterComponentEdit();
				return;
			}
			timelineView_->applyToSelection([&](TlClip &c) {
				TlTransform tf = c.transformAt(timelinePlayheadMs());
				if (key == QStringLiteral("posX"))
					tf.posX = v;
				else if (key == QStringLiteral("posY"))
					tf.posY = v;
				else if (key == QStringLiteral("scale"))
					tf.scale = v;
				else if (key == QStringLiteral("rotation"))
					tf.rotation = v;
				else if (key == QStringLiteral("opacity"))
					tf.opacity = v;
				c.setBaseTransform(tf);
			});
			afterComponentEdit();
		});
	// A component's own button. The window does not know what any of them mean:
	// it looks the action up on the type, runs it over a copy of that
	// component's properties, and writes the result back -- so a built-in and a
	// user-written component reach this by exactly the same route, and a new
	// action costs nothing here.
	connect(componentPanel_, &ComponentPanel::actionInvoked, this,
		[this](const QString &typeId, int ordinal, const QString &actionId) {
			// The Sound component's buttons need the window: a sound picker
			// and the speakers.
			if (typeId == QLatin1String(kSoundComponentId)) {
				const bool in = actionId.endsWith(QLatin1String("In"));
				const QString key = in ? QStringLiteral("inSound") : QStringLiteral("outSound");
				if (actionId.startsWith(QLatin1String("pick"))) {
					QString name;
					const int src = pickSound(QCursor::pos(), &name);
					if (src < 0)
						return;
					editSharedComponent(typeId, ordinal, [&](ComponentInstance &ci) {
						ci.props.insert(key, src);
						if (!in)
							ci.props.insert(QStringLiteral("outOn"), true);
					});
					afterComponentEdit();
					return;
				}
				// Play: the primary clip's sound for that slot.
				if (const TlClip *c = timelineView_->selectedClipPtr()) {
					int seen = 0;
					for (const ComponentInstance &ci : c->components)
						if (ci.typeId == typeId && seen++ == ordinal) {
							const int src = ci.props.value(key, 0).toInt();
							if (src > 0)
								previewSound(src);
							else if (infoLabel_)
								infoLabel_->setText(QStringLiteral("No sound chosen yet."));
						}
				}
				return;
			}
			const ComponentType *type = ComponentRegistry::instance().find(typeId);
			if (!type)
				return;
			const ComponentAction *act = nullptr;
			for (const ComponentAction &a : type->actions)
				if (a.id == actionId)
					act = &a;
			if (!act || !act->run)
				return;

			if (type->addable) {
				editSharedComponent(typeId, ordinal, [act](ComponentInstance &ci) {
					act->run(ci.props);
					// A keyed property would immediately overwrite whatever
					// the action just wrote, leaving a button that visibly
					// does nothing. Only the keys it actually touched go.
					for (auto it = ci.props.cbegin(); it != ci.props.cend(); ++it)
						ci.keys.remove(it.key());
				});
				afterComponentEdit();
				return;
			}

			// A pinned component is backed by the clip's own fields rather than
			// by a ComponentInstance, so the action runs against a bag built
			// from those fields and the result is written back into them.
			timelineView_->applyToSelection([act](TlClip &c) {
				const TlTransform base = c.baseTransform();
				PropBag p;
				p[QStringLiteral("scale")] = base.scale;
				p[QStringLiteral("posX")] = base.posX;
				p[QStringLiteral("posY")] = base.posY;
				p[QStringLiteral("rotation")] = base.rotation;
				p[QStringLiteral("opacity")] = base.opacity;
				act->run(p);
				TlTransform tf;
				tf.scale = p.value(QStringLiteral("scale"), base.scale).toDouble();
				tf.posX = p.value(QStringLiteral("posX"), base.posX).toDouble();
				tf.posY = p.value(QStringLiteral("posY"), base.posY).toDouble();
				tf.rotation =
					p.value(QStringLiteral("rotation"), base.rotation).toDouble();
				tf.opacity = p.value(QStringLiteral("opacity"), base.opacity).toDouble();
				c.setBaseTransform(tf);
				// Same reasoning as above: keyframes would win over the pose
				// the action just set, so Reset has to clear them to mean
				// anything on an animated clip.
				c.keys.clear();
			});
			afterComponentEdit();
		});
	insLayout->addWidget(componentPanel_);
	// Animation and Tags: after the components (see buildClipInspector).
	insLayout->addWidget(clipTailBox_);

	insLayout->addStretch(1);
	fitInspectorToWidth();

	// ---- Sources: a floating, toggleable panel with two tabs ----------------
	sourcesPanel_ = new QWidget(this, Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
	sourcesPanel_->setWindowTitle(QStringLiteral("Sources"));
	// Wide enough that a library name like "DarkIsland_DFI Daily Video
	// (2)_0108_clip.mp4" reads whole beside its thumbnail; the narrow default
	// it had cut every one of them off. The floor keeps a dragged-down panel
	// from going back there.
	sourcesPanel_->resize(560, 620);
	sourcesPanel_->setMinimumWidth(440);
	auto *sideLayout = new QVBoxLayout(sourcesPanel_);
	sideLayout->setContentsMargins(8, 8, 8, 8);
	sideLayout->setSpacing(6);
	auto *srcTabs = new QTabWidget(sourcesPanel_);
	sourcesTabs_ = srcTabs;
	srcTabs->setObjectName(QStringLiteral("editorSourcesTabs"));

	// Tab 1 — "Sources": the videos currently loaded into this edit.
	auto *usedTab = new QWidget(srcTabs);
	auto *usedLayout = new QVBoxLayout(usedTab);
	usedLayout->setContentsMargins(0, 6, 0, 0);
	usedLayout->setSpacing(6);
	sourceList_ = new UrlDragList(usedTab); // rows drag out as files, onto the timeline
	sourceList_->setObjectName(QStringLiteral("editorProjectSources"));
	sourceList_->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(sourceList_, &QListWidget::customContextMenuRequested, this,
		[this](const QPoint &pos) { showSourceFileMenu(sourceList_, pos); });
	sourceList_->setIconSize(QSize(128, 72));
	sourceList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	sourceList_->setToolTip(QStringLiteral(
		"Videos you can cut from. Click one to cut from it; double-click to add its "
		"whole clip at the playhead, or drag it onto the lane you want."));
	usedLayout->addWidget(sourceList_, 1);
	auto *addSrcBtn = new QPushButton(QStringLiteral("Add video…"), usedTab);
	addSrcBtn->setToolTip(QStringLiteral("Add another video as a source (or drag files onto the window)"));
	connect(addSrcBtn, &QPushButton::clicked, this, &VideoEditorWindow::onAddSource);
	usedLayout->addWidget(addSrcBtn);
	auto *removeSrcBtn = new QPushButton(QStringLiteral("Remove"), usedTab);
	removeSrcBtn->setToolTip(QStringLiteral("Remove the selected source (only if no cut uses it)"));
	connect(removeSrcBtn, &QPushButton::clicked, this, &VideoEditorWindow::onRemoveSource);
	usedLayout->addWidget(removeSrcBtn);
	connect(sourceList_, &QListWidget::itemSelectionChanged, this,
		&VideoEditorWindow::onSourceRowChanged);
	connect(sourceList_, &QListWidget::itemDoubleClicked, this,
		&VideoEditorWindow::onSourceDoubleClicked);
	srcTabs->addTab(usedTab, QStringLiteral("Sources"));

	// Tab 2 — "Library": your existing recordings, double-click to add as a source.
	auto *libTab = new QWidget(srcTabs);
	auto *libLayout = new QVBoxLayout(libTab);
	libLayout->setContentsMargins(0, 6, 0, 0);
	libLayout->setSpacing(6);
	libraryList_ = new UrlDragList(libTab);
	libraryList_->setObjectName(QStringLiteral("editorRecordingLibrary"));
	libraryList_->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(libraryList_, &QListWidget::customContextMenuRequested, this,
		[this](const QPoint &pos) { showSourceFileMenu(libraryList_, pos); });
	libraryList_->setIconSize(QSize(128, 72));
	libraryList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	libraryList_->setToolTip(QStringLiteral(
		"Recordings from your library. Double-click to add one as a source (and, in Full "
		"editing, onto the timeline at the playhead); or drag it straight onto a lane."));
	libLayout->addWidget(libraryList_, 1);
	auto *refreshLibBtn = new QPushButton(QStringLiteral("Refresh"), libTab);
	connect(refreshLibBtn, &QPushButton::clicked, this, &VideoEditorWindow::refreshLibrary);
	libLayout->addWidget(refreshLibBtn);
	connect(libraryList_, &QListWidget::itemDoubleClicked, this,
		&VideoEditorWindow::onLibraryDoubleClicked);
	srcTabs->addTab(libTab, QStringLiteral("Library"));

	sideLayout->addWidget(srcTabs, 1);
	sideLayout->addWidget(
		infoHint(QStringLiteral("Drag a clip onto a timeline lane, or double-click it to add it at the playhead."),
			 sourcesPanel_),
		0, Qt::AlignLeft);

	thumbCache_ = new ThumbnailCache(this);
	connect(thumbCache_, &ThumbnailCache::ready, this, &VideoEditorWindow::onThumbReady);
	refreshLibrary();
	sourcesPanel_->installEventFilter(this); // keep the toolbar toggle in sync

	// (preview/editing split) | right inspector. Sources float over this.
	auto *hsplit = new QSplitter(Qt::Horizontal, this);
	hsplit->setChildrenCollapsible(false);
	hsplit->setHandleWidth(6);
	hsplit->addWidget(splitter);
	hsplit->addWidget(inspector_);
	hsplit->setStretchFactor(0, 5); // editing area takes the lion's share
	hsplit_ = hsplit;
	hsplit->setStretchFactor(1, 1); // inspector
	hsplit->setSizes({820, 190});
	root->addWidget(hsplit, 1);
	// In Vertical, where the preview column is dragged to is remembered.
	// (Standard's divider is vsplit_'s, saved per mode by its own handler.)
	connect(hsplit, &QSplitter::splitterMoved, this, [this]() {
		if (editorLayout_ != EditorLayout::Vertical || !hsplit_ || !topPane_)
			return;
		const int w = hsplit_->sizes().value(hsplit_->indexOf(topPane_));
		if (w > 0)
			QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"))
				.setValue(QStringLiteral("editor/verticalPreviewW"), w);
	});
	setEditorLayout(savedEditorLayout());

	playTimer_ = new QTimer(this);
	playTimer_->setInterval(33); // ~30 fps preview
	connect(playTimer_, &QTimer::timeout, this, &VideoEditorWindow::onPlayTick);
	connect(playBtn_, &QPushButton::clicked, this, &VideoEditorWindow::onPlayPause);
	connect(speedSlider_, &QSlider::valueChanged, this, &VideoEditorWindow::onSpeedChanged);
	connect(speedSpin_, &QDoubleSpinBox::valueChanged, this, &VideoEditorWindow::onSpeedSpinChanged);
	connect(this, &QDialog::rejected, this, &VideoEditorWindow::stopPlayback);

	previewTimer_ = new QTimer(this);
	previewTimer_->setSingleShot(true);
	previewTimer_->setInterval(20);
	connect(previewTimer_, &QTimer::timeout, this, &VideoEditorWindow::onPreviewTick);

	// Preview frames are decoded on a worker thread; the GUI thread never waits
	// on a seek. See PreviewDecoder.hpp for why -- a 4K scrub is 300-400 ms of
	// decode, and it used to happen right here on the GUI thread.
	previewDecoder_ = std::make_unique<PreviewDecoder>();
	connect(previewDecoder_.get(), &PreviewDecoder::frameReady, this,
		&VideoEditorWindow::onPreviewFrameReady);

	// Threading alone does not make a 4K seek fast, it only stops it blocking.
	// For files that are genuinely the wrong shape for scrubbing, a small
	// short-GOP proxy is built in the background and the preview switches to it
	// when it is ready. Export never sees it.
	proxyBuilder_ = std::make_unique<ProxyBuilder>();
	// Housekeeping while nothing is waiting on the pool: keep the proxy cache
	// under 2 GB, least-recently-used first. Off the GUI thread -- it lists
	// and stats a directory that can hold hundreds of files.
	QThreadPool::globalInstance()->start([]() {
		const qint64 freed = pruneProxyCache(2LL * 1024 * 1024 * 1024);
		if (freed > 0)
			qInfo("[harpia] proxy cache pruned: %lld MB freed", (long long)(freed / (1024 * 1024)));
	});
	connect(proxyBuilder_.get(), &ProxyBuilder::ready, this, &VideoEditorWindow::onProxyReady);
	connect(proxyBuilder_.get(), &ProxyBuilder::progress, this,
		&VideoEditorWindow::onProxyProgress);
	connect(proxyBuilder_.get(), &ProxyBuilder::failed, this, [this](int id, const QString &why) {
		// Not worth a dialog: the editor carries on with the original file,
		// exactly as it did before proxies existed. The message goes to the log
		// (main() forwards Qt messages into it) so it is still discoverable.
		qWarning("harpia: preview proxy failed for source %d: %s", id,
			 qUtf8Printable(why));
		proxyProgress_.remove(id);
		proxyPending_.remove(id);
		// The filmstrip was waiting on a proxy that will not arrive, so build it
		// from the original after all -- slow is better than never.
		if (const EditorSource *s = sourceById(id))
			startFilmstrip(id, s->path);
		updateProxyStatus();
	});

	// Undo/redo history: coalesce a burst of edits (a drag, slider sweep) into
	// one snapshot taken shortly after they settle.
	histTimer_ = new QTimer(this);
	histTimer_->setSingleShot(true);
	histTimer_->setInterval(350);
	connect(histTimer_, &QTimer::timeout, this, &VideoEditorWindow::captureSnapshot);
	// ---- Shortcuts -------------------------------------------------------
	// Every binding is registered rather than hard-wired, so the Keyboard
	// Shortcuts panel can list it, rebind it live and spot a collision. The
	// timeline commands no-op outside Full editing, so they never surprise you
	// in the other modes. Frame-sized steps come from the project frame rate,
	// so a nudge always lands on a frame boundary rather than a round number
	// of ms.
	shortcuts_ = new ShortcutRegistry(this, this);
	// Which of the two clipboards is the fresher one. Watched rather than polled
	// on Ctrl+V, because "did this change since I copied clips?" is an ordering
	// question and only the signal knows the order.
	connect(QApplication::clipboard(), &QClipboard::dataChanged, this, [this]() {
		systemCopySeq_ = ++seqCounter_;
		// The clipboard cannot change in front of Harpia unless Harpia did
		// it; ownsClipboard() says the same where the platform can tell.
		systemCopyHere_ = QApplication::activeWindow() != nullptr || QApplication::clipboard()->ownsClipboard();
	});
	auto onTimeline = [this]() { return fullEdit() && timelineView_; };
	auto frameMs = [this]() {
		const double fps = timelineFps();
		return qint64(std::llround(1000.0 / (fps > 1.0 ? fps : 30.0)));
	};
	auto seekTo = [this](qint64 ms) {
		const TimelineModel &model = timelineView_->model();
		qint64 editingEnd = std::max(model.contentDurationMs(), model.durationMs());
		for (qint64 marker : model.markers)
			editingEnd = std::max(editingEnd, marker);
		const qint64 t = std::clamp<qint64>(ms, 0, editingEnd);
		timelineView_->setPlayhead(t);
		onTimelineScrub(t);
	};
	auto cmd = [this](const char *id, const char *label, const char *category,
			  QVector<QKeySequence> keys, std::function<void()> fn) {
		ShortcutCommand c;
		c.id = QLatin1String(id);
		c.label = QLatin1String(label);
		c.category = QLatin1String(category);
		c.defaults = std::move(keys);
		c.run = std::move(fn);
		shortcuts_->addCommand(c);
	};
	auto tlCmd = [&](const char *id, const char *label, const char *category,
			 QVector<QKeySequence> keys, std::function<void()> fn) {
		cmd(id, label, category, std::move(keys), [onTimeline, fn]() {
			if (onTimeline())
				fn();
		});
	};

	cmd("edit.undo", "Undo", "Editing", {QKeySequence::Undo}, [this]() { undo(); });
	cmd("edit.redo", "Redo", "Editing",
	    {QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y)}, [this]() { redo(); });
	mouseHistoryFilter_ = new EditorMouseHistory(this, [this]() { undo(); }, [this]() { redo(); });
	cmd("playback.playPause", "Play / Pause", "Playback", {QKeySequence(Qt::Key_Space)},
	    [this]() { onPlayPause(); });
	// Up / Down on the selected cuts in Multi-Cut: faster / slower by the Dev
	// panel's step (0.2 by default), Shift for five steps at once. Live ONLY in
	// Multi-Cut (`when`): a window-level shortcut eats its key even when the
	// handler does nothing, and Up / Down belong to the lists and spin boxes
	// everywhere else. setEditMode() refreshes the gate.
	auto cutCmd = [this](const char *id, const char *label, QKeySequence key,
			     std::function<void()> fn) {
		ShortcutCommand c;
		c.id = QLatin1String(id);
		c.label = QLatin1String(label);
		c.category = QStringLiteral("Speed");
		c.defaults = {key};
		c.run = std::move(fn);
		c.when = [this]() { return multiCut(); };
		shortcuts_->addCommand(c);
	};
	cutCmd("speed.up", "Speed up selected cuts", QKeySequence(Qt::Key_Up),
	       [this]() { stepSelectedSpeed(+speedStep_); });
	cutCmd("speed.down", "Slow down selected cuts", QKeySequence(Qt::Key_Down),
	       [this]() { stepSelectedSpeed(-speedStep_); });
	cutCmd("speed.upBig", "Speed up selected cuts (large step)", QKeySequence(Qt::SHIFT | Qt::Key_Up),
	       [this]() { stepSelectedSpeed(+5.0 * speedStep_); });
	cutCmd("speed.downBig", "Slow down selected cuts (large step)",
	       QKeySequence(Qt::SHIFT | Qt::Key_Down), [this]() { stepSelectedSpeed(-5.0 * speedStep_); });
	cmd("project.save", "Save project", "Project", {QKeySequence::Save},
	    [this]() { onSaveProject(); });
	cmd("project.export", "Export…", "Export", {QKeySequence(Qt::CTRL | Qt::Key_E)},
	    [this]() { onSave(); });
	cmd("view.shortcuts", "Keyboard shortcuts…", "View",
	    {QKeySequence(Qt::CTRL | Qt::Key_Slash)}, [this]() { openShortcutPanel(); });
	cmd("view.inspector", "Show inspector", "View", {QKeySequence(Qt::CTRL | Qt::Key_I)},
	    [this]() { revealInspector(); });
	cmd("view.fullscreen", "Full screen", "View", {QKeySequence(Qt::Key_F11)},
	    [this]() { setFullScreen(!isFullScreen()); });

	tlCmd("timeline.split", "Split clip at playhead", "Timeline",
	      {QKeySequence(Qt::Key_S), QKeySequence(Qt::CTRL | Qt::Key_K)},
	      [this]() { timelineView_->splitAtPlayhead(); });
	tlCmd("selection.all", "Select all clips", "Selection",
	      {QKeySequence(Qt::CTRL | Qt::Key_A)}, [this]() { timelineView_->selectAllClips(); });
	tlCmd("edit.copy", "Copy clips", "Editing", {QKeySequence::Copy},
	      [this]() { copySelectedClips(false); });
	tlCmd("edit.cut", "Cut clips", "Editing", {QKeySequence::Cut},
	      [this]() { copySelectedClips(true); });
	// Not tlCmd: pasting a picture is meaningful from any mode (it switches to
	// Full editing to place it), so this one must not no-op outside the timeline.
	cmd("edit.paste", "Paste clips, or what you copied elsewhere", "Editing", {QKeySequence::Paste},
	    [this]() { pasteFromClipboard(); });
	// NOT tlCmd: Delete has to work on whatever is selected in whatever mode you
	// are in. It was bound to the Full-editing timeline alone, and because a
	// window-level QShortcut is consulted BEFORE the focused widget's
	// keyPressEvent, the binding swallowed the key everywhere else and then did
	// nothing -- so Multi-Cut's cuts and the voiceover takes had working Delete
	// handlers that could never be reached.
	cmd("edit.delete", "Delete selection", "Editing",
	    {QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)},
	    [this]() { deleteSelection(); });
	tlCmd("marker.toggle", "Add / remove marker", "Markers", {QKeySequence(Qt::Key_M)},
	      [this]() { timelineView_->toggleMarkerAtPlayhead(); });
	// N, not S — S already splits. (Resolve uses N for the magnet too.)
	tlCmd("timeline.snap", "Toggle snapping", "Timeline", {QKeySequence(Qt::Key_N)}, [this]() {
		if (snapBtn_)
			snapBtn_->toggle();
	});
	tlCmd("timeline.fit", "Zoom to fit", "View", {QKeySequence(Qt::Key_F)},
	      [this]() { timelineView_->zoomToFit(); });
	// The fast loop: Ctrl+R, look, Ctrl+Z, Ctrl+R. No panel needed once the
	// options are set; they are remembered.
	// Multi-Cut shuffles its cut list; Full editing its lanes.
	cmd("timeline.randomize", "Randomize clip order", "Timeline", {QKeySequence(Qt::CTRL | Qt::Key_R)},
	    [this]() {
		    if (multiCut() || fullEdit())
			    randomizeClips();
	    });
	tlCmd("view.console", "Editing console", "View", {QKeySequence(Qt::CTRL | Qt::Key_QuoteLeft)},
	      [this]() { openConsole(); });

	// Arrows nudge a selection, or step the playhead when nothing is selected —
	// the same key doing the obvious thing for what you have in hand.
	auto arrow = [this, frameMs, seekTo](int dir, int frames) {
		const qint64 step = frameMs() * frames * dir;
		if (timelineView_->hasSelection())
			timelineView_->nudgeSelection(step);
		else
			seekTo(timelinePlayheadMs() + step);
	};
	tlCmd("nudge.left", "Nudge / step back one frame", "Timeline",
	      {QKeySequence(Qt::Key_Left)}, [arrow]() { arrow(-1, 1); });
	tlCmd("nudge.right", "Nudge / step forward one frame", "Timeline",
	      {QKeySequence(Qt::Key_Right)}, [arrow]() { arrow(+1, 1); });
	tlCmd("nudge.left10", "Nudge / step back ten frames", "Timeline",
	      {QKeySequence(Qt::SHIFT | Qt::Key_Left)}, [arrow]() { arrow(-1, 10); });
	tlCmd("nudge.right10", "Nudge / step forward ten frames", "Timeline",
	      {QKeySequence(Qt::SHIFT | Qt::Key_Right)}, [arrow]() { arrow(+1, 10); });

	// Frame stepping, always the playhead whatever is selected.
	tlCmd("playhead.prevFrame", "Previous frame", "Playback",
	      {QKeySequence(Qt::Key_Comma)},
	      [this, frameMs, seekTo]() { seekTo(timelinePlayheadMs() - frameMs()); });
	tlCmd("playhead.nextFrame", "Next frame", "Playback", {QKeySequence(Qt::Key_Period)},
	      [this, frameMs, seekTo]() { seekTo(timelinePlayheadMs() + frameMs()); });
	tlCmd("playhead.start", "Go to start", "Playback", {QKeySequence(Qt::Key_Home)},
	      [seekTo]() { seekTo(0); });
	tlCmd("playhead.end", "Go to end", "Playback", {QKeySequence(Qt::Key_End)},
	      [this, seekTo]() { seekTo(timelineView_->durationMs()); });
	tlCmd("marker.prev", "Previous marker", "Markers",
	      {QKeySequence(Qt::CTRL | Qt::Key_Left)}, [this, seekTo]() {
		      const qint64 m = timelineView_->markerNear(timelinePlayheadMs(), false);
		      if (m >= 0)
			      seekTo(m);
	      });
	tlCmd("marker.next", "Next marker", "Markers", {QKeySequence(Qt::CTRL | Qt::Key_Right)},
	      [this, seekTo]() {
		      const qint64 m = timelineView_->markerNear(timelinePlayheadMs(), true);
		      if (m >= 0)
			      seekTo(m);
	      });

	// Gestures the mouse owns. Listed so the panel is a complete reference,
	// but not rebindable: the widget reads the mouse directly.
	shortcuts_->addMouseGesture(QStringLiteral("edit.undoMouse"), QStringLiteral("Undo"),
				    QStringLiteral("Editing"), QStringLiteral("Mouse Back (side button)"));
	shortcuts_->addMouseGesture(QStringLiteral("edit.redoMouse"), QStringLiteral("Redo"),
				    QStringLiteral("Editing"), QStringLiteral("Mouse Forward (side button)"));
	shortcuts_->addMouseGesture(QStringLiteral("timeline.zoom"), QStringLiteral("Zoom timeline"),
				    QStringLiteral("View"), QStringLiteral("Ctrl + Mouse wheel"));
	shortcuts_->addMouseGesture(QStringLiteral("timeline.pan"), QStringLiteral("Pan timeline"),
				    QStringLiteral("View"),
				    QStringLiteral("Shift + Mouse wheel / Middle-drag / Alt + drag"));
	shortcuts_->addMouseGesture(QStringLiteral("preview.zoomClip"),
				    QStringLiteral("Zoom the selected clip"),
				    QStringLiteral("Video"),
				    QStringLiteral("Mouse wheel on the preview"));
	shortcuts_->addMouseGesture(QStringLiteral("preview.moveClip"),
				    QStringLiteral("Reposition the selected clip"),
				    QStringLiteral("Video"), QStringLiteral("Drag on the preview"));
	shortcuts_->addMouseGesture(QStringLiteral("timeline.fine"),
				    QStringLiteral("Precise drag (bypass snapping)"),
				    QStringLiteral("Timeline"), QStringLiteral("Hold Shift"));

	shortcuts_->load(); // apply anything the user rebound previously
	// Tooltips carry the live binding, so a rebind is visible without opening
	// the panel.
	connect(shortcuts_, &ShortcutRegistry::bindingsChanged, this,
		&VideoEditorWindow::refreshShortcutHints);
	refreshShortcutHints();
	buildMenuBar(root);
	connect(shortcuts_, &ShortcutRegistry::bindingsChanged, this, &VideoEditorWindow::refreshMenuLabels);

	connect(timeline_, &Timeline::scrub, this, &VideoEditorWindow::onScrub);
	connect(timeline_, &Timeline::hoverScrub, this, &VideoEditorWindow::onHoverScrub);
	connect(timeline_, &Timeline::hoverScrubEnded, this, &VideoEditorWindow::showPlayheadFrame);
	connect(timeline_, &Timeline::startChanged, this, [this]() {
		updateVoiceoverAxis();
		updateInspector();
		scheduleSnapshot();
	});
	connect(timeline_, &Timeline::endChanged, this, [this]() {
		updateVoiceoverAxis();
		updateInspector();
		scheduleSnapshot();
	});
	connect(tracks_, &TrackEditor::scrubSource, this, &VideoEditorWindow::onScrub);
	connect(tracks_, &TrackEditor::hoverScrub, this, &VideoEditorWindow::onHoverScrub);
	connect(tracks_, &TrackEditor::hoverScrubEnded, this, &VideoEditorWindow::showPlayheadFrame);
	connect(tracks_, &TrackEditor::segmentsChanged, this, &VideoEditorWindow::onSegmentsChanged);
	connect(tracks_, &TrackEditor::segmentsChanged, this, &VideoEditorWindow::scheduleSnapshot);
	connect(tracks_, &TrackEditor::selectionChanged, this, &VideoEditorWindow::onSegmentSelected);
	connect(tracks_, &TrackEditor::autoCutRequested, this, &VideoEditorWindow::onAutoCut);
	connect(tracks_, &TrackEditor::inspectRequested, this, &VideoEditorWindow::revealInspector);
	connect(trimModeBtn_, &QPushButton::clicked, this, [this]() { setEditMode(EditMode::Trim); });
	connect(cutModeBtn_, &QPushButton::clicked, this, [this]() { setEditMode(EditMode::MultiCut); });
	connect(fullModeBtn_, &QPushButton::clicked, this, [this]() { setEditMode(EditMode::Full); });
	connect(cropToggle_, &QCheckBox::toggled, this, &VideoEditorWindow::onCropToggled);
	connect(cropToggle_, &QCheckBox::toggled, this, [this]() { scheduleSnapshot(); });
	connect(canvas_, &PreviewCanvas::cropChanged, this, [this]() { scheduleSnapshot(); });
	connect(resetCrop, &QPushButton::clicked, this, [this]() { canvas_->resetCrop(); });
	connect(saveBtn, &QPushButton::clicked, this, &VideoEditorWindow::onSave);
	connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
	connect(openProjBtn, &QPushButton::clicked, this, &VideoEditorWindow::onOpenProject);
	connect(saveProjBtn, &QPushButton::clicked, this, &VideoEditorWindow::onSaveProject);

	// Restore any layout tweaks saved from a previous Developer Panel session
	// (independent of the source).
	{
		TimelineLayoutParams tl = timeline_->layoutParams();
		TrackLayoutParams tr = tracks_->layoutParams();
		VoiceoverLayoutParams vo = voTrack_->layoutParams();
		PreviewLayoutParams pv = canvas_->layoutParams();
		DevPanel::loadInto(tl, tr, vo, pv);
		timeline_->setLayoutParams(tl);
		tracks_->setLayoutParams(tr);
		voTrack_->setLayoutParams(vo);
		canvas_->setLayoutParams(pv);
		timelineView_->setLayoutParams(DevPanel::loadFullTimeline());
		// One palette for all three track widgets.
		const EditorColors cl = DevPanel::loadColors();
		timelineView_->setColors(cl);
		tracks_->setColors(cl);
		voTrack_->setColors(cl);
	}

	// Opened blank: no source to load, and the editor is perfectly valid -- it
	// is simply waiting for one. Everything that needs a video is switched off
	// by updateEmptyState() until the first one arrives.
	buildEmptyPanel();
	if (inPath_.isEmpty()) {
		valid_ = true;
		history_.clear();
		history_.push_back(snapshot());
		histIndex_ = 0;
		updateUndoRedoButtons();
		updateEmptyState();
		return;
	}

	// Open the launch file as the first source.
	const int firstId = addSource(inPath_);
	valid_ = (firstId >= 0);
	if (valid_) {
		setActiveSource(firstId);
		updateInspector();
		updateVoiceoverAxis();
		// Seed the undo history with the untouched state.
		history_.clear();
		history_.push_back(snapshot());
		histIndex_ = 0;
		updateUndoRedoButtons();
	} else {
		infoLabel_->setText(QStringLiteral("Could not open this video."));
		saveBtn->setEnabled(false);
		playBtn_->setEnabled(false);
		speedSlider_->setEnabled(false);
		speedSpin_->setEnabled(false);
		cropToggle_->setEnabled(false);
		trimModeBtn_->setEnabled(false);
		cutModeBtn_->setEnabled(false);
		voImportBtn_->setEnabled(false);
	}

	// Collect every button now (the Developer Panel is created lazily, so its
	// own buttons are excluded) and apply the saved Dev-tunable chrome.
	uiButtons_ = findChildren<QPushButton *>();
	// Only the editor window's own buttons: the voiceover window sizes its
	// Record button itself (Dev panel > Panels).
	uiButtons_.erase(std::remove_if(uiButtons_.begin(), uiButtons_.end(),
					[this](QPushButton *b) { return b->window() != this; }),
			 uiButtons_.end());
	applyChrome(DevPanel::loadChrome());
	applyInspectorParams(DevPanel::loadInspector());
	applyPanelParams(DevPanel::loadPanel());

	// Battery saving keys off the APPLICATION losing focus, not this window's.
	// Window focus would also fire for the editor's own Sources panel and its
	// dialogs, pausing playback every time one of them is clicked.
	connect(qApp, &QGuiApplication::applicationStateChanged, this,
		[this](Qt::ApplicationState st) { setPowerSaving(st != Qt::ApplicationActive); });
}

void VideoEditorWindow::applyPanelParams(const EditorPanelParams &p)
{
	panelConsoleFontPx_ = p.consoleFontPx;
	panelConsoleMinH_ = p.consoleMinH;
	panelPopupRows_ = p.consolePopupRows;
	panelChipGap_ = p.tagChipGap;
	const bool cardsChanged = panelCardPad_ != p.soundCardPad || panelCardRadius_ != p.soundCardRadius ||
				  panelCardGap_ != p.soundCardGap;
	panelCardPad_ = p.soundCardPad;
	panelCardRadius_ = p.soundCardRadius;
	panelCardGap_ = p.soundCardGap;
	panelVoMargin_ = p.voPanelMargin;
	panelVoSpacing_ = p.voPanelSpacing;
	panelVoBtnH_ = p.voRecordBtnH;
	if (consoleOut_) {
		consoleOut_->setMinimumHeight(p.consoleMinH);
		QFont f = consoleOut_->font();
		f.setPixelSize(p.consoleFontPx > 0 ? p.consoleFontPx : std::max(11, uiReadoutPx() - 1));
		consoleOut_->setFont(f);
		if (consoleIn_)
			consoleIn_->setFont(f);
		if (consolePopup_)
			consolePopup_->setFont(f);
	}
	if (textEdit_)
		textEdit_->setFixedHeight(p.textBoxH);
	if (spotList_)
		spotList_->setFixedHeight(p.spotListH);
	if (voPanel_) {
		if (auto *l = voPanel_->layout()) {
			l->setContentsMargins(p.voPanelMargin, p.voPanelMargin - 2, p.voPanelMargin, p.voPanelMargin - 2);
			l->setSpacing(p.voPanelSpacing);
		}
		if (voRecordBtn_)
			voRecordBtn_->setMinimumHeight(p.voRecordBtnH);
	}
	if (cardsChanged && soundsList_) {
		if (auto *ll = soundsList_->layout())
			ll->setSpacing(p.soundCardGap);
		// Force the rule cards to be rebuilt with the new padding and radius.
		for (const SoundRow &row : soundRows_)
			if (row.on)
				row.on->parentWidget()->deleteLater();
		soundRows_.clear();
		rebuildSoundsTab();
	}
	syncClipTags();
}

void VideoEditorWindow::applyInspectorParams(const EditorInspectorParams &p)
{
	inspectorParams_ = p;
	if (!inspector_)
		return;
	inspector_->setMinimumWidth(std::max(120, p.minWidth));
	if (insContentLayout_) {
		insContentLayout_->setContentsMargins(p.margin, p.spacing, p.margin, p.margin);
		insContentLayout_->setSpacing(p.spacing);
	}
	// Every form inside the panel, wherever it was built — the clip transform,
	// the text style, the audio levels, the script parameters.
	for (QFormLayout *f : inspector_->findChildren<QFormLayout *>()) {
		f->setHorizontalSpacing(p.labelSpacing);
		f->setVerticalSpacing(p.rowSpacing);
	}
	if (scriptList_)
		scriptList_->setMaximumHeight(std::max(48, p.scriptListH));
	// If it is already open and now narrower than its own minimum, widen it.
	if (inspector_->isVisible() && hsplit_ && inspectorWidth() < p.minWidth)
		setInspectorWidth(p.minWidth);
}

void VideoEditorWindow::applyChrome(const EditorChromeParams &p)
{
	powerSaveEnabled_ = p.powerSaveOnBlur;
	if (!powerSaveEnabled_ && powerSaving_)
		setPowerSaving(false); // turned off while already saving: come back now
	speedStep_ = (p.speedStep > 0.0) ? p.speedStep : 0.2;

	if (p.buttonH > 0)
		for (QPushButton *b : uiButtons_)
			if (b)
				b->setFixedHeight(p.buttonH);

	if (cursorTimeLabel_)
		cursorTimeLabel_->setStyleSheet(
			QStringLiteral("color:#e8eaed; font-family:monospace; font-weight:bold; "
				       "font-size:%1px;")
				.arg(p.timecodeFontPx));

	const QString insStyle =
		QStringLiteral("color:#e8eaed; font-family:monospace; font-size:%1px;")
			.arg(p.inspectorFontPx);
	for (QLabel *l : {inspInMs_, inspOutMs_, inspSrcLen_, inspSpeed_, inspOutLen_})
		if (l)
			l->setStyleSheet(insStyle);

	if (speedSlider_) {
		speedSlider_->setMinimumWidth(p.speedSliderMinW);
		speedSlider_->setMaximumWidth(p.speedSliderMinW); // fixed, compact width
	}
	if (speedSpin_)
		speedSpin_->setFixedWidth(p.speedSpinW);
}

// ---------------------------------------------------------------------------
// Sources (multi-video mixing)
// ---------------------------------------------------------------------------

EditorSource *VideoEditorWindow::sourceById(int id)
{
	for (auto &s : sources_)
		if (s.id == id)
			return &s;
	return nullptr;
}

EditorSource *VideoEditorWindow::sourceByPath(const QString &path)
{
	if (path.isEmpty())
		return nullptr;
	const QString want = QFileInfo(path).absoluteFilePath();
	for (auto &s : sources_)
		if (QFileInfo(s.path).absoluteFilePath() == want)
			return &s;
	return nullptr;
}

EditorSource *VideoEditorWindow::activeSource()
{
	return sourceById(activeSourceId_);
}

FrameSeeker *VideoEditorWindow::seekerFor(int id)
{
	EditorSource *s = sourceById(id);
	return s ? s->seeker.get() : nullptr;
}

FrameSeeker *VideoEditorWindow::playSeekerFor(int sourceId, int track, int decoderSlot)
{
	// Track 0 keeps using the source's own seeker: the overwhelmingly common
	// single-video-track project then behaves exactly as it did, with no extra
	// decoder and no extra memory.
	if (track <= 0 && decoderSlot == 0)
		return seekerFor(sourceId);
	const quint64 key = playKey(sourceId, track * 2 + decoderSlot);
	if (const auto it = playSeekers_.find(key); it != playSeekers_.end())
		return it->second.get();
	const EditorSource *s = sourceById(sourceId);
	if (!s)
		return nullptr;
	auto sk = std::make_unique<FrameSeeker>();
	// Whatever the source is PLAYING from, which is the proxy once one has been
	// built -- opening the 4K original here would undo the proxy's whole point
	// for every stack past the first.
	const QString path = playbackPath_.value(sourceId, s->path);
	if (!sk->open(path))
		return nullptr;
	FrameSeeker *raw = sk.get();
	playSeekers_.emplace(key, std::move(sk));
	return raw;
}

bool VideoEditorWindow::sourceInUse(int id) const
{
	for (const CutSegment &c : tracks_->segments())
		if (c.sourceId == id)
			return true;
	return false;
}

int VideoEditorWindow::addSource(const QString &path)
{
	LoadingScope loading(*this, QStringLiteral("Loading video"));
	loading.stage(QStringLiteral("Opening video…"), path);
	auto seeker = runLoadingTask([path]() {
		auto opened = std::make_unique<FrameSeeker>();
		if (!opened->open(path))
			opened.reset();
		return opened;
	});
	if (!seeker) {
		loading.hide();
		// While a project is being applied this is COLLECTED, not shown. A
		// message box runs a nested event loop, and doing that half-way through
		// rebuilding the editor -- once per missing file, so five missing files
		// meant five nested loops interleaved with the load -- is how a failed
		// load turns into something worse than a failed load. The caller
		// reports them together when the editor is whole again.
		if (loadingProject_)
			loadFailures_ << QFileInfo(path).fileName();
		else
			warnAndLog(
				this, QStringLiteral("Add video"),
				QStringLiteral("Could not open %1").arg(QFileInfo(path).fileName()));
		return -1;
	}
	EditorSource src;
	src.id = nextSourceId_++;
	src.path = path;
	src.name = QFileInfo(path).fileName();
	src.durationMs = seeker->durationMs();
	src.width = seeker->width();
	src.height = seeker->height();
	const QString codecName = seeker->codecName();
	src.seeker = std::move(seeker);
	const int id = src.id;
	if (previewDecoder_) {
		previewDecoder_->setSource(id, path);
		// Only files that actually scrub badly: above 1080p, or one of the
		// slow-to-seek codecs. An ordinary recording is left alone, because a
		// proxy for it would be a transcode that buys nothing.
		if (proxyBuilder_ && wantsProxy(src.width, src.height, codecName)) {
			proxyProgress_[id] = 0;
			proxyPending_.insert(id);
			proxyBuilder_->request(id, path);
			updateProxyStatus();
		}
	}
	auto *thumbs = new TimelineThumbs(this);
	src.thumbs = thumbs;
	sources_.push_back(std::move(src));
	// The filmstrip streams in on a worker thread; cache it per source and, if
	// this is the active source, push it live to the timeline/tracks widgets.
	connect(thumbs, &TimelineThumbs::updated, this, [this, id, thumbs]() {
		EditorSource *s = sourceById(id);
		if (!s)
			return;
		s->thumbCache = thumbs->thumbs();
		// Update just this source's row icon in place (rebuilding the whole list
		// on every streamed thumbnail, for every source, was needless churn).
		if (sourceList_ && !s->thumbCache.isEmpty() && !s->thumbCache.front().isNull()) {
			for (int i = 0; i < sourceList_->count(); ++i) {
				QListWidgetItem *it = sourceList_->item(i);
				if (it->data(Qt::UserRole).toInt() == id) {
					it->setIcon(QIcon(QPixmap::fromImage(s->thumbCache.front())));
					break;
				}
			}
		}
		// Every source feeds the Output track so its cuts render from their own
		// frames; the active source also drives the Source track + Simple-Trim.
		tracks_->setSourceThumbs(id, s->thumbCache, s->durationMs);
		if (timelineView_)
			timelineView_->setSourceThumbs(id, s->thumbCache, s->durationMs);
		if (id == activeSourceId_) {
			timeline_->setThumbs(s->thumbCache);
			tracks_->setThumbs(s->thumbCache);
		}
	});
	// The filmstrip is sixty evenly-spaced frames, and on a heavy source that is
	// sixty full-size seeks on a background thread while you are trying to work:
	// 1182 ms for an eight-second 4K VP9 clip against 70 ms from its proxy, and
	// the gap widens with length -- past a two-second spacing every thumbnail
	// stops being able to roll forward and becomes a whole-GOP seek of its own.
	// So when a proxy is coming, wait for it. Nothing is lost but the few
	// seconds before the strip appears, and the status line says why.
	if (!proxyPending_.contains(id))
		thumbs->start(path, 60, 128, 72);
	refreshSourceList();

	// The first video added to a blank editor BECOMES the launch file. One
	// assignment, and everything downstream behaves as if the editor had been
	// opened on it: the canvas size and frame rate already come from
	// sources_.front(), and inPath_ is what names the project, seeds the export
	// folder and titles the window.
	if (inPath_.isEmpty()) {
		inPath_ = path;
		setWindowTitle(QStringLiteral("Edit — %1").arg(QFileInfo(path).fileName()));
	}
	updateEmptyState();
	return id;
}

void VideoEditorWindow::setActiveSource(int id)
{
	EditorSource *s = sourceById(id);
	if (!s)
		return;
	stopPlayback();
	// A queued scrub still names the previous video. It must not repaint over
	// the new source after the dropdown (or an Output cut) switches videos.
	if (previewTimer_)
		previewTimer_->stop();
	pendingMs_ = -1;
	activeSourceId_ = id;
	seeker_ = s->seeker.get();
	if (multiCutSourceCombo_) {
		QSignalBlocker b(multiCutSourceCombo_);
		multiCutSourceCombo_->setCurrentIndex(multiCutSourceCombo_->findData(id));
	}
	tracks_->setActiveSource(id);
	// Simple Trim exports the active source at its own resolution, so the
	// preview is that source. Multi-Cut assembles everything into ONE output
	// sized from the primary, and re-shaping the canvas to whichever source was
	// last selected made the other clips draw stretched into it -- the preview
	// stopped describing the file that would come out.
	const QSize pv = previewCanvasSize();
	canvas_->setVideoSize(pv.width(), pv.height());
	timeline_->setDuration(s->durationMs);
	tracks_->setDuration(s->durationMs);
	timeline_->setThumbs(s->thumbCache);
	tracks_->setThumbs(s->thumbCache);
	baseInfo_ = QStringLiteral("%1 × %2   %3s")
			    .arg(s->width)
			    .arg(s->height)
			    .arg(s->durationMs / 1000.0, 0, 'f', 1);
	updateInfoLabel();
	// Reflect the selection in the sidebar without re-entering the slot.
	if (sourceList_) {
		QSignalBlocker b(sourceList_);
		for (int i = 0; i < sourceList_->count(); ++i)
			if (sourceList_->item(i)->data(Qt::UserRole).toInt() == id) {
				sourceList_->setCurrentRow(i);
				break;
			}
	}
	showFrame(id, 0);
}

void VideoEditorWindow::refreshMultiCutSources()
{
	if (!multiCutSourceCombo_)
		return;
	QSignalBlocker b(multiCutSourceCombo_);
	multiCutSourceCombo_->clear();
	for (const EditorSource &s : sources_) {
		if (!s.seeker)
			continue; // images and audio have no video timeline to cut from
		multiCutSourceCombo_->addItem(s.name, s.id);
		multiCutSourceCombo_->setItemData(multiCutSourceCombo_->count() - 1, s.path, Qt::ToolTipRole);
	}
	multiCutSourceCombo_->setCurrentIndex(multiCutSourceCombo_->findData(activeSourceId_));
	multiCutSourceCombo_->setEnabled(multiCutSourceCombo_->count() > 0);
}

void VideoEditorWindow::refreshSourceList()
{
	refreshMultiCutSources();
	if (!sourceList_)
		return;
	QSignalBlocker b(sourceList_); // rebuild must not fire selection changes
	sourceList_->clear();
	for (const EditorSource &s : sources_) {
		auto *item = new QListWidgetItem(
			QStringLiteral("%1\n%2s · %3×%4")
				.arg(s.name)
				.arg(s.durationMs / 1000.0, 0, 'f', 1)
				.arg(s.width)
				.arg(s.height));
		item->setData(Qt::UserRole, s.id);
		item->setData(UrlDragList::PathRole, s.path); // what a drag of this row carries
		if (!s.thumbCache.isEmpty() && !s.thumbCache.front().isNull())
			item->setIcon(QIcon(QPixmap::fromImage(s.thumbCache.front())));
		item->setToolTip(QStringLiteral("%1  ·  %2×%3  ·  %4s")
					 .arg(s.name)
					 .arg(s.width)
					 .arg(s.height)
					 .arg(s.durationMs / 1000.0, 0, 'f', 1));
		sourceList_->addItem(item);
		if (s.id == activeSourceId_)
			sourceList_->setCurrentItem(item);
	}
}

void VideoEditorWindow::onAddSource()
{
	RecordingVideoPicker picker(libraryFolders_, this);
	QStringList protectedPaths;
	for (const EditorSource &source : sources_) {
		protectedPaths << source.path;
		if (!source.origPath.isEmpty())
			protectedPaths << source.origPath;
	}
	picker.setProtectedPaths(protectedPaths);
	connect(&picker, &RecordingVideoPicker::recordingsDeleted, this, [this](const QStringList &paths) {
		refreshLibrary();
		emit recordingsDeleted(paths);
	});
	if (picker.exec() != QDialog::Accepted)
		return;
	QString path = picker.selectedVideo();
	if (picker.importFromFolderRequested()) {
		const QString dir = activeSource() ? QFileInfo(activeSource()->path).absolutePath() : QString();
		path = QFileDialog::getOpenFileName(
			this, QStringLiteral("Add video"), dir,
			QStringLiteral("Video files (*.mp4 *.mov *.mkv *.webm *.avi *.m4v *.gif *.wmv *.flv *.ts);;"
				       "All files (*)"));
	}
	if (path.isEmpty())
		return;
	const EditorSource *existing = sourceByPath(path);
	const int id = existing ? existing->id : addSource(path);
	if (id >= 0)
		setActiveSource(id);
}

void VideoEditorWindow::buildEmptyPanel()
{
	if (!canvas_)
		return;
	// A child of the preview, like the timeline overview above it: it floats
	// over the picture, takes no layout space, and is placed by hand whenever
	// the preview resizes.
	emptyPanel_ = new QWidget(canvas_);
	emptyPanel_->setObjectName(QStringLiteral("emptyProjectPanel"));
	emptyPanel_->setStyleSheet(QStringLiteral(
		"QWidget{background:rgba(24,27,33,220); border:1px solid #3a3f48; border-radius:8px;}"));
	auto *outer = new QVBoxLayout(emptyPanel_);
	outer->setContentsMargins(1, 1, 1, 1);
	auto *scroll = new QScrollArea(emptyPanel_);
	scroll->setObjectName(QStringLiteral("emptyProjectScroll"));
	scroll->setWidgetResizable(true);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setStyleSheet(QStringLiteral("QScrollArea{border:none; background:transparent;}"));
	emptyPanelContent_ = new QWidget;
	emptyPanelContent_->setStyleSheet(QStringLiteral("QWidget{border:none; background:transparent;}"));
	scroll->setWidget(emptyPanelContent_);
	outer->addWidget(scroll);
	auto *lay = new QVBoxLayout(emptyPanelContent_);
	lay->setContentsMargins(24, 20, 24, 20);
	lay->setSpacing(10);

	auto *title = new QLabel(QStringLiteral("<b>Nothing loaded yet</b>"), emptyPanel_);
	title->setAlignment(Qt::AlignCenter);
	title->setStyleSheet(QStringLiteral("border:none; background:transparent; color:#e6e6e6;"));
	lay->addWidget(title);

	auto *hint = new QLabel(QStringLiteral("Add a video, load a project, or drop a file here."),
				emptyPanel_);
	hint->setAlignment(Qt::AlignCenter);
	hint->setStyleSheet(QStringLiteral("border:none; background:transparent; color:#8a8f98;"));
	lay->addWidget(hint);

	auto *row = new QHBoxLayout;
	row->setSpacing(8);
	auto *addBtn = new QPushButton(QStringLiteral("Add video…"), emptyPanel_);
	addBtn->setStyleSheet(QStringLiteral(
		"QPushButton{background:#3d84b8; color:white; border:none; border-radius:4px; padding:6px 14px;}"
		"QPushButton:hover{background:#4a97cf;}"));
	connect(addBtn, &QPushButton::clicked, this, &VideoEditorWindow::onAddSource);
	auto *projBtn = new QPushButton(QStringLiteral("Load project…"), emptyPanel_);
	projBtn->setStyleSheet(QStringLiteral(
		"QPushButton{background:#181b21; color:#e6e6e6; border:1px solid #3a3f48;"
		"border-radius:4px; padding:6px 14px;}"
		"QPushButton:hover{background:#2b313b;}"));
	connect(projBtn, &QPushButton::clicked, this, &VideoEditorWindow::onOpenProject);
	row->addStretch(1);
	row->addWidget(addBtn);
	row->addWidget(projBtn);
	row->addStretch(1);
	lay->addLayout(row);

	recentProjectsPanel_ = new QWidget(emptyPanel_);
	recentProjectsPanel_->setObjectName(QStringLiteral("recentProjectsPanel"));
	recentProjectsPanel_->setStyleSheet(QStringLiteral("QWidget{border:none; background:transparent;}"));
	auto *recent = new QVBoxLayout(recentProjectsPanel_);
	recent->setContentsMargins(0, 8, 0, 0);
	recent->setSpacing(6);
	auto *recentTitle = new QLabel(QStringLiteral("Recent projects"), recentProjectsPanel_);
	recentTitle->setStyleSheet(QStringLiteral("color:#8a8f98;"));
	recent->addWidget(recentTitle);
	for (int i = 0; i < 3; ++i) {
		auto *button = new QPushButton(recentProjectsPanel_);
		button->setObjectName(QStringLiteral("recentProject%1").arg(i + 1));
		button->setAutoDefault(false);
		button->setCursor(Qt::PointingHandCursor);
		button->setStyleSheet(QStringLiteral(
			"QPushButton{background:#22262d; color:#dce2eb; border:1px solid #3a3f48;"
			"border-radius:4px; padding:6px 10px; text-align:left;}"
			"QPushButton:hover{background:#303946; border-color:#5286b2;}"));
		connect(button, &QPushButton::clicked, this, [this, button]() {
			// Keep the normal open path's validation and missing-media relinking,
			// without a confirmation box after a successful one-click reopen.
			const QString path = button->property("projectPath").toString();
			if (!path.isEmpty())
				openProjectWithDialogs(path, /*showSuccess=*/false);
			refreshRecentProjects();
		});
		recentProjectButtons_.append(button);
		recent->addWidget(button);
	}
	lay->addWidget(recentProjectsPanel_);
	refreshRecentProjects();
	emptyPanel_->hide();
}

void VideoEditorWindow::refreshRecentProjects()
{
	if (!recentProjectsPanel_)
		return;
	const QStringList paths = recentProjects();
	for (int i = 0; i < recentProjectButtons_.size(); ++i) {
		QPushButton *button = recentProjectButtons_[i];
		const QString path = paths.value(i);
		button->setProperty("projectPath", path);
		button->setVisible(!path.isEmpty());
		if (path.isEmpty())
			continue;
		const QFileInfo info(path);
		// Bound both lines so a long filename or directory cannot widen the
		// opening panel beyond the preview. The tooltip keeps the full path.
		const auto elide = [button](const QString &text) {
			return button->fontMetrics().elidedText(text, Qt::ElideMiddle, 280);
		};
		const QString name = elide(info.completeBaseName()).replace(QLatin1Char('&'), QStringLiteral("&&"));
		const QString folder = elide(QDir::toNativeSeparators(info.absolutePath()))
					       .replace(QLatin1Char('&'), QStringLiteral("&&"));
		button->setText(name + QLatin1Char('\n') + folder);
		button->setToolTip(QDir::toNativeSeparators(path));
		button->setAccessibleName(info.completeBaseName());
	}
	recentProjectsPanel_->setVisible(!paths.isEmpty());
	layOutEmptyPanel();
}

void VideoEditorWindow::layOutEmptyPanel()
{
	if (!emptyPanel_ || !canvas_ || !emptyPanel_->isVisible())
		return;
	const QSize hint = emptyPanelContent_->sizeHint() + QSize(2, 2);
	const int h = std::min(hint.height(), std::max(1, canvas_->height() - 24));
	const int scrollWidth = h < hint.height() ? style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 0;
	const int w = std::min(hint.width() + scrollWidth, std::max(1, canvas_->width() - 24));
	emptyPanel_->setGeometry((canvas_->width() - w) / 2, (canvas_->height() - h) / 2, w, h);
}

// Is there NOTHING in this project? Not "is the media pool empty" -- that was
// the old test, and it is wrong for everything the editor can make on its own.
//
// A caption, a still, an effect clip: none of them adds a source, so a project
// with a text clip on the timeline reported itself empty and left "Nothing
// loaded yet" floating over the caption it was busy rendering.
bool VideoEditorWindow::projectIsEmpty() const
{
	if (!sources_.empty())
		return false;
	if (timelineView_)
		for (const TlTrack &t : timelineView_->model().tracks)
			if (!t.clips.isEmpty())
				return false;
	return true;
}

void VideoEditorWindow::updateEmptyState()
{
	const bool empty = projectIsEmpty();
	if (empty)
		refreshRecentProjects();
	if (emptyPanel_) {
		emptyPanel_->setVisible(empty);
		layOutEmptyPanel();
	}
	// Everything that needs a video to act on. The two ways OUT of the empty
	// state -- adding a source and loading a project -- stay live, as does the
	// shortcut panel, which is a reference rather than an action.
	for (QWidget *w : {static_cast<QWidget *>(playBtn_), static_cast<QWidget *>(speedSlider_),
			   static_cast<QWidget *>(speedSpin_), static_cast<QWidget *>(cropToggle_),
			   static_cast<QWidget *>(trimModeBtn_), static_cast<QWidget *>(cutModeBtn_),
			   static_cast<QWidget *>(voImportBtn_)})
		if (w)
			w->setEnabled(!empty);
	if (empty && infoLabel_)
		infoLabel_->setText(QStringLiteral("Empty project"));
}

void VideoEditorWindow::onSourceRowChanged()
{
	if (!sourceList_ || !sourceList_->currentItem())
		return;
	const int id = sourceList_->currentItem()->data(Qt::UserRole).toInt();
	if (id != activeSourceId_)
		setActiveSource(id);
}

void VideoEditorWindow::onRemoveSource()
{
	if (!sourceList_ || !sourceList_->currentItem())
		return;
	const int id = sourceList_->currentItem()->data(Qt::UserRole).toInt();
	if (sources_.size() <= 1) {
		QMessageBox::information(this, QStringLiteral("Remove source"),
					 QStringLiteral("The editor needs at least one source."));
		return;
	}
	if (sourceInUse(id)) {
		warnAndLog(
			this, QStringLiteral("Remove source"),
			QStringLiteral("Some cuts still use this video. Delete those cuts first."));
		return;
	}
	for (auto it = sources_.begin(); it != sources_.end(); ++it) {
		if (it->id == id) {
			if (it->thumbs)
				it->thumbs->deleteLater();
			if (previewDecoder_)
				previewDecoder_->removeSource(id);
			proxyProgress_.remove(id);
			proxyPending_.remove(id);
			proxied_.remove(id);
			playbackPath_.remove(id);
			// The per-track playback decoders for this source outlive the
			// source itself otherwise -- they hold their own open file, so
			// nothing else would ever close it.
			for (auto pit = playSeekers_.begin(); pit != playSeekers_.end();) {
				if (int(pit->first >> 32) == id)
					pit = playSeekers_.erase(pit);
				else
					++pit;
			}
			sources_.erase(it);
			invalidatePreviewCache();
			break;
		}
	}
	refreshSourceList(); // rebuild the sidebar without the removed row
	if (activeSourceId_ == id) {
		// Fall back to another source -- if there IS one. front() on an empty
		// vector is undefined behaviour, and removing the LAST source is not an
		// exotic case: it is what you do the moment a project comes back with
		// its media missing.
		if (!sources_.empty()) {
			setActiveSource(sources_.front().id);
		} else {
			activeSourceId_ = 0;
			// Its decoder went with the source: nothing may read it now.
			seeker_ = nullptr;
			updateEmptyState(); // back to "Nothing loaded yet", correctly this time
		}
	}
}

void VideoEditorWindow::openRecordingLibrary()
{
	if (!fullEdit() || !sourcesTabs_ || !libraryList_)
		return;
	refreshLibrary(); // include recordings made since the editor was opened
	sourcesTabs_->setCurrentWidget(libraryList_->parentWidget());
	sourcesBtn_->setChecked(true);
	showSourcesPanel(); // also raise it when the Sources toggle was already checked
	libraryList_->setFocus(Qt::OtherFocusReason);
}

void VideoEditorWindow::showSourceFileMenu(QListWidget *list, const QPoint &pos)
{
	const QListWidgetItem *item = list ? list->itemAt(pos) : nullptr;
	if (!item)
		return;
	QString path = item->data(UrlDragList::PathRole).toString();
	if (list == sourceList_)
		if (const EditorSource *source = sourceById(item->data(Qt::UserRole).toInt()))
			path = source->origPath.isEmpty() ? source->path : source->origPath;
	if (path.isEmpty())
		return;
	// Copy the clicked path before the menu's event loop; selection or a
	// refreshed Library list must never redirect an action to another video.
	const QFileInfo file(path);
	const QString folder = file.absolutePath();
	QMenu menu(sourcesPanel_);
	menu.setObjectName(QStringLiteral("editorMediaFileMenu"));
	const bool audio = ClipLibrary::isAudioPath(path) || isAudioFile(path);
	const bool image = isImageFile(path) && !isVideoFile(path);
	QAction *play = menu.addAction(audio ? QStringLiteral("Play audio") : image ? QStringLiteral("Open image")
										 : QStringLiteral("Play video"));
	play->setObjectName(QStringLiteral("mediaPlayVideo"));
	play->setEnabled(file.isFile());
	QAction *openFolder = menu.addAction(QStringLiteral("Open folder"));
	openFolder->setObjectName(QStringLiteral("mediaOpenFolder"));
	openFolder->setEnabled(QFileInfo(folder).isDir());
	QAction *copyFolder = menu.addAction(QStringLiteral("Copy folder path"));
	copyFolder->setObjectName(QStringLiteral("mediaCopyFolderPath"));
	menu.addSeparator();
	QAction *details = menu.addAction(QStringLiteral("Details…"));
	details->setObjectName(QStringLiteral("mediaFileDetails"));
	QAction *chosen = menu.exec(list->viewport()->mapToGlobal(pos));
	if (chosen == copyFolder) {
		QApplication::clipboard()->setText(QDir::toNativeSeparators(folder));
	} else if (chosen == details) {
		MediaFileDetailsDialog dialog(file.absoluteFilePath(), sourcesPanel_);
		dialog.exec();
	} else if (chosen == play || chosen == openFolder) {
		const QString target = chosen == play ? file.absoluteFilePath() : folder;
		if (!QDesktopServices::openUrl(QUrl::fromLocalFile(target)))
			warnAndLog(sourcesPanel_, QStringLiteral("Open file"),
				   chosen == play ? QStringLiteral("Could not open this file in its default application.")
						  : QStringLiteral("Could not open the recording's folder."));
	}
}

void VideoEditorWindow::refreshLibrary()
{
	if (!libraryList_)
		return;
	libraryList_->clear();
	const QSize thumbSz(128, 72); // matches the list's icon size
	const QVector<ClipInfo> clips = ClipLibrary::scan(libraryFolders_);
	for (const ClipInfo &c : clips) {
		auto *item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(c.fileName, c.relativeAge()));
		item->setData(Qt::UserRole, c.filePath);
		item->setData(UrlDragList::PathRole, c.filePath);
		item->setToolTip(c.filePath);
		const QImage img = thumbCache_ ? thumbCache_->cached(c.filePath, thumbSz) : QImage();
		if (!img.isNull())
			item->setIcon(QIcon(QPixmap::fromImage(img)));
		else if (thumbCache_)
			thumbCache_->ensure(c.filePath, thumbSz); // decodes async → onThumbReady
		libraryList_->addItem(item);
	}
}

void VideoEditorWindow::onThumbReady(const QString &path)
{
	if (!libraryList_ || !thumbCache_)
		return;
	const QImage img = thumbCache_->cached(path, QSize(128, 72));
	if (img.isNull())
		return;
	const QIcon icon(QPixmap::fromImage(img));
	for (int i = 0; i < libraryList_->count(); ++i) {
		QListWidgetItem *it = libraryList_->item(i);
		if (it->data(Qt::UserRole).toString() == path)
			it->setIcon(icon);
	}
}

void VideoEditorWindow::onLibraryDoubleClicked(QListWidgetItem *item)
{
	if (!item)
		return;
	const QString path = item->data(Qt::UserRole).toString();
	if (path.isEmpty())
		return;
	// Already loaded as a source? Just make it active. Otherwise add it. In
	// Full editing it also goes onto the timeline at the playhead, which is
	// what double-clicking a clip in a bin means in every editor.
	const EditorSource *es = sourceByPath(path);
	int id = es ? es->id : -1;
	if (id < 0)
		id = isImageFile(path) && !isVideoFile(path) ? addImageSource(path) : addSource(path);
	if (id < 0)
		return;
	setActiveSource(id);
	if (fullEdit() && timelineView_) {
		if (QListWidgetItem *row = sourceRowForId(id))
			onSourceDoubleClicked(row);
	}
}

QListWidgetItem *VideoEditorWindow::sourceRowForId(int id) const
{
	if (!sourceList_)
		return nullptr;
	for (int i = 0; i < sourceList_->count(); ++i)
		if (sourceList_->item(i)->data(Qt::UserRole).toInt() == id)
			return sourceList_->item(i);
	return nullptr;
}

void VideoEditorWindow::onSourceDoubleClicked(QListWidgetItem *item)
{
	if (!item)
		return;
	const int id = item->data(Qt::UserRole).toInt();
	EditorSource *s = sourceById(id);
	if (!s || s->durationMs <= 0)
		return;
	// In Full editing, put the whole clip on the timeline AT THE PLAYHEAD. It
	// used to go after the last clip, which on any real edit is off the right
	// edge of the view -- the clip arrived and nothing visibly happened, which
	// reads as "double-click does nothing". Otherwise drop it onto the
	// Multi-Cut Output track as one cut.
	if (fullEdit()) {
		TlClip c;
		c.type = stillImages_.contains(id) ? TlClip::Type::Image : TlClip::Type::Video;
		c.sourceId = id;
		c.srcStartMs = 0;
		c.srcEndMs = s->durationMs;
		c.outStartMs = timelinePlayheadMs();
		timelineView_->addClip(TlTrack::Kind::Video, c);
		updateInfoLabel();
		showTimelineFrame(timelinePlayheadMs());
		return;
	}
	if (!multiCut())
		setEditMode(EditMode::MultiCut);
	CutSegment c;
	c.srcStartMs = 0;
	c.srcEndMs = s->durationMs;
	c.speed = 1.0;
	c.sourceId = id;
	tracks_->addSegments({c});
	updateInfoLabel();
	updateInspector();
	scheduleSnapshot();
}

// ---- Still images ---------------------------------------------------------

// The reader itself moved to StillImage.cpp, so the export worker can use the
// same one; these stay as the window's published entry points.
QString VideoEditorWindow::imageOpenFilter()
{
	return harpia::imageOpenFilter();
}

QImage VideoEditorWindow::readStillImage(const QString &path, QString *why)
{
	return harpia::readStillImage(path, why);
}

QSize VideoEditorWindow::stillSize(int sourceId) const
{
	if (const auto it = stillSizes_.constFind(sourceId); it != stillSizes_.constEnd())
		return it.value();
	if (const auto it = stillImages_.constFind(sourceId); it != stillImages_.constEnd())
		return it.value().size();
	return QSize();
}

const QImage &VideoEditorWindow::stillForPreview(int sourceId, QSize render)
{
	// One copy per still per preview size. A new size (quality change, window
	// resize past a 64 px step) remakes them lazily, as each is next drawn.
	if (render != stillPreviewFor_) {
		stillPreview_.clear();
		stillPreviewFor_ = render;
	}
	auto it = stillPreview_.find(sourceId);
	if (it != stillPreview_.end())
		return it.value();
	const QImage &work = stillImages_[sourceId];
	const QSize want = stillPreviewSize(work.size(), render);
	QImage copy = want == work.size() ? work
					  : work.scaled(want, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	return stillPreview_.insert(sourceId, copy).value();
}

void VideoEditorWindow::warnHeavyStill(const QString &path, QSize px, qint64 bytes)
{
	const StillWeight w = stillWeight(px, bytes);
	if (!w.heavy)
		return;
	const QString name = QFileInfo(path).fileName();
	editorLog(EditorLogLevel::Warning, QStringLiteral("Add image"),
		  QStringLiteral("%1 is a very large image: %2. The editor works on a smaller copy; "
				 "the export still reads it whole.")
			  .arg(name, w.why));
	// Not while a project opens (a dialog per image, before the window is
	// even usable), and not again once the user said so.
	QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	if (loadingProject_ || st.value(QStringLiteral("editor/quietLargeImages"), false).toBool())
		return;
	QMessageBox box(QMessageBox::Warning, QStringLiteral("Very large image"),
			QStringLiteral("<b>%1</b> is %2.").arg(name.toHtmlEscaped(), w.why.toHtmlEscaped()),
			QMessageBox::Ok, this);
	box.setInformativeText(QStringLiteral(
		"Images this big can make the preview lag and the export slow, and use a lot of memory. "
		"The editor now previews a smaller copy (up to %1 px), so editing should stay smooth; "
		"the export still uses the full image.\n\n"
		"For the lightest project, resize it before adding it: 3840 x 2160 (4K) is plenty "
		"for a 1080p or 4K video. If the preview is still slow, set the "
		"\u201cPreview\u201d box in the top bar to \u00BD or \u00BC.")
				       .arg(kStillWorkingMax));
	auto *quiet = new QCheckBox(QStringLiteral("Don't warn me about large images again"), &box);
	box.setCheckBox(quiet);
	box.exec();
	if (quiet->isChecked())
		st.setValue(QStringLiteral("editor/quietLargeImages"), true);
}

int VideoEditorWindow::addImageSource(const QString &path)
{
	LoadingScope loading(*this, QStringLiteral("Loading image"));
	loading.stage(QStringLiteral("Reading image…"), path);
	struct PreparedImage { QImage pixels; QSize fullSize; QString error; };
	PreparedImage prepared = runLoadingTask([path]() {
		PreparedImage result;
		result.pixels = harpia::readStillImage(path, &result.error);
		result.fullSize = result.pixels.size();
		if (!result.pixels.isNull()) {
			const QSize working = stillWorkingSize(result.fullSize);
			if (working != result.fullSize)
				result.pixels = result.pixels.scaled(working, Qt::IgnoreAspectRatio,
								   Qt::SmoothTransformation);
			result.pixels = result.pixels.convertToFormat(result.pixels.hasAlphaChannel()
				? QImage::Format_RGBA8888 : QImage::Format_RGBX8888);
		}
		return result;
	});
	QImage img = std::move(prepared.pixels);
	const QString why = prepared.error;
	if (img.isNull()) {
		loading.hide();
		// The reason, not just the fact. "Could not read that image." left the
		// user with nowhere to go -- most often it was a WebP and a Qt build
		// with no WebP plugin, which is not something anyone could guess.
		//
		// Collected rather than shown while a project is loading, like the
		// other two: the reason still reaches the user, once, at the end.
		if (loadingProject_)
			loadFailures_ << QStringLiteral("%1 — %2").arg(QFileInfo(path).fileName(), why);
		else
			warnAndLog(this, QStringLiteral("Add image"), why);
		return -1;
	}
	// The original's size is what every piece of geometry is measured in; the
	// pixels kept are a working copy capped at kStillWorkingMax (the export
	// re-reads the file whole). See StillWeight.hpp.
	const QSize fullSize = prepared.fullSize;
	const qint64 fileBytes = QFileInfo(path).size();
	EditorSource s;
	s.id = nextSourceId_++;
	s.path = path;
	s.name = QFileInfo(path).fileName();
	s.durationMs = 5000; // a still has no length of its own; 5s is the default
	s.width = fullSize.width();
	s.height = fullSize.height();
	stillImages_.insert(s.id, img);
	stillSizes_.insert(s.id, fullSize);
	invalidatePreviewCache();
	stillPreview_.remove(s.id);
	loading.hide();
	warnHeavyStill(path, fullSize, fileBytes);
	// A still's filmstrip: one tile, at the same 128x72 bound the video strips
	// use. That single tile is what lets the timeline draw an image clip with
	// its own picture, the way a video clip is drawn with its frames -- an image
	// clip was previously a flat rectangle with "IMG #3" on it, which told you
	// there was a picture but not which one, and stills are usually chosen by
	// looking rather than by name.
	//
	// Scaled here rather than on a worker: a still is already decoded, and one
	// smooth scale of an image in memory is not work worth a thread.
	s.thumbCache = {img.scaled(128, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation)};
	const int id = s.id;
	const QVector<QImage> strip = s.thumbCache;
	sources_.push_back(std::move(s));
	// Duration 0: a still has no source timeline to space tiles along, and the
	// timeline's image path repeats the one tile instead of asking. Nothing
	// consults srcThumbDur_ for a still -- its clip is freeDuration(), so a trim
	// is never capped by a decoder's length.
	if (timelineView_)
		timelineView_->setSourceThumbs(id, strip, 0);
	refreshSourceList();
	return id;
}

void VideoEditorWindow::addImageClip()
{
	if (!fullEdit())
		setEditMode(EditMode::Full); // an image goes on the timeline, which only Full editing has
	const QString f = QFileDialog::getOpenFileName(
		this, QStringLiteral("Add image"), QFileInfo(inPath_).absolutePath(),
		imageOpenFilter());
	if (f.isEmpty())
		return;
	const int id = addImageSource(f);
	if (id < 0)
		return;
	if (!fullEdit())
		setEditMode(EditMode::Full);
	TlClip c;
	c.type = TlClip::Type::Image;
	c.sourceId = id;
	c.srcStartMs = 0;
	c.srcEndMs = 5000; // freely stretchable, like a caption
	c.outStartMs = timelinePlayheadMs();
	timelineView_->addClipOnFreeLane(TlTrack::Kind::Video, c);
	updateInfoLabel();
	showTimelineFrame(timelinePlayheadMs());
}

// Files dragged from the desktop straight onto the lanes. The view worked out
// WHERE; this works out what, and puts them there.
//
// Several files dropped together are laid end to end from the drop point rather
// than stacked on top of each other -- dropping a folder of stills should give a
// sequence, not one clip with four hidden underneath it.
void VideoEditorWindow::onFilesDroppedOnTimeline(const QStringList &paths, int track,
						 int newTrackAt, qint64 outMs)
{
	if (!timelineView_ || paths.isEmpty())
		return;
	if (!fullEdit())
		setEditMode(EditMode::Full);

	// Picture and sound get their OWN running positions, both starting at the
	// drop point. Dropping a video and its music track together then puts the
	// music UNDER the video, which is what that gesture means -- a single
	// running position would have queued the music after the picture ended.
	// A drag of nothing but audio was aimed at the audio lanes by the view, so
	// its track/newTrackAt can be taken at face value. See the audio branch.
	const bool audioDrag = TimelineView::allAudio(paths);
	qint64 atPic = std::max<qint64>(0, outMs);
	qint64 atAud = atPic;
	int landed = -1;    // the picture lane the first clip made or joined
	int audLanded = -1; // ...and the audio one
	bool any = false;
	LoadingScope loading(*this, QStringLiteral("Adding media"));
	QScopedValueRollback<int> totalLoading(loadingTotal_, int(paths.size()));
	QScopedValueRollback<int> completedLoading(loadingCompleted_, 0);
	for (int fileIndex = 0; fileIndex < paths.size(); ++fileIndex) {
		loadingCompleted_ = fileIndex;
		const QString f = paths[fileIndex];
		bool audio = false;
		const std::optional<TlClip> made = mediaClipForFile(f, &audio);
		if (!made)
			continue; // mediaClipForFile already said why
		TlClip c = *made;
		if (audio) {
			c.outStartMs = atAud;
			atAud += std::max<qint64>(1, c.outDurationMs());
			// The drop's target lane is only usable when the WHOLE drag was
			// audio: that is the case the view computed against the audio
			// group. In a mixed drag it named a picture lane, and inserting an
			// audio track at a picture position would break the
			// video-then-audio ordering the compositor relies on -- so those
			// fall back to addClipAt's own "last audio lane, or make one".
			const bool aimed = audioDrag && audLanded < 0;
			audLanded = timelineView_->addClipAt(TlTrack::Kind::Audio, c,
							     audLanded >= 0 ? audLanded
									    : (aimed ? track : -1),
							     aimed ? newTrackAt : -1);
			any = true;
			continue;
		}
		c.outStartMs = atPic;
		atPic += std::max<qint64>(1, c.outDurationMs());
		// Only the FIRST clip makes a track; the rest join it, or the drop of
		// three files would leave three new lanes.
		landed = timelineView_->addClipAt(TlTrack::Kind::Video, c, landed >= 0 ? landed : track,
						  landed >= 0 ? -1 : newTrackAt);
		any = true;
	}
	if (!any)
		return;
	updateInfoLabel();
	showTimelineFrame(timelinePlayheadMs());
}

std::optional<TlClip> VideoEditorWindow::mediaClipForFile(const QString &f, bool *isAudio)
{
	if (isAudio)
		*isAudio = false;
	TlClip c;
	// A GIF is both, and the video reader is what people mean by one -- so
	// only the audio test goes ahead of the video one, matching MediaFiles.hpp.
	if (isAudioFile(f)) {
		// An audio-only file has no video stream for FrameSeeker to open, so
		// this decodes it to a session WAV and registers THAT. Before this it
		// fell through to the image branch and produced "could not be
		// decoded", which was true of the still reader and useless. A file
		// already loaded (a row dragged from the Sources panel, the same file
		// pasted twice) is reused rather than decoded a second time.
		const EditorSource *have = sourceByPath(f);
		const int id = have ? have->id : addAudioSource(f);
		if (id < 0)
			return std::nullopt; // addAudioSource already said why
		const EditorSource *src = sourceById(id);
		const qint64 duration = src ? src->durationMs : 0;
		const QString wav = audioProxyFor(id);
		c.type = TlClip::Type::Video; // "media clip"; the LANE makes it audio
		c.sourceId = id;
		c.srcStartMs = 0;
		c.srcEndMs = duration;
		LoadingScope loading(*this, QStringLiteral("Loading audio"));
		loading.stage(QStringLiteral("Building audio waveforms…"), f);
		c.peaks = runLoadingTask([wav]() { return VoiceoverTrack::loadPeaks(wav, 600); });
		if (isAudio)
			*isAudio = true;
		return c;
	}
	if (isVideoFile(f)) {
		const EditorSource *have = sourceByPath(f);
		const int id = have ? have->id : addSource(f);
		if (id < 0)
			return std::nullopt; // addSource already said why
		const EditorSource *src = sourceById(id);
		c.type = TlClip::Type::Video;
		c.sourceId = id;
		c.srcStartMs = 0;
		c.srcEndMs = src ? src->durationMs : 0;
		return c;
	}
	const EditorSource *have = sourceByPath(f);
	const int id = have ? have->id : addImageSource(f);
	if (id < 0)
		return std::nullopt;
	c.type = TlClip::Type::Image;
	c.sourceId = id;
	c.srcStartMs = 0;
	c.srcEndMs = 5000; // a still has no length of its own
	return c;
}

bool VideoEditorWindow::pasteMediaFiles(const QStringList &paths, qint64 atMs)
{
	if (deferUntilLoaded([this, paths, atMs]() { pasteMediaFiles(paths, atMs); }))
		return true;
	if (!timelineView_ || paths.isEmpty())
		return false;
	if (!fullEdit())
		setEditMode(EditMode::Full);
	// Pictures and sound each run on from the paste point, as a drop does: a
	// video and its music pasted together put the music UNDER the video.
	qint64 atPic = std::max<qint64>(0, atMs);
	qint64 atAud = atPic;
	QVector<TlClip> pictures, sounds;
	LoadingScope loading(*this, QStringLiteral("Adding media"));
	QScopedValueRollback<int> totalLoading(loadingTotal_, int(paths.size()));
	QScopedValueRollback<int> completedLoading(loadingCompleted_, 0);
	for (int fileIndex = 0; fileIndex < paths.size(); ++fileIndex) {
		loadingCompleted_ = fileIndex;
		const QString f = paths[fileIndex];
		bool audio = false;
		std::optional<TlClip> c = mediaClipForFile(f, &audio);
		if (!c)
			continue; // mediaClipForFile already said why
		qint64 &at = audio ? atAud : atPic;
		c->outStartMs = at;
		at += std::max<qint64>(1, c->outDurationMs());
		(audio ? sounds : pictures).append(*c);
	}
	if (pictures.isEmpty() && sounds.isEmpty())
		return false;
	timelineView_->placePasted({{TlTrack::Kind::Video, pictures}, {TlTrack::Kind::Audio, sounds}});
	updateInfoLabel();
	syncPreviewTransformTarget();
	syncClipInspector();
	showTimelineFrame(timelinePlayheadMs());
	return true;
}

// Drop an effect clip at the playhead, on its own Effect track. An effect
// grades everything composited BELOW its track, so a fresh one goes on top of
// the picture stack where it covers the whole composition.
void VideoEditorWindow::addEffectClip()
{
	if (!timelineView_)
		return;
	if (!fullEdit())
		setEditMode(EditMode::Full);
	TlClip c;
	c.type = TlClip::Type::Effect;
	c.srcStartMs = 0;
	c.srcEndMs = 3000; // freely stretchable, like a caption
	c.outStartMs = timelinePlayheadMs();
	c.fx.type = FxType::Brightness;
	c.fx.params = fxDefaults(c.fx.type);
	c.components.append(TlClip::newEffectComponent(effectComponentId(c.fx.type),
						       fxDefaults(c.fx.type)));
	timelineView_->addClip(TlTrack::Kind::Effect, c);
	updateInfoLabel();
	showTimelineFrame(timelinePlayheadMs());
	revealInspector();
}

// ---- Audio on the timeline -----------------------------------------------

QString VideoEditorWindow::sessionAudioDir()
{
	// Reuse the voiceover session dir: same lifetime, cleaned up on close.
	return voiceoverTempDir();
}

QString VideoEditorWindow::audioProxyFor(int sourceId)
{
	if (const auto it = audioProxy_.constFind(sourceId); it != audioProxy_.constEnd())
		return it.value();
	QString wav;
	if (EditorSource *s = sourceById(sourceId)) {
		const QString dir = sessionAudioDir();
		if (!dir.isEmpty()) {
			const QString cand =
				dir + QStringLiteral("/proxy_%1.wav").arg(sourceId);
			const QString sourcePath = s->path;
			LoadingScope loading(*this, QStringLiteral("Loading audio"));
			loading.stage(QStringLiteral("Extracting audio…"), sourcePath);
			const bool ok = runLoadingTask([sourcePath, cand]() {
				return VoiceoverMixer::decodeToWav(sourcePath, cand);
			});
			if (ok)
				wav = cand;
		}
	}
	audioProxy_.insert(sourceId, wav);
	return wav;
}

int VideoEditorWindow::addAudioSource(const QString &path)
{
	// Audio-only files have no video stream, so FrameSeeker can't open them.
	// Decode to a session WAV and register that as the source: the waveform,
	// duration and the export mix all read it uniformly.
	const QString dir = sessionAudioDir();
	if (dir.isEmpty())
		return -1;
	const int id = nextSourceId_++;
	const QString wav = dir + QStringLiteral("/import_%1.wav").arg(id);
	LoadingScope loading(*this, QStringLiteral("Loading audio"));
	loading.stage(QStringLiteral("Preparing audio…"), path);
	const bool ok = runLoadingTask([path, wav]() {
		return VoiceoverMixer::decodeToWav(path, wav);
	});
	if (!ok) {
		loading.hide();
		QFile::remove(wav);
		// Same rule as addSource: while a project is being applied this is
		// collected and reported once at the end, not put up as a modal in the
		// middle of a half-rebuilt editor.
		if (loadingProject_)
			loadFailures_ << QFileInfo(path).fileName();
		else
			warnAndLog(
				this, QStringLiteral("Add audio"),
				QStringLiteral("Could not read any audio from that file."));
		return -1;
	}
	EditorSource s;
	s.id = id;
	s.path = wav; // the decoded proxy IS the source (export decodes it again)
	s.origPath = path; // ...but the project remembers the file you chose
	s.name = QFileInfo(path).fileName();
	s.durationMs = VoiceoverTrack::wavDurationMs(wav);
	s.width = s.height = 0; // audio-only: no seeker, no filmstrip
	sources_.push_back(std::move(s));
	audioProxy_.insert(id, wav);
	refreshSourceList();
	return id;
}

void VideoEditorWindow::addAudioClipFromSource(int sourceId)
{
	if (deferUntilLoaded([this, sourceId]() { addAudioClipFromSource(sourceId); }))
		return;
	EditorSource *s = sourceById(sourceId);
	if (!s || !timelineView_)
		return;
	// Extract on a worker with the same loading feedback as an audio-file
	// import. Keep one scope through waveform preparation and clip insertion.
	const QString name = s->name;
	qint64 dur = s->durationMs;
	LoadingScope loading(*this, QStringLiteral("Loading audio"));
	const QString wav = audioProxyFor(sourceId);
	if (wav.isEmpty()) {
		loading.hide();
		QMessageBox::information(this, QStringLiteral("Add audio"),
					 QStringLiteral("\"%1\" has no audio track.").arg(name));
		return;
	}
	if (dur <= 0)
		dur = VoiceoverTrack::wavDurationMs(wav);
	if (dur <= 0)
		return;

	TlClip c;
	c.type = TlClip::Type::Video; // "media clip"; the track kind makes it audio
	c.sourceId = sourceId;
	c.srcStartMs = 0;
	c.srcEndMs = dur;
	c.outStartMs = timelinePlayheadMs();
	loading.stage(QStringLiteral("Building audio waveforms…"), wav);
	c.peaks = runLoadingTask([wav]() { return VoiceoverTrack::loadPeaks(wav, 600); });
	timelineView_->addClip(TlTrack::Kind::Audio, c);
	if (!fullEdit())
		setEditMode(EditMode::Full);
	updateInfoLabel();
	showTimelineFrame(timelinePlayheadMs());
}

// ---- sounds for events ---------------------------------------------------------

void VideoEditorWindow::applySoundRules()
{
	if (!timelineView_)
		return;
	const TimelineModel &cur = timelineView_->model();
	if (cur.soundRules.isEmpty() && cur.soundsLane() < 0) {
		// The common case, and the cheap one: no rules, no Sounds lane. Only a
		// Sound component, or a stray derived clip (a hand-edited file), would
		// need the full pass.
		bool stray = false;
		for (int ti = 0; ti < cur.tracks.size() && !stray; ++ti)
			for (const TlClip &c : cur.tracks[ti].clips) {
				if (c.soundRule != 0) {
					stray = true;
					break;
				}
				for (const ComponentInstance &ci : c.components)
					if (ci.typeId == QLatin1String(kSoundComponentId)) {
						stray = true;
						break;
					}
				if (stray)
					break;
			}
		if (!stray)
			return;
	}
	TimelineModel m = cur;
	if (SoundRules::apply(m, [this](int id) { return soundInfoFor(id); }))
		timelineView_->setModelQuiet(m);
}

SoundInfo VideoEditorWindow::soundInfoFor(int sourceId)
{
	SoundInfo si;
	EditorSource *s = sourceById(sourceId);
	if (!s)
		return si;
	const qint64 duration = s->durationMs;
	const QString wav = audioProxyFor(sourceId);
	if (wav.isEmpty())
		return si;
	si.durationMs = duration > 0 ? duration : VoiceoverTrack::wavDurationMs(wav);
	auto it = soundPeaks_.find(sourceId);
	if (it == soundPeaks_.end())
		it = soundPeaks_.insert(sourceId, VoiceoverTrack::loadPeaks(wav, 200));
	si.peaks = it.value();
	return si;
}

int VideoEditorWindow::librarySoundSource(const QString &slot)
{
	for (const EditorSource &es : sources_)
		if (es.librarySlot.compare(slot, Qt::CaseInsensitive) == 0)
			return es.id;
	const QVector<SoundSlot> lib = SoundLibrary::load();
	const QString file = SoundLibrary::resolve(lib, slot, SoundLibrary::defaultCacheDir());
	if (file.isEmpty())
		return -1;
	const int id = addAudioSource(file);
	if (id >= 0)
		if (EditorSource *es = sourceById(id)) {
			const SoundSlot *ss = SoundLibrary::find(lib, slot);
			es->librarySlot = ss ? ss->name : slot;
			es->name = es->librarySlot; // "Whoosh", not "whoosh.wav"
		}
	return id;
}

void VideoEditorWindow::refreshLibrarySources()
{
	const QVector<SoundSlot> lib = SoundLibrary::load();
	bool changed = false;
	for (EditorSource &es : sources_) {
		if (es.librarySlot.isEmpty())
			continue;
		const QString file = SoundLibrary::resolve(lib, es.librarySlot, SoundLibrary::defaultCacheDir());
		if (file.isEmpty() || QFileInfo(file).absoluteFilePath() == QFileInfo(es.origPath).absoluteFilePath())
			continue;
		const QString dir = sessionAudioDir();
		if (dir.isEmpty())
			continue;
		static int serial = 0;
		const QString wav = dir + QStringLiteral("/lib_%1_%2.wav").arg(es.id).arg(++serial);
		if (!VoiceoverMixer::decodeToWav(file, wav))
			continue;
		es.path = wav;
		es.origPath = file;
		es.durationMs = VoiceoverTrack::wavDurationMs(wav);
		audioProxy_.insert(es.id, wav);
		soundPeaks_.remove(es.id);
		changed = true;
	}
	if (!changed)
		return;
	// Every derived clip is rebuilt from the new lengths and waveforms, and
	// the mix follows. The source ids did not change, so every rule and every
	// Sound component already points at the new sound.
	applySoundRules();
	invalidateAudioMix();
	refreshSourceList();
	rebuildSoundsTab();
	if (fullEdit())
		requestPreview(-1, timelinePlayheadMs());
}

int VideoEditorWindow::pickSound(const QPoint &at, QString *name)
{
	QMenu menu(this);
	menu.setToolTipsVisible(true);
	// The library first: your files where you set them, the built-ins where not.
	const QVector<SoundSlot> lib = SoundLibrary::load();
	for (const SoundSlot &ss : lib) {
		const bool mine = !ss.file.isEmpty() && QFileInfo(ss.file).isFile();
		if (!ss.builtin && !mine)
			continue; // a custom slot with nothing to play
		QAction *a = menu.addAction(mine && ss.builtin ? QStringLiteral("%1  ★").arg(ss.name) : ss.name);
		a->setToolTip(mine ? QDir::toNativeSeparators(ss.file) : QStringLiteral("Built-in sound"));
		a->setData(QStringLiteral("l:") + ss.name);
	}
	// Then any other sound already in the project.
	bool sep = false;
	for (const EditorSource &es : sources_) {
		if (es.width != 0 || es.height != 0 || es.durationMs <= 0 || !es.librarySlot.isEmpty())
			continue;
		if (!sep) {
			menu.addSeparator();
			sep = true;
		}
		QAction *a = menu.addAction(es.name);
		a->setData(QStringLiteral("s:%1").arg(es.id));
	}
	menu.addSeparator();
	QAction *file = menu.addAction(QStringLiteral("Audio file…"));
	file->setToolTip(QStringLiteral("Any mp3, wav, ogg, m4a or flac on this computer."));
	QAction *edit = menu.addAction(QStringLiteral("Edit sound library…"));
	edit->setToolTip(QStringLiteral("Put your own files behind Whoosh, Pop, Click… or add sounds of your own."));
	QAction *chosen = menu.exec(at);
	if (!chosen)
		return -1;
	if (chosen == edit) {
		openSoundLibrary();
		return -1;
	}
	if (chosen == file) {
		const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Choose a sound"),
							       QFileInfo(inPath_).absolutePath(), audioOpenFilter());
		if (f.isEmpty())
			return -1;
		const int id = addAudioSource(f);
		if (id >= 0 && name)
			*name = QFileInfo(f).fileName();
		return id;
	}
	const QString d = chosen->data().toString();
	if (d.startsWith(QLatin1String("l:"))) {
		const QString n = d.mid(2);
		if (name)
			*name = n;
		return librarySoundSource(n);
	}
	if (d.startsWith(QLatin1String("s:"))) {
		const int id = d.mid(2).toInt();
		if (name)
			if (const EditorSource *es = sourceById(id))
				*name = es->name;
		return id;
	}
	return -1;
}

void VideoEditorWindow::openSoundLibrary()
{
	QDialog dlg(this);
	dlg.setWindowTitle(QStringLiteral("Sound library"));
	dlg.setMinimumWidth(560);
	auto *v = new QVBoxLayout(&dlg);
	auto *intro = new QLabel(
		QStringLiteral("The sounds every rule and Sound component picks from. Point a slot at a file "
			       "of your own and it changes everywhere that slot is used, in every project. "
			       "Reset goes back to the built-in sound."),
		&dlg);
	intro->setWordWrap(true);
	intro->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	v->addWidget(intro);
	auto *list = new QWidget(&dlg);
	auto *grid = new QGridLayout(list);
	grid->setContentsMargins(0, 6, 0, 6);
	grid->setHorizontalSpacing(8);
	grid->setVerticalSpacing(4);
	grid->setColumnStretch(1, 1);
	v->addWidget(list);

	QVector<SoundSlot> lib = SoundLibrary::load();
	std::function<void()> rebuild;
	auto commit = [&]() {
		SoundLibrary::save(lib);
		rebuild();
	};
	rebuild = [&]() {
		while (QLayoutItem *it = grid->takeAt(0)) {
			if (QWidget *w = it->widget())
				w->deleteLater();
			delete it;
		}
		for (int i = 0; i < lib.size(); ++i) {
			const SoundSlot ss = lib[i];
			auto *nm = new QLabel(ss.name, list);
			nm->setStyleSheet(QStringLiteral("color:#e8eaed; font-weight:bold;"));
			grid->addWidget(nm, i, 0);
			const bool mine = !ss.file.isEmpty();
			const bool gone = mine && !QFileInfo(ss.file).isFile();
			auto *fl = new QLabel(list);
			fl->setText(!mine ? (ss.builtin ? QStringLiteral("Built-in sound") : QStringLiteral("No file"))
					  : gone ? QStringLiteral("Missing: %1").arg(QFileInfo(ss.file).fileName())
						 : QFileInfo(ss.file).fileName());
			fl->setToolTip(mine ? QDir::toNativeSeparators(ss.file) : QString());
			fl->setStyleSheet(gone ? QStringLiteral("color:#e5484d;")
					       : mine ? QStringLiteral("color:#c8ccd4;") : QStringLiteral("color:#7f858e;"));
			grid->addWidget(fl, i, 1);
			auto *play = new QPushButton(QStringLiteral("▶"), list);
			play->setFixedWidth(28);
			play->setToolTip(QStringLiteral("Hear it"));
			connect(play, &QPushButton::clicked, &dlg, [this, name = ss.name]() {
				const QString f = SoundLibrary::resolve(SoundLibrary::load(), name, SoundLibrary::defaultCacheDir());
				if (f.isEmpty())
					return;
				QString wav = f;
				if (!f.endsWith(QLatin1String(".wav"), Qt::CaseInsensitive)) {
					wav = sessionAudioDir() + QStringLiteral("/libpreview.wav");
					QFile::remove(wav);
					if (!VoiceoverMixer::decodeToWav(f, wav))
						return;
				}
				previewWav(wav);
			});
			grid->addWidget(play, i, 2);
			auto *choose = new QPushButton(QStringLiteral("Choose file…"), list);
			connect(choose, &QPushButton::clicked, &dlg, [&, i]() {
				const QString start = lib[i].file.isEmpty() ? QString() : QFileInfo(lib[i].file).absolutePath();
				const QString f = QFileDialog::getOpenFileName(&dlg, QStringLiteral("Sound for %1").arg(lib[i].name),
									       start, audioOpenFilter());
				if (f.isEmpty())
					return;
				lib[i].file = f;
				commit();
			});
			grid->addWidget(choose, i, 3);
			auto *reset = new QPushButton(ss.builtin ? QStringLiteral("Reset") : QStringLiteral("Remove"), list);
			reset->setEnabled(!ss.builtin || mine);
			reset->setToolTip(ss.builtin ? QStringLiteral("Back to the built-in sound.")
						     : QStringLiteral("Remove this slot. Projects already using it keep "
								      "the sound they have."));
			connect(reset, &QPushButton::clicked, &dlg, [&, i]() {
				if (lib[i].builtin)
					lib[i].file.clear();
				else
					lib.remove(i);
				commit();
			});
			grid->addWidget(reset, i, 4);
		}
	};
	rebuild();

	auto *btns = new QHBoxLayout;
	auto *add = new QPushButton(QStringLiteral("Add sound…"), &dlg);
	add->setToolTip(QStringLiteral("A new named slot with a file of yours, e.g. Riser or Notification."));
	connect(add, &QPushButton::clicked, &dlg, [&]() {
		const QString f = QFileDialog::getOpenFileName(&dlg, QStringLiteral("Add a sound"), QString(), audioOpenFilter());
		if (f.isEmpty())
			return;
		bool okName = false;
		QString n = QInputDialog::getText(&dlg, QStringLiteral("Add a sound"), QStringLiteral("Name:"),
						  QLineEdit::Normal, QFileInfo(f).completeBaseName(), &okName)
				    .trimmed();
		if (!okName || n.isEmpty())
			return;
		if (SoundSlot *ex = const_cast<SoundSlot *>(SoundLibrary::find(lib, n))) {
			ex->file = f; // same name: that slot gets the file
		} else {
			SoundSlot ss;
			ss.name = n;
			ss.file = f;
			lib.append(ss);
		}
		commit();
	});
	btns->addWidget(add);
	btns->addStretch(1);
	auto *close = new QPushButton(QStringLiteral("Close"), &dlg);
	close->setDefault(true);
	connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
	btns->addWidget(close);
	v->addLayout(btns);
	dlg.exec();
	refreshLibrarySources();
}

void VideoEditorWindow::savePreset()
{
	if (!timelineView_)
		return;
	const TimelineModel &m = timelineView_->model();
	if (m.tags.isEmpty() && m.soundRules.isEmpty()) {
		QMessageBox::information(this, QStringLiteral("Save preset"),
					 QStringLiteral("This project has no tags or sound rules to save yet."));
		return;
	}
	bool okName = false;
	const QString name = QInputDialog::getText(this, QStringLiteral("Save preset"),
						   QStringLiteral("Save this project's %1 tag(s) and %2 sound rule(s) as:")
							   .arg(m.tags.size())
							   .arg(m.soundRules.size()),
						   QLineEdit::Normal, QString(), &okName)
				     .trimmed();
	if (!okName || name.isEmpty())
		return;
	if (SoundPresets::names().contains(name) &&
	    QMessageBox::question(this, QStringLiteral("Save preset"),
				  QStringLiteral("Replace the preset “%1”?").arg(name)) != QMessageBox::Yes)
		return;
	const QJsonObject p = SoundPresets::toJson(m, name, [this](int id) {
		SoundRef r;
		if (const EditorSource *es = sourceById(id)) {
			if (!es->librarySlot.isEmpty())
				r.slot = es->librarySlot;
			else
				r.file = es->origPath.isEmpty() ? es->path : es->origPath;
		}
		return r;
	});
	if (!SoundPresets::save(name, p))
		warnAndLog(this, QStringLiteral("Save preset"), QStringLiteral("Could not write the preset."));
	else if (infoLabel_)
		infoLabel_->setText(QStringLiteral("Preset “%1” saved.").arg(name));
}

void VideoEditorWindow::loadPreset(const QString &name)
{
	if (!timelineView_)
		return;
	const QJsonObject p = SoundPresets::load(name);
	if (p.isEmpty())
		return;
	if (!fullEdit())
		setEditMode(EditMode::Full);
	PresetMergeResult res;
	auto sourceFor = [this](const SoundRef &r) -> int {
		if (!r.slot.isEmpty())
			return librarySoundSource(r.slot);
		if (r.file.isEmpty())
			return -1;
		const QString want = QFileInfo(r.file).absoluteFilePath();
		for (const EditorSource &es : sources_)
			if (QFileInfo(es.origPath).absoluteFilePath() == want || QFileInfo(es.path).absoluteFilePath() == want)
				return es.id;
		return QFileInfo(r.file).isFile() ? addAudioSource(r.file) : -1;
	};
	timelineView_->editModel([&](TimelineModel &m) { res = SoundPresets::merge(m, p, sourceFor); });
	QString msg = QStringLiteral("Preset “%1”: %2 tag(s) and %3 rule(s) added")
			      .arg(name)
			      .arg(res.tagsAdded)
			      .arg(res.rulesAdded);
	if (res.rulesSkipped)
		msg += QStringLiteral(", %1 already here").arg(res.rulesSkipped);
	if (res.soundsMissing)
		msg += QStringLiteral(", %1 skipped because their sound file was not found").arg(res.soundsMissing);
	if (infoLabel_)
		infoLabel_->setText(msg + QLatin1Char('.'));
	showSoundsTab();
}

void VideoEditorWindow::addSoundRule(TlSoundRule r, int sourceId, const QString &name)
{
	if (!timelineView_ || sourceId < 0)
		return;
	if (!fullEdit())
		setEditMode(EditMode::Full);
	r.id = timelineView_->model().nextSoundRuleId();
	r.sourceId = sourceId;
	r.soundName = name;
	r.enabled = true;
	timelineView_->editSoundRules([r](QVector<TlSoundRule> &rules) { rules.append(r); });
	showSoundsTab();
	const TimelineModel &m = timelineView_->model();
	int placed = 0;
	if (const int lane = m.soundsLane(); lane >= 0)
		for (const TlClip &c : m.tracks[lane].clips)
			if (c.soundRule == r.id)
				++placed;
	consolePrint(QStringLiteral("// sound rule: %1 -> %2   (%3 placed now)")
			     .arg(SoundRules::triggerLabel(r, &timelineView_->model()), name)
			     .arg(placed),
		     ConsoleTone::Comment);
	if (infoLabel_)
		infoLabel_->setText(placed == 0
					    ? QStringLiteral("Rule added: %1. Nothing on the timeline matches it yet; "
							     "the sound is placed as soon as something does.")
						      .arg(SoundRules::triggerLabel(r, &timelineView_->model()))
					    : QStringLiteral("Rule added: %1 — %2 placed on the Sounds lane.")
						      .arg(SoundRules::triggerLabel(r, &timelineView_->model()))
						      .arg(placed == 1 ? QStringLiteral("1 sound")
								       : QStringLiteral("%1 sounds").arg(placed)));
}

void VideoEditorWindow::editSoundRule(int id, const std::function<void(TlSoundRule &)> &fn)
{
	if (!timelineView_ || !fn)
		return;
	timelineView_->editSoundRules([id, &fn](QVector<TlSoundRule> &rules) {
		for (TlSoundRule &r : rules)
			if (r.id == id)
				fn(r);
	});
}

void VideoEditorWindow::onSoundForClip(int track, int clip, bool everyKind)
{
	if (!timelineView_)
		return;
	const TimelineModel &m = timelineView_->model();
	if (track < 0 || track >= m.tracks.size() || clip < 0 || clip >= m.tracks[track].clips.size())
		return;
	const TlClip target = m.tracks[track].clips[clip];
	const bool picture = TimelineModel::isPictureKind(m.tracks[track].kind);
	QString name;
	const int src = pickSound(QCursor::pos(), &name);
	if (src < 0)
		return;
	if (everyKind) {
		addSoundRule(SoundRules::ruleForClip(target, picture), src, name);
		return;
	}
	// This clip only: one ordinary audio clip at its start, on a free lane.
	// Shaped like a rule's clip (short fades, no crossfade) but not marked as
	// one, so it is the user's to move, trim and delete.
	const SoundInfo si = soundInfoFor(src);
	if (si.durationMs <= 0) {
		QMessageBox::information(this, QStringLiteral("Sound"),
					 QStringLiteral("That sound could not be read."));
		return;
	}
	TlSoundRule once;
	once.sourceId = src;
	SoundEvent e;
	e.atMs = target.outStartMs;
	TlClip c = SoundRules::clipFor(once, e, si);
	c.soundRule = 0;
	if (!fullEdit())
		setEditMode(EditMode::Full);
	timelineView_->addClipOnFreeLane(TlTrack::Kind::Audio, c);
	if (infoLabel_)
		infoLabel_->setText(QStringLiteral("%1 placed at %2.").arg(name, timeTextMs(target.outStartMs)));
}

void VideoEditorWindow::onSoundForTransition(int track, int clip, bool thisTypeOnly)
{
	if (!timelineView_)
		return;
	const TimelineModel &m = timelineView_->model();
	if (track < 0 || track >= m.tracks.size() || clip < 0 || clip >= m.tracks[track].clips.size())
		return;
	const TlClip incoming = m.tracks[track].clips[clip];
	QString name;
	const int src = pickSound(QCursor::pos(), &name);
	if (src < 0)
		return;
	addSoundRule(SoundRules::ruleForTransition(incoming, thisTypeOnly), src, name);
}

void VideoEditorWindow::previewSound(int sourceId)
{
	const QString wav = audioProxyFor(sourceId);
	if (!wav.isEmpty())
		previewWav(wav);
}

void VideoEditorWindow::previewWav(const QString &wav)
{
	if (wav.isEmpty())
		return;
	// Through the same decoder and output path as timeline playback (a float
	// stereo 48 kHz sink), not QSoundEffect: that played nothing on Windows,
	// where its backend never opened the decoded WAV. renderTakes is what the
	// preview mix already trusts, and a sound is short enough to decode here.
	VoiceoverMixer::Take tk;
	tk.path = wav;
	tk.fadeInMs = SoundRules::kFadeInMs;
	tk.fadeOutMs = SoundRules::kFadeOutMs;
	std::vector<float> pcm = VoiceoverMixer::renderTakes({tk});
	if (pcm.empty()) {
		if (infoLabel_)
			infoLabel_->setText(QStringLiteral("That sound could not be decoded."));
		return;
	}
	if (!soundPreview_)
		soundPreview_ = new AudioPreview(this);
	if (!AudioPreview::available()) {
		if (infoLabel_)
			infoLabel_->setText(QStringLiteral("No audio output device to play it on."));
		return;
	}
	// Timeline playback and the sound preview share the speakers, not the
	// buffer: stop the one so the other is heard on its own.
	if (audioPreview_ && audioPreview_->isPlaying())
		audioPreview_->stop();
	soundPreview_->setBuffer(std::move(pcm));
	soundPreview_->setVolume(0.9);
	soundPreview_->start(0);
}

void VideoEditorWindow::freezeSounds()
{
	if (!timelineView_)
		return;
	const TimelineModel &m = timelineView_->model();
	if (m.soundRules.isEmpty() && m.soundsLane() < 0)
		return;
	const auto answer = QMessageBox::question(
		this, QStringLiteral("Convert to normal clips"),
		QStringLiteral("The Sounds lane's clips become ordinary audio clips you can move, trim "
			       "and delete. The %1 rule(s) and any Sound components that made them are "
			       "removed, so nothing is placed automatically from now on.\n\nConvert?")
			.arg(m.soundRules.size()),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
	if (answer != QMessageBox::Yes)
		return;
	TimelineModel w = m;
	SoundRules::freeze(w);
	timelineView_->setModelAndCommit(w); // one undo step; the tab follows through clipsChanged
}

void VideoEditorWindow::showSoundsTab()
{
	revealInspector();
	if (insTabs_ && kInsTabSounds < insTabs_->count())
		insTabs_->setCurrentIndex(kInsTabSounds);
	rebuildSoundsTab();
}

void VideoEditorWindow::buildSoundsTab(QVBoxLayout *into)
{
	soundsLayout_ = into;
	auto *intro = new QLabel(
		QStringLiteral("A sound that plays whenever a kind of thing happens: every transition, "
			       "every image or caption that appears, every typing caption. Each rule "
			       "places its clips on a locked Sounds lane and places them again whenever "
			       "the timeline changes, so a caption added later gets its sound too.\n\n"
			       "Right-click a clip, or inside a transition, for “Sound when this "
			       "appears”."),
		this);
	intro->setWordWrap(true);
	intro->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	into->addWidget(intro);

	auto *top = new QHBoxLayout;
	auto *libBtn = new QPushButton(QStringLiteral("Sound library…"), this);
	libBtn->setToolTip(QStringLiteral("Put your own files behind Whoosh, Pop, Click… or add sounds of your own."));
	connect(libBtn, &QPushButton::clicked, this, &VideoEditorWindow::openSoundLibrary);
	top->addWidget(libBtn);
	auto *presetBtn = new QPushButton(QStringLiteral("Presets  \u25BE"), this);
	presetBtn->setToolTip(QStringLiteral("Save this project's tags and rules, or load them into this one."));
	connect(presetBtn, &QPushButton::clicked, this, [this, presetBtn]() {
		QMenu menu(this);
		QAction *save = menu.addAction(QStringLiteral("Save as preset…"));
		QMenu *load = menu.addMenu(QStringLiteral("Load preset"));
		QMenu *del = menu.addMenu(QStringLiteral("Delete preset"));
		const QStringList names = SoundPresets::names();
		for (const QString &n : names) {
			load->addAction(n)->setData(QStringLiteral("load:") + n);
			del->addAction(n)->setData(QStringLiteral("del:") + n);
		}
		load->setEnabled(!names.isEmpty());
		del->setEnabled(!names.isEmpty());
		menu.addSeparator();
		QAction *folder = menu.addAction(QStringLiteral("Open presets folder"));
		QAction *chosen = menu.exec(presetBtn->mapToGlobal(QPoint(0, presetBtn->height())));
		if (!chosen)
			return;
		if (chosen == save) {
			savePreset();
			return;
		}
		if (chosen == folder) {
			QDir().mkpath(SoundPresets::dir());
			QDesktopServices::openUrl(QUrl::fromLocalFile(SoundPresets::dir()));
			return;
		}
		const QString d = chosen->data().toString();
		if (d.startsWith(QLatin1String("load:")))
			loadPreset(d.mid(5));
		else if (d.startsWith(QLatin1String("del:")) &&
			 QMessageBox::question(this, QStringLiteral("Delete preset"),
					       QStringLiteral("Delete the preset \u201c%1\u201d?").arg(d.mid(4))) ==
				 QMessageBox::Yes)
			SoundPresets::remove(d.mid(4));
	});
	top->addWidget(presetBtn);
	top->addStretch(1);
	into->addLayout(top);

	soundsList_ = new QWidget(this);
	auto *ll = new QVBoxLayout(soundsList_);
	ll->setContentsMargins(0, 0, 0, 0);
	ll->setSpacing(panelCardGap_);
	into->addWidget(soundsList_);
	soundsEmpty_ = new QLabel(QStringLiteral("No rules yet."), this);
	soundsEmpty_->setStyleSheet(QStringLiteral("color:#7f858e; padding:6px 2px;"));
	into->addWidget(soundsEmpty_);

	auto *btnsHost = new QWidget(this);
	auto *btns = new FlowLayout(btnsHost, 6, 4); // two long labels: wrap, never scroll
	auto *add = new QPushButton(QStringLiteral("Add rule…"), this);
	add->setToolTip(QStringLiteral("Pick what it fires on, then the sound."));
	connect(add, &QPushButton::clicked, this, [this, add]() {
		QMenu menu(this);
		auto put = [&](QMenu *m, const TlSoundRule &r) {
			QAction *a = m->addAction(SoundRules::triggerLabel(r, &timelineView_->model()));
			a->setData(QVariant::fromValue(QStringList{QString::number(int(r.trigger)),
								   QString::number(r.transitionType), r.componentId,
								   QString::number(r.tagId)}));
		};
		TlSoundRule r;
		r.trigger = TlSoundRule::Trigger::AnyTransition;
		put(&menu, r);
		QMenu *byType = menu.addMenu(QStringLiteral("One transition type"));
		for (int i = 0; i < kTransitionTypeCount; ++i) {
			r.trigger = TlSoundRule::Trigger::Transition;
			r.transitionType = i;
			put(byType, r);
		}
		r.transitionType = 0;
		menu.addSeparator();
		r.trigger = TlSoundRule::Trigger::ImageAppears;
		put(&menu, r);
		r.trigger = TlSoundRule::Trigger::TextAppears;
		put(&menu, r);
		r.trigger = TlSoundRule::Trigger::Component;
		r.componentId = QStringLiteral("harpia.textType");
		put(&menu, r);
		r.componentId.clear();
		r.trigger = TlSoundRule::Trigger::VideoStarts;
		put(&menu, r);
		r.trigger = TlSoundRule::Trigger::AnyClipStarts;
		put(&menu, r);
		menu.addSeparator();
		QMenu *byTag = menu.addMenu(QStringLiteral("Clips with a tag"));
		for (const TlTag &t : timelineView_->model().tags)
			put(byTag, SoundRules::ruleForTag(t.id));
		if (timelineView_->model().tags.isEmpty()) {
			QAction *none = byTag->addAction(QStringLiteral("No tags yet"));
			none->setEnabled(false);
			none->setToolTip(QStringLiteral("Tag a clip first: right-click it \u25B8 Tags, or type in the "
							"Tags field on the Clip tab."));
			byTag->setToolTipsVisible(true);
		}
		QAction *chosen = menu.exec(add->mapToGlobal(QPoint(0, add->height())));
		if (!chosen)
			return;
		const QStringList d = chosen->data().toStringList();
		TlSoundRule nr;
		nr.trigger = TlSoundRule::Trigger(d.value(0).toInt());
		nr.transitionType = d.value(1).toInt();
		nr.componentId = d.value(2);
		nr.tagId = d.value(3).toInt();
		QString name;
		const int src = pickSound(add->mapToGlobal(QPoint(0, add->height())), &name);
		if (src < 0)
			return;
		addSoundRule(nr, src, name);
	});
	btns->addWidget(add);
	soundsFreezeBtn_ = new QPushButton(QStringLiteral("Convert to normal clips"), this);
	soundsFreezeBtn_->setToolTip(QStringLiteral(
		"Hand the Sounds lane's clips to you as ordinary audio clips and remove the rules."));
	connect(soundsFreezeBtn_, &QPushButton::clicked, this, &VideoEditorWindow::freezeSounds);
	btns->addWidget(soundsFreezeBtn_);
	into->addWidget(btnsHost);
	into->addStretch(1);
	rebuildSoundsTab();
}

void VideoEditorWindow::fillTriggerCombo(QComboBox *cb, const TlSoundRule &r)
{
	auto key = [](TlSoundRule::Trigger t, int tt, const QString &comp, int tag) {
		return QStringLiteral("%1|%2|%3|%4").arg(int(t)).arg(tt).arg(comp).arg(tag);
	};
	const TimelineModel *m = timelineView_ ? &timelineView_->model() : nullptr;
	// The options depend only on the tag list, so they are refilled only when
	// that changed -- not on every edit, which used to rebuild some twenty
	// entries per rule per mouse move. The caller holds a signal blocker.
	QString sig;
	if (m)
		for (const TlTag &t : m->tags)
			sig += QString::number(t.id) + QLatin1Char(':') + t.name + QLatin1Char('|');
	if (cb->count() == 0 || cb->property("harpiaSig").toString() != sig) {
		cb->clear();
		auto put = [&](TlSoundRule::Trigger t, int tt = 0, const QString &comp = QString(), int tag = 0) {
			cb->addItem(SoundRules::triggerLabel(t, tt, comp, m ? m->tagName(tag) : QString()),
				    key(t, tt, comp, tag));
		};
		put(TlSoundRule::Trigger::AnyTransition);
		for (int i = 0; i < kTransitionTypeCount; ++i)
			put(TlSoundRule::Trigger::Transition, i);
		put(TlSoundRule::Trigger::ImageAppears);
		put(TlSoundRule::Trigger::TextAppears);
		put(TlSoundRule::Trigger::Component, 0, QStringLiteral("harpia.textType"));
		put(TlSoundRule::Trigger::VideoStarts);
		put(TlSoundRule::Trigger::AnyClipStarts);
		if (m)
			for (const TlTag &t : m->tags)
				put(TlSoundRule::Trigger::Tagged, 0, QString(), t.id);
		cb->setProperty("harpiaSig", sig);
	}
	const bool tagged = r.trigger == TlSoundRule::Trigger::Tagged;
	const bool comp = r.trigger == TlSoundRule::Trigger::Component;
	const bool trans = r.trigger == TlSoundRule::Trigger::Transition;
	int i = cb->findData(key(r.trigger, trans ? r.transitionType : 0, comp ? r.componentId : QString(),
				 tagged ? r.tagId : 0));
	if (i < 0 && (comp || tagged)) {
		// A component or tag the list does not know: shown as is, and the
		// list refilled next time so the extra entry does not linger.
		cb->addItem(SoundRules::triggerLabel(r, m),
			    key(r.trigger, 0, comp ? r.componentId : QString(), tagged ? r.tagId : 0));
		i = cb->count() - 1;
		cb->setProperty("harpiaSig", QString());
	}
	cb->setCurrentIndex(i < 0 ? 0 : i);
}

void VideoEditorWindow::rebuildSoundsTab()
{
	if (!soundsList_ || !timelineView_)
		return;
	if (!insTabShowing(kInsTabSounds)) {
		soundsTabDirty_ = true; // caught up when the tab is shown
		return;
	}
	soundsTabDirty_ = false;
	const TimelineModel &m = timelineView_->model();
	const QVector<TlSoundRule> &rules = m.soundRules;
	bool same = rules.size() == soundRows_.size();
	for (int i = 0; same && i < rules.size(); ++i)
		if (rules[i].id != soundRows_[i].id)
			same = false;
	if (!same) {
		// The set changed: build the rows afresh. (Values alone are synced
		// below without touching the widgets, so a spin box being dragged is
		// not pulled out from under the pointer.)
		for (const SoundRow &row : soundRows_)
			if (row.on)
				row.on->parentWidget()->deleteLater();
		soundRows_.clear();
		auto *ll = qobject_cast<QVBoxLayout *>(soundsList_->layout());
		for (const TlSoundRule &r : rules) {
			const int id = r.id;
			SoundRow row;
			row.id = id;
			auto *frame = new QFrame(soundsList_);
			frame->setStyleSheet(QStringLiteral("QFrame{background:#26292d;border-radius:%1px;}").arg(panelCardRadius_));
			auto *v = new QVBoxLayout(frame);
			v->setContentsMargins(panelCardPad_, std::max(0, panelCardPad_ - 2), panelCardPad_,
					      std::max(0, panelCardPad_ - 2));
			v->setSpacing(4);

			auto *top = new QHBoxLayout;
			row.on = new QCheckBox(frame);
			row.on->setToolTip(QStringLiteral("Off keeps the rule but places nothing."));
			connect(row.on, &QCheckBox::toggled, this,
				[this, id](bool on) { editSoundRule(id, [on](TlSoundRule &r) { r.enabled = on; }); });
			top->addWidget(row.on);
			row.trigger = new QComboBox(frame);
			row.trigger->setToolTip(QStringLiteral("What the sound plays on."));
			connect(row.trigger, &QComboBox::currentIndexChanged, this, [this, id](int i) {
				auto *cb = qobject_cast<QComboBox *>(sender());
				if (!cb || i < 0)
					return;
				const QStringList k = cb->itemData(i).toString().split(QLatin1Char('|'));
				if (k.size() != 4)
					return;
				editSoundRule(id, [k](TlSoundRule &r) {
					r.trigger = TlSoundRule::Trigger(k[0].toInt());
					r.transitionType = k[1].toInt();
					r.componentId = k[2];
					r.tagId = k[3].toInt();
				});
			});
			top->addWidget(row.trigger, 1);
			auto *del = new QPushButton(QStringLiteral("✕"), frame);
			del->setFlat(true);
			del->setFixedWidth(24);
			del->setToolTip(QStringLiteral("Remove this rule and the sounds it placed."));
			connect(del, &QPushButton::clicked, this, [this, id]() {
				timelineView_->editSoundRules([id](QVector<TlSoundRule> &rules) {
					for (int i = 0; i < rules.size(); ++i)
						if (rules[i].id == id) {
							rules.remove(i);
							return;
						}
				});
			});
			top->addWidget(del);
			v->addLayout(top);

			auto *mid = new QHBoxLayout;
			row.sound = new QPushButton(frame);
			row.sound->setToolTip(QStringLiteral("The sound. Click to pick another."));
			connect(row.sound, &QPushButton::clicked, this, [this, id]() {
				auto *b = qobject_cast<QPushButton *>(sender());
				QString name;
				const int src = pickSound(b ? b->mapToGlobal(QPoint(0, b->height())) : QCursor::pos(), &name);
				if (src < 0)
					return;
				editSoundRule(id, [src, name](TlSoundRule &r) {
					r.sourceId = src;
					r.soundName = name;
				});
			});
			mid->addWidget(row.sound, 1);
			auto *play = new QPushButton(QStringLiteral("▶"), frame);
			play->setFixedWidth(28);
			play->setToolTip(QStringLiteral("Hear it."));
			connect(play, &QPushButton::clicked, this, [this, id]() {
				for (const TlSoundRule &r : timelineView_->model().soundRules)
					if (r.id == id)
						previewSound(r.sourceId);
			});
			mid->addWidget(play);
			row.lane = new QComboBox(frame);
			row.lane->setToolTip(QStringLiteral("Only events on this lane, or on any lane."));
			connect(row.lane, &QComboBox::currentIndexChanged, this, [this, id](int i) {
				auto *cb = qobject_cast<QComboBox *>(sender());
				if (!cb || i < 0)
					return;
				const int lane = cb->itemData(i).toInt();
				editSoundRule(id, [lane](TlSoundRule &r) { r.lane = lane; });
			});
			mid->addWidget(row.lane);
			v->addLayout(mid);

			auto *bottom = new QHBoxLayout;
			auto mkSpin = [&](const QString &label, int lo, int hi, const QString &suffix,
					  const QString &tip) {
				auto *l = new QLabel(label, frame);
				l->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
				bottom->addWidget(l);
				auto *sp = new QSpinBox(frame);
				sp->setRange(lo, hi);
				sp->setSuffix(suffix);
				sp->setToolTip(tip);
				sp->setKeyboardTracking(false);
				new WheelGuard(sp, this);
				bottom->addWidget(sp);
				return sp;
			};
			row.volume = mkSpin(QStringLiteral("Vol"), 0, 200, QStringLiteral(" %"),
					    QStringLiteral("The clip's volume."));
			connect(row.volume, &QSpinBox::valueChanged, this, [this, id](int v) {
				editSoundRule(id, [v](TlSoundRule &r) { r.volume = v / 100.0; });
			});
			row.offset = mkSpin(QStringLiteral("Start"), -3000, 3000, QStringLiteral(" ms"),
					    QStringLiteral("Shift the sound: negative starts it before the event, "
							   "positive after."));
			connect(row.offset, &QSpinBox::valueChanged, this, [this, id](int v) {
				editSoundRule(id, [v](TlSoundRule &r) { r.offsetMs = v; });
			});
			row.maxLen = mkSpin(QStringLiteral("Cut at"), 0, 20000, QStringLiteral(" ms"),
					    QStringLiteral("Stop the sound after this long. 0 plays it whole."));
			row.maxLen->setSpecialValueText(QStringLiteral("whole"));
			connect(row.maxLen, &QSpinBox::valueChanged, this, [this, id](int v) {
				editSoundRule(id, [v](TlSoundRule &r) { r.maxMs = v; });
			});
			bottom->addStretch(1);
			v->addLayout(bottom);

			ll->addWidget(frame);
			soundRows_.append(row);
		}
	}
	// Values follow the model, silently.
	QString laneSig;
	for (int t = 0; t < m.tracks.size(); ++t)
		if (TimelineModel::isPictureKind(m.tracks[t].kind))
			laneSig += QString::number(t) + QLatin1Char(':') + m.tracks[t].name + QLatin1Char('|');
	for (int i = 0; i < rules.size() && i < soundRows_.size(); ++i) {
		const TlSoundRule &r = rules[i];
		SoundRow &row = soundRows_[i];
		QSignalBlocker b1(row.on), b2(row.trigger), b3(row.lane), b4(row.volume), b5(row.offset),
			b6(row.maxLen);
		row.on->setChecked(r.enabled);
		fillTriggerCombo(row.trigger, r);
		// Lanes: the picture lanes as they are now, refilled only when the
		// lane list changed.
		if (row.lane->count() == 0 || row.lane->property("harpiaSig").toString() != laneSig) {
			row.lane->clear();
			row.lane->addItem(QStringLiteral("Any lane"), -1);
			for (int t = 0; t < m.tracks.size(); ++t)
				if (TimelineModel::isPictureKind(m.tracks[t].kind))
					row.lane->addItem(m.tracks[t].name.isEmpty() ? QStringLiteral("Lane %1").arg(t + 1)
										     : m.tracks[t].name,
							  t);
			row.lane->setProperty("harpiaSig", laneSig);
		}
		const int li = row.lane->findData(r.lane);
		row.lane->setCurrentIndex(li < 0 ? 0 : li);
		row.sound->setText(r.soundName.isEmpty() ? QStringLiteral("Choose sound…") : r.soundName);
		row.volume->setValue(int(std::lround(r.volume * 100)));
		row.offset->setValue(r.offsetMs);
		row.maxLen->setValue(r.maxMs);
	}
	if (soundsEmpty_)
		soundsEmpty_->setVisible(rules.isEmpty());
	if (soundsFreezeBtn_)
		soundsFreezeBtn_->setEnabled(!rules.isEmpty() || m.soundsLane() >= 0);
}

// ---- tags -----------------------------------------------------------------------

void VideoEditorWindow::tagSelection(int tagId, bool on)
{
	if (!timelineView_ || tagId <= 0)
		return;
	const auto sel = timelineView_->selectedPairs();
	if (sel.isEmpty())
		return;
	timelineView_->editModel([&](TimelineModel &m) {
		for (const auto &p : sel)
			if (p.first >= 0 && p.first < m.tracks.size() && !m.tracks[p.first].locked &&
			    p.second >= 0 && p.second < m.tracks[p.first].clips.size())
				m.tracks[p.first].clips[p.second].setTag(tagId, on);
	});
}

void VideoEditorWindow::syncClipTags()
{
	if (!clipTagChips_ || !timelineView_)
		return;
	const TimelineModel &m = timelineView_->model();
	const auto sel = timelineView_->selectedPairs();
	// A tag every selected clip carries is a bright chip; one only some carry
	// is faded, so a mixed selection says so instead of pretending.
	QMap<int, int> count;
	for (const auto &p : sel)
		if (p.first >= 0 && p.first < m.tracks.size() && p.second >= 0 &&
		    p.second < m.tracks[p.first].clips.size())
			for (int id : m.tracks[p.first].clips[p.second].tags)
				++count[id];
	QStringList chips;
	for (const TlTag &t : m.tags) {
		const int n = count.value(t.id);
		if (n == 0)
			continue;
		const bool all = n == sel.size();
		chips << QStringLiteral("<span style='color:%1;'>●</span>&nbsp;<span style='color:%2;'>%3</span>"
					"&nbsp;<a href='rm:%4' style='color:#9a9fa8;text-decoration:none;'>✕</a>")
				 .arg(t.color.name(), all ? QStringLiteral("#e8eaed") : QStringLiteral("#7f858e"),
				      t.name.toHtmlEscaped())
				 .arg(t.id);
	}
	clipTagChips_->setText(chips.isEmpty() ? QStringLiteral("<span style='color:#7f858e;'>No tags.</span>")
					       : chips.join(QStringLiteral("&nbsp;").repeated(std::max(1, panelChipGap_))));
	if (tagNames_) {
		QStringList names;
		for (const TlTag &t : m.tags)
			names << t.name;
		if (tagNames_->stringList() != names)
			tagNames_->setStringList(names);
	}
	if (clipTagEdit_)
		clipTagEdit_->setPlaceholderText(sel.size() > 1
							 ? QStringLiteral("Tag all %1 selected clips: type, Enter").arg(sel.size())
							 : QStringLiteral("Add a tag: type a name, Enter"));
}

void VideoEditorWindow::buildTagsSection(QVBoxLayout *into)
{
	QWidget *body = addSection(into, QStringLiteral("Tags"), true);
	auto *v = qobject_cast<QVBoxLayout *>(body->layout());
	auto *intro = new QLabel(
		QStringLiteral("Labels for clips: “intro”, “callout”, “b-roll”. A "
			       "sound rule can fire on a tag. Tag a clip from its right-click menu or the "
			       "Clip tab."),
		body);
	intro->setWordWrap(true);
	intro->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	v->addWidget(intro);
	tagsList_ = new QWidget(body);
	auto *ll = new QVBoxLayout(tagsList_);
	ll->setContentsMargins(0, 0, 0, 0);
	ll->setSpacing(3);
	v->addWidget(tagsList_);
	newTagEdit_ = new QLineEdit(body);
	newTagEdit_->setPlaceholderText(QStringLiteral("New tag: type a name, Enter"));
	connect(newTagEdit_, &QLineEdit::returnPressed, this, [this]() {
		const QString name = newTagEdit_->text().trimmed();
		if (name.isEmpty() || !timelineView_)
			return;
		if (timelineView_->model().tagNamed(name)) {
			newTagEdit_->selectAll();
			return; // already there
		}
		timelineView_->editModel(
			[&](TimelineModel &m) { m.ensureTag(name, TimelineView::randomPastel()); });
		newTagEdit_->clear();
	});
	v->addWidget(newTagEdit_);
	rebuildTagsSection();
}

void VideoEditorWindow::rebuildTagsSection()
{
	if (!tagsList_ || !timelineView_)
		return;
	if (!insTabShowing(kInsTabProject)) {
		tagsDirty_ = true; // caught up when the Project tab is shown
		return;
	}
	tagsDirty_ = false;
	const TimelineModel &m = timelineView_->model();
	QVector<int> ids;
	for (const TlTag &t : m.tags)
		ids << t.id;
	auto *ll = qobject_cast<QVBoxLayout *>(tagsList_->layout());
	if (ids != tagRowIds_) {
		while (QLayoutItem *it = ll->takeAt(0)) {
			if (QWidget *w = it->widget())
				w->deleteLater();
			delete it;
		}
		tagRowIds_ = ids;
		for (const TlTag &tg : m.tags) {
			const int id = tg.id;
			auto *row = new QWidget(tagsList_);
			row->setObjectName(QStringLiteral("tag%1").arg(id));
			auto *h = new QHBoxLayout(row);
			h->setContentsMargins(0, 0, 0, 0);
			h->setSpacing(4);
			auto *col = new QPushButton(row);
			col->setObjectName(QStringLiteral("color"));
			col->setFixedSize(18, 18);
			col->setToolTip(QStringLiteral("Colour"));
			connect(col, &QPushButton::clicked, this, [this, id]() {
				const TlTag *t = timelineView_->model().tag(id);
				if (!t)
					return;
				const QColor c = QColorDialog::getColor(t->color, this, QStringLiteral("Tag colour"));
				if (!c.isValid())
					return;
				timelineView_->editModel([id, c](TimelineModel &mm) {
					for (TlTag &x : mm.tags)
						if (x.id == id)
							x.color = c;
				});
			});
			h->addWidget(col);
			auto *name = new QLineEdit(row);
			name->setObjectName(QStringLiteral("name"));
			name->setToolTip(QStringLiteral("Rename: every clip with this tag follows."));
			connect(name, &QLineEdit::editingFinished, this, [this, id, name]() {
				const QString n = name->text().trimmed();
				const TlTag *t = timelineView_->model().tag(id);
				if (!t)
					return;
				if (n.isEmpty() || n == t->name) {
					name->setText(t->name);
					return;
				}
				if (const TlTag *other = timelineView_->model().tagNamed(n); other && other->id != id) {
					name->setText(t->name); // two tags cannot share a name
					return;
				}
				timelineView_->editModel([id, n](TimelineModel &mm) {
					for (TlTag &x : mm.tags)
						if (x.id == id)
							x.name = n;
				});
			});
			h->addWidget(name, 1);
			auto *count = new QLabel(row);
			count->setObjectName(QStringLiteral("count"));
			count->setStyleSheet(QStringLiteral("color:#7f858e;"));
			count->setToolTip(QStringLiteral("Clips with this tag"));
			h->addWidget(count);
			auto *snd = new QPushButton(QStringLiteral("♪"), row);
			snd->setFixedWidth(26);
			snd->setToolTip(QStringLiteral("A sound for every clip with this tag (a sound rule)."));
			connect(snd, &QPushButton::clicked, this, [this, id, snd]() {
				QString nm;
				const int src = pickSound(snd->mapToGlobal(QPoint(0, snd->height())), &nm);
				if (src >= 0)
					addSoundRule(SoundRules::ruleForTag(id), src, nm);
			});
			h->addWidget(snd);
			auto *del = new QPushButton(QStringLiteral("✕"), row);
			del->setFlat(true);
			del->setFixedWidth(22);
			del->setToolTip(QStringLiteral("Delete the tag: it comes off every clip, and its sound rules go."));
			connect(del, &QPushButton::clicked, this, [this, id]() {
				const TimelineModel &mm = timelineView_->model();
				int rules = 0;
				for (const TlSoundRule &r : mm.soundRules)
					if (r.trigger == TlSoundRule::Trigger::Tagged && r.tagId == id)
						++rules;
				if (rules > 0 &&
				    QMessageBox::question(this, QStringLiteral("Delete tag"),
							  QStringLiteral("“%1” has %2 sound rule(s). Deleting the tag "
									 "removes them too.")
								  .arg(mm.tagName(id))
								  .arg(rules)) != QMessageBox::Yes)
					return;
				timelineView_->editModel([id](TimelineModel &x) { x.removeTag(id); });
			});
			h->addWidget(del);
			ll->addWidget(row);
		}
	}
	// Values, silently. Clips per tag in one pass over the timeline, not one
	// pass per tag.
	QHash<int, int> perTag;
	for (const TlTrack &t : m.tracks)
		for (const TlClip &c : t.clips)
			for (int id : c.tags)
				++perTag[id];
	for (const TlTag &tg : m.tags) {
		auto *row = tagsList_->findChild<QWidget *>(QStringLiteral("tag%1").arg(tg.id));
		if (!row)
			continue;
		if (auto *col = row->findChild<QPushButton *>(QStringLiteral("color"))) {
			// setStyleSheet re-styles the widget even for the same text.
			const QString css = QStringLiteral("QPushButton{background:%1;border:1px solid #111;border-radius:9px;}")
						    .arg(tg.color.name());
			if (col->styleSheet() != css)
				col->setStyleSheet(css);
		}
		if (auto *name = row->findChild<QLineEdit *>(QStringLiteral("name")); name && !name->hasFocus())
			name->setText(tg.name);
		if (auto *count = row->findChild<QLabel *>(QStringLiteral("count")))
			count->setText(QString::number(perTag.value(tg.id)));
	}
}

void VideoEditorWindow::onAddAudioClicked()
{
	if (!fullEdit())
		setEditMode(EditMode::Full);
	QMenu menu(this);
	// The active clip's own audio, so it can be moved/faded on its own lane.
	if (EditorSource *s = activeSource()) {
		QAction *a = menu.addAction(
			QStringLiteral("Audio from \"%1\"").arg(s->name));
		const int id = s->id;
		connect(a, &QAction::triggered, this, [this, id]() { addAudioClipFromSource(id); });
	}
	QAction *imp = menu.addAction(QStringLiteral("Import audio file…"));
	menu.addSeparator();
	QAction *empty = menu.addAction(QStringLiteral("Add empty audio track"));

	QAction *chosen = menu.exec(addMenuBtn_ ? addMenuBtn_->mapToGlobal(QPoint(0, addMenuBtn_->height()))
						: QCursor::pos());
	if (chosen == imp) {
		const QString f = QFileDialog::getOpenFileName(
			this, QStringLiteral("Import audio"), QFileInfo(inPath_).absolutePath(),
			audioOpenFilter());
		if (f.isEmpty())
			return;
		const int id = addAudioSource(f);
		if (id >= 0)
			addAudioClipFromSource(id);
	} else if (chosen == empty) {
		if (!fullEdit())
			setEditMode(EditMode::Full);
		timelineView_->addTrack(TlTrack::Kind::Audio);
	}
}

void VideoEditorWindow::addActiveSourceToTimeline()
{
	EditorSource *s = activeSource();
	if (!s || s->durationMs <= 0 || !timelineView_)
		return;
	TlClip c;
	c.sourceId = s->id;
	c.srcStartMs = 0;
	c.srcEndMs = s->durationMs;
	c.outStartMs = timelineView_->model().contentDurationMs();
	timelineView_->addClip(TlTrack::Kind::Video, c);
	if (!fullEdit())
		setEditMode(EditMode::Full);
	updateInfoLabel();
}

double VideoEditorWindow::timelineFps() const
{
	// A frame rate the project pins wins. Otherwise the first source sets it,
	// like the canvas size does.
	if (projFps_ >= 1.0)
		return projFps_;
	if (!sources_.empty() && sources_.front().seeker && sources_.front().seeker->fps() > 1.0)
		return sources_.front().seeker->fps();
	return 30.0;
}

QSize VideoEditorWindow::previewRenderSize(QSize canvas) const
{
	if (canvas.isEmpty())
		return canvas;
	switch (previewQuality_) {
	case PreviewQuality::Full:
		return canvas;
	case PreviewQuality::Half:
		return canvas / 2;
	case PreviewQuality::Quarter:
		return canvas / 4;
	case PreviewQuality::Auto:
		break;
	}
	// Auto: no more than the preview widget can actually show. Rendering 1920
	// wide into an 800px area throws away more than half the pixels drawn, and
	// the difference is invisible by definition. Device pixel ratio is included
	// so it stays sharp on a HiDPI screen.
	if (!canvas_ || canvas_->width() <= 0)
		return canvas;
	const double dpr = canvas_->devicePixelRatioF() > 0.0 ? canvas_->devicePixelRatioF() : 1.0;
	// Quantized UP to the next 64 px: the decode cache keys on the exact
	// render size, so deriving it raw from the widget width meant every 1 px
	// of a splitter drag or window resize threw away every cached frame for
	// every source. Rounding up (never down) keeps it at least as sharp as
	// the widget needs while giving resizes a 64 px dead band.
	const int rawW = int(std::lround(canvas_->width() * dpr));
	const int wantW = ((rawW + 63) / 64) * 64;
	// Keeps the aspect exactly, and never goes below a size that is still
	// legible. Shared with the tests, because clamping the two sides separately
	// is precisely how a tall project came out stretched.
	return reducedRenderSize(canvas, wantW);
}

QSize VideoEditorWindow::multiCutCanvasSize() const
{
	// The PRIMARY source, because that is literally what the export encodes to:
	// runVideoCutsMulti() takes its canvas from inputs[0], and inputs[0] is
	// sources_.front(). Anything else here and the preview would be describing
	// a file nobody is going to get.
	if (!sources_.empty() && sources_.front().width > 0 && sources_.front().height > 0)
		return QSize(sources_.front().width, sources_.front().height);
	return QSize(1920, 1080);
}

// The shape the PREVIEW should have, which is the shape the current mode's
// output will have -- and those are three different things:
//   Full editing : the project canvas (that is what the project resolution is);
//   Multi-Cut    : the primary clip, which is the canvas the exporter builds;
//   Simple Trim  : the active source, written at its own size.
// Getting this wrong does not merely mislead, it distorts: the preview widget
// letterboxes the composited frame into this shape.
QSize VideoEditorWindow::previewCanvasSize()
{
	if (fullEdit())
		return timelineCanvasSize();
	if (multiCut())
		return multiCutCanvasSize();
	const EditorSource *s = sourceById(activeSourceId_);
	if (s && s->width > 0 && s->height > 0)
		return QSize(s->width, s->height);
	return QSize(1920, 1080);
}

QSize VideoEditorWindow::timelineCanvasSize() const
{
	// A canvas the project pins wins; otherwise the first source defines it
	// (like the multi-source export).
	if (projCanvas_.isValid() && !projCanvas_.isEmpty())
		return projCanvas_;
	if (!sources_.empty() && sources_.front().width > 0 && sources_.front().height > 0)
		return QSize(sources_.front().width, sources_.front().height);
	return QSize(1920, 1080);
}

void VideoEditorWindow::showTimelineFrame(qint64 outMs)
{
	if (!timelineView_ || !canvas_)
		return;
	// Feeds the compositor from the editor's per-source decoders (GUI thread).
	struct Provider : TimelineCompositor::FrameProvider {
		VideoEditorWindow *w = nullptr;
		int decodeW = 1920, decodeH = 1080;
		QSize sourceSize(int sourceId) override
		{
			if (const QSize s = w->stillSize(sourceId); !s.isEmpty())
				return s;
			if (const EditorSource *s = w->sourceById(sourceId); s && s->width > 0 && s->height > 0)
				return QSize(s->width, s->height);
			return QSize();
		}
		QImage frameFor(int sourceId, qint64 srcMs) override
		{
			// A still serves the same picture at every timestamp: a copy
			// sized for this preview, not the whole photo every frame.
			if (w->stillImages_.contains(sourceId))
				return w->stillForPreview(sourceId, QSize(decodeW, decodeH));

			// Playback owns a decoder per source/track. frameAt holds the frame
			// on screen, rolls forward for nearby requests and seeks directly
			// for a new in-point, matching the export's timing. Bounded catch-up
			// via nextFrameAt used to SHOW footage before the in-point while
			// racing through it eight frames per tick on a cold decoder.
			if (w->playing_) {
				// The same recording can play on two tracks at different
				// source times. Independent decoders keep each track's cached
				// frame and sequential position separate.
				const int lane = std::max(0, track());
				// Playback uses the seeker's own frame cache.
				w->frameCacheable_ = false;
				if (FrameSeeker *sk = w->playSeekerFor(sourceId, lane, decoderSlot())) {
					const QImage img = sk->frameAt(srcMs, decodeW, decodeH);
					if (!img.isNull())
						return img;
					// Decode failed (e.g. past EOF): fall through to the
					// scrub cache rather than compositing a hole.
				}
			}

			if (!w->previewDecoder_)
				return QImage();
			// Decoding straight to the size actually being composited saves both
			// the scale and the memory traffic behind it. Whatever the decoder
			// already has comes back now; anything it has to decode arrives via
			// frameReady and this composite is redone. One slow clip therefore
			// no longer holds up the whole frame.
			bool exact = false;
			const QImage img =
				w->previewDecoder_->frame(sourceId, srcMs, decodeW, decodeH, &exact);
			if (!exact) {
				w->shownExact_ = false;
				w->frameCacheable_ = false;
			}
			return img;
		}
	} fp;

	// Any clip script that hasn't been compiled in this evaluator yet (e.g. after
	// loading a project) is brought up before the frame is composed.
	// Only worth walking when the timeline actually changed — this is every clip
	// times every script in its stack, and it ran on every single frame.
	if (scriptEval_ && scriptScanDirty_) {
		scriptScanDirty_ = false;
		for (const TlTrack &t : timelineView_->model().tracks)
			for (const TlClip &c : t.clips)
				for (const TlScript &s : c.scripts)
					if (!s.name.isEmpty() && !scriptEval_->has(s.name))
						ensureScriptCompiled(s.name, nullptr);
	}
	const double fps = timelineFps();

	const QSize canvasSize = timelineCanvasSize();
	// The canvas keeps the PROJECT size: crop rectangles and preview-drag maths
	// are expressed against it, and must not move when preview quality changes.
	canvas_->setVideoSize(canvasSize.width(), canvasSize.height());
	const QSize renderSize = previewRenderSize(canvasSize);
	fp.w = this;
	fp.decodeW = renderSize.width();
	fp.decodeH = renderSize.height();
	// Cleared by the provider if any clip's frame was not ready; -1 is the
	// "composite the timeline" source id requestPreview already uses.
	shownSource_ = -1;
	shownMs_ = outMs;
	shownExact_ = true;
	// Seen before, with nothing on screen changed since: no decode, no composite.
	if (previewFrames_.maxCost() != kPreviewCacheMB * 1024)
		previewFrames_.setMaxCost(kPreviewCacheMB * 1024); // in KB
	// The timeline as shown: with a text variation previewed, a copy with that
	// clip's words swapped. Signed from the same copy, so a cached frame of the
	// original text is never served for a variation, or the other way round.
	const TimelineModel shownModel = previewModel();
	QByteArray sig;
	// Playback must also position its decoders, including when its first frame
	// was already rendered by the scrub cache. Otherwise cold-start work is
	// deferred until AFTER the playback clock has begun.
	if (!playing_) {
		// Playback neither reads nor writes this cache, so avoid serializing
		// active clips and hashing a key that it would immediately discard.
		sig = previewSignature(shownModel, outMs, renderSize, canvasSize);
		if (const QImage *hit = previewFrames_.object(sig)) {
			previewRenderWarning_->clearFailure();
			setPreviewFrame(*hit, outMs);
			return;
		}
	}
	frameCacheable_ = !playing_;
	TimelineCompositor::RenderError renderError;
	const QImage composed = TimelineCompositor::compose(shownModel, outMs, renderSize, fp, scriptEval_.get(),
							    fps, canvasSize, &renderError);
	if (composed.isNull() && !renderError.message.isEmpty()) {
		const QString key = QStringLiteral("%1/%2/%3/%4").arg(renderError.code)
			.arg(renderError.track).arg(renderError.clip).arg(renderError.message);
		QString report = QStringLiteral("Preview rendering failed [%1].\n\n%2\n\nTimeline time: %3 ms\nRender canvas: %4 × %5\n")
			.arg(renderError.code, renderError.message).arg(outMs)
			.arg(renderSize.width()).arg(renderSize.height());
		if (renderError.track >= 0 && renderError.track < shownModel.tracks.size()) {
			const TlTrack &track = shownModel.tracks[renderError.track];
			report += QStringLiteral("Track: %1 (%2)\nClip: %3\n")
				.arg(renderError.track + 1).arg(track.name).arg(renderError.clip + 1);
		}
		previewRenderWarning_->setFailure(key, renderError.message.section(QLatin1Char('\n'), 0, 0), report);
	} else {
		previewRenderWarning_->clearFailure();
	}
	if (frameCacheable_ && shownExact_ && !composed.isNull())
		previewFrames_.insert(sig, new QImage(composed), int(std::max<qint64>(1, composed.sizeInBytes() / 1024)));
	setPreviewFrame(composed, outMs);
}

// See FrameSignature.hpp; the shader generation joins the window's own, since
// a hot-reloaded .frag changes pixels without touching the model.
QByteArray VideoEditorWindow::previewSignature(const TimelineModel &m, qint64 outMs, QSize render,
					      QSize logical) const
{
	const qint64 gen = qint64(previewGen_) * 1000003 + ShaderComponents::generation();
	return timelineFrameSignature(m, outMs, render, logical, gen);
}

TimelineModel VideoEditorWindow::previewModel() const
{
	if (previewVariant_ <= 0 || !timelineView_)
		return timelineView_ ? timelineView_->model() : TimelineModel();
	const int t = timelineView_->selectedTrack(), ci = timelineView_->selectedClip();
	TimelineModel m = timelineView_->model();
	if (t < 0 || t >= m.tracks.size() || ci < 0 || ci >= m.tracks[t].clips.size())
		return m;
	TlClip &c = m.tracks[t].clips[ci];
	const QStringList opts = text_variations::optionsOf(c.text);
	if (c.type == TlClip::Type::Text && c.text.random && previewVariant_ < opts.size()) {
		c.text.text = opts[previewVariant_];
		c.words.clear();
	}
	return m;
}

void VideoEditorWindow::stepPreviewVariant(int delta)
{
	const TlClip *c = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!c || c->type != TlClip::Type::Text || !c->text.random)
		return;
	const int n = text_variations::optionsOf(c->text).size();
	if (n <= 1)
		return;
	previewVariant_ = ((previewVariant_ + delta) % n + n) % n; // wraps both ways
	syncVariationsInspector(*c);
	requestPreview(-1, timelinePlayheadMs());
}

void VideoEditorWindow::syncVariationsInspector(const TlClip &c, bool texts)
{
	if (!variantListLayout_ || c.type != TlClip::Type::Text || !c.text.random)
		return;
	const QStringList &vars = c.text.variations;
	if (texts && variantEdits_.size() != vars.size())
		rebuildVariantEditors(int(vars.size()));
	const int n = text_variations::optionsOf(c.text).size();
	previewVariant_ = std::clamp(previewVariant_, 0, std::max(0, n - 1));
	for (int i = 0; i < variantEdits_.size() && i < vars.size(); ++i) {
		QPlainTextEdit *ed = variantEdits_[i];
		// Only from the model when asked: a preview change arrives from a
		// box's cursor signal, which fires BEFORE that box's own edit has
		// reached the model -- writing the model's text back then would
		// undo the keystroke being typed.
		if (texts && ed->toPlainText() != vars[i]) {
			const QSignalBlocker b(ed);
			ed->setPlainText(vars[i]);
		}
		// The text the preview is showing is outlined.
		const bool shown = previewVariant_ > 0 && text_variations::optionIndexOfEntry(c.text, i) == previewVariant_;
		ed->setStyleSheet(shown ? QStringLiteral("QPlainTextEdit { border:1px solid #4c8dff; }") : QString());
	}
	textEdit_->setStyleSheet(previewVariant_ == 0 && n > 1 ? QStringLiteral("QPlainTextEdit { border:1px solid #4c8dff; }")
							       : QString());
	// Short, so the arrows and it fit the narrowest panel; the tooltip says it in full.
	variantLabel_->setText(n <= 1 ? QStringLiteral("1 text") : QStringLiteral("%1 of %2").arg(previewVariant_ + 1).arg(n));
	variantLabel_->setToolTip(n <= 1 ? QStringLiteral("Add a text to make a second version")
					 : QStringLiteral("The preview shows text %1 of %2").arg(previewVariant_ + 1).arg(n));
	variantPrev_->setEnabled(n > 1);
	variantNext_->setEnabled(n > 1);
}

void VideoEditorWindow::rebuildVariantEditors(int count)
{
	// The old rows may hold the button whose click got us here: let them go
	// once that click has returned.
	for (QWidget *row : variantRows_) {
		row->hide();
		row->deleteLater();
	}
	variantRows_.clear();
	variantEdits_.clear();
	for (int i = 0; i < count; ++i) {
		auto *row = new QWidget(variationsBox_);
		auto *rl = new QVBoxLayout(row);
		rl->setContentsMargins(0, 0, 0, 0);
		rl->setSpacing(2);
		auto *hdr = new QHBoxLayout;
		auto *cap = new QLabel(QStringLiteral("Text %1").arg(i + 2), row);
		cap->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
		auto *del = new QPushButton(row);
		del->setIcon(uiIcon(Glyph::Cross, 11));
		del->setFixedSize(24, 22);
		del->setStyleSheet(
			QStringLiteral("padding:0px; min-width:24px; max-width:24px; min-height:22px; max-height:22px;"));
		del->setFocusPolicy(Qt::NoFocus);
		del->setToolTip(QStringLiteral("Remove this text"));
		hdr->addWidget(cap, 1);
		hdr->addWidget(del);
		rl->addLayout(hdr);
		auto *ed = new QPlainTextEdit(row);
		ed->setFixedHeight(48);
		ed->setPlaceholderText(QStringLiteral("Another text for this caption… (Enter for a new line)"));
		ed->setToolTip(kRichTextTip);
		rl->addWidget(ed);
		variantListLayout_->addWidget(row);
		variantRows_.push_back(row);
		variantEdits_.push_back(ed);
		connect(del, &QPushButton::clicked, this, [this, i]() { removeVariantText(i); });
		connect(ed, &QPlainTextEdit::textChanged, this, [this, i, ed]() {
			if (syncingClip_ || captionLaneTrack_ >= 0)
				return;
			const QString t = ed->toPlainText();
			editSelectedClip([i, &t](TlClip &c) {
				if (i < c.text.variations.size())
					c.text.variations[i] = t;
			});
			if (const TlClip *c = timelineView_ ? timelineView_->selectedClipPtr() : nullptr)
				showPreviewVariant(text_variations::optionIndexOfEntry(c->text, i));
		});
		connect(ed, &QPlainTextEdit::cursorPositionChanged, this, [this, i, ed]() {
			if (syncingClip_ || !ed->hasFocus())
				return;
			if (const TlClip *c = timelineView_ ? timelineView_->selectedClipPtr() : nullptr)
				showPreviewVariant(text_variations::optionIndexOfEntry(c->text, i));
		});
	}
}

void VideoEditorWindow::showPreviewVariant(int option)
{
	const TlClip *c = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (option < 0 || !c || c->type != TlClip::Type::Text || !c->text.random)
		return;
	if (option == previewVariant_)
		return;
	previewVariant_ = option;
	syncVariationsInspector(*c, false);
	requestPreview(-1, timelinePlayheadMs());
}

void VideoEditorWindow::addVariantText()
{
	const TlClip *c = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!c || c->type != TlClip::Type::Text)
		return;
	editSelectedClip([](TlClip &c) {
		c.text.random = true;
		c.text.variations.append(QString());
	});
	syncClipInspector();
	if (!variantEdits_.isEmpty())
		variantEdits_.last()->setFocus();
}

void VideoEditorWindow::removeVariantText(int index)
{
	editSelectedClip([index](TlClip &c) {
		if (index >= 0 && index < c.text.variations.size())
			c.text.variations.removeAt(index);
	});
	previewVariant_ = 0;
	syncClipInspector();
	requestPreview(-1, timelinePlayheadMs());
}

void VideoEditorWindow::addRandomTextClip()
{
	if (!timelineView_)
		return;
	if (!fullEdit())
		setEditMode(EditMode::Full);
	TlClip c;
	c.type = TlClip::Type::Text;
	c.srcStartMs = 0;
	c.srcEndMs = 4000;
	c.outStartMs = timelinePlayheadMs();
	c.text.text = QStringLiteral("Your text");
	c.text.random = true;
	c.text.variations = {QStringLiteral("Your other text")}; // two versions from the start
	timelineView_->addClipOnFreeLane(TlTrack::Kind::Video, c);
	previewVariant_ = 0;
	syncPreviewTransformTarget();
	syncClipInspector();
	showTimelineFrame(timelinePlayheadMs());
}

void VideoEditorWindow::invalidatePreviewCache()
{
	++previewGen_;
	previewFrames_.clear();
}

void VideoEditorWindow::onTimelineScrub(qint64 outMs)
{
	if (playing_)
		stopPlayback();
	timelineView_->setPlayhead(outMs);
	cursorTimeLabel_->setText(previewTimeText(outMs));
	// The component list shows values AT THE PLAYHEAD, so moving the playhead
	// has to move them — otherwise an animated clip's numbers disagree with the
	// picture beside them, which is the fault the Spotlight fields had. Only the
	// component panel, not the whole clip inspector: this runs on every mouse
	// move of a scrub, and the panel's refresh is a handful of setValue calls
	// where a full sync rebuilds the effect controls.
	syncComponentPanel();
	// The preview's transform box outlines the clip AT THE PLAYHEAD, so a jump
	// to a keyframe (the list, Prev/Next, a ruler click) has to move it too, or
	// the grips stay on the old pose while the picture shows the new one.
	syncPreviewTransformTarget();
	requestPreview(-1, outMs); // -1 = "composite the timeline"
}

// Move the keyframe list's highlight to whichever key `atMs` sits on, without
// touching the list itself. Split out of syncClipInspector for the hover path:
// that runs on every mouse-move, and rebuilding a QListWidget at that rate
// wastes work and makes the scrollbar jump under the pointer.
void VideoEditorWindow::highlightKeyAt(qint64 atMs)
{
	if (!keyList_ || !timelineView_)
		return;
	const TlClip *c = timelineView_->selectedClipPtr();
	if (!c || keyList_->count() != c->keys.size())
		return;
	const bool wasSyncing = syncingClip_;
	syncingClip_ = true;
	keyList_->setCurrentRow(c->keyframeIndexAt(atMs));
	syncingClip_ = wasSyncing;
}

qint64 VideoEditorWindow::inspectTimeMs() const
{
	return hoverInspectMs_ >= 0 ? hoverInspectMs_ : timelinePlayheadMs();
}

void VideoEditorWindow::setHoverInspect(qint64 outMs)
{
	if (hoverInspectMs_ == outMs)
		return;
	const bool was = hoverInspectMs_ >= 0;
	hoverInspectMs_ = outMs;
	if (componentPanel_ && was != (outMs >= 0))
		componentPanel_->setPreviewing(outMs >= 0);
	// The values only -- not syncClipInspector(), which rebuilds the keyframe
	// list. This runs on every mouse-move across the timeline, and rebuilding a
	// list widget at that rate is both wasted work and a scrollbar that jumps
	// under the pointer. The list's contents do not depend on the instant
	// anyway; only which row is highlighted does, and that is done below.
	syncComponentPanel();
	syncSpotlightInspector();
	highlightKeyAt(inspectTimeMs());
}

void VideoEditorWindow::onTimelineHoverScrub(qint64 outMs)
{
	if (playing_)
		return;
	requestPreview(-1, outMs);
	// The Inspector describes the frame you are looking at. Hovering the
	// timeline changes which frame that is, and the panel used to keep showing
	// the playhead's numbers over a preview of somewhere else entirely -- so
	// reading off what a keyframed move is doing at 4.2 s meant dragging the
	// playhead there and losing where you were.
	setHoverInspect(outMs);
}

// ---- Per-clip transform scripting ----------------------------------------

QString VideoEditorWindow::componentsDirPath()
{
	if (componentsDir_.isEmpty()) {
		const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
		componentsDir_ = base + QStringLiteral("/harpia/components");
	}
	QDir().mkpath(componentsDir_);
	// Seed the bundled example, but never overwrite: unlike the scripts folder
	// this has one file and no history of shipped versions to reconcile, so the
	// simple rule is the right one until there is a reason for the other.
	const QString dst = componentsDir_ + QStringLiteral("/pulse.js");
	if (!QFile::exists(dst)) {
		QFile res(QStringLiteral(":/components/pulse.js"));
		if (res.open(QIODevice::ReadOnly)) {
			QFile out(dst);
			if (out.open(QIODevice::WriteOnly))
				out.write(res.readAll());
		}
	}
	return componentsDir_;
}

// Re-scan the components folder. Registering a type replaces any earlier one
// with the same id, so this is all hot reload needs; every render thread
// recompiles the new source on its next frame.
void VideoEditorWindow::reloadComponentsFromDisk()
{
	componentErrors_.clear();
	ComponentRegistry &reg = ComponentRegistry::instance();
	componentCount_ = ScriptComponents::loadFolder(componentsDirPath(), reg, &componentErrors_);
	// The shaders and the transform scripts are components too -- one type per
	// file, from the same folders they always lived in. Registering them here
	// rather than at startup is what makes their folders hot-reload on the same
	// terms as the components folder: save a .frag and the next frame has it.
	componentCount_ += ShaderComponents::loadFolder(shadersDirPath(), reg, &componentErrors_);
	invalidatePreviewCache(); // a reloaded component may draw differently
	componentCount_ +=
		TransformScriptComponents::loadFolder(scriptsDirPath(), reg, &componentErrors_);
	// Shown in the panel rather than written to the log: someone whose component
	// will not load needs to be told where they can see why, not left to
	// discover that Error Logs exists.
	syncComponentPanel();
	// A reloaded component may render differently, and the frame on screen was
	// made with the old one.
	if (fullEdit())
		showTimelineFrame(timelinePlayheadMs());
}

QString VideoEditorWindow::scriptsDirPath()
{
	if (scriptsDir_.isEmpty()) {
		const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
		scriptsDir_ = base + QStringLiteral("/harpia/scripts");
	}
	QDir().mkpath(scriptsDir_);
	// Seed the bundled examples, and refresh a copy the user hasn't touched.
	//
	// "Never overwrite" alone isn't enough: a fix to a shipped script would only
	// ever reach a fresh install, and everyone else would keep running the old
	// one with no way to know. So the hash of what was last written out is
	// remembered — if the file on disk still matches it, nobody has edited it and
	// it is safe to replace. Anything else is the user's, and is left alone.
	QSettings seedSt(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	seedSt.beginGroup(QStringLiteral("scriptSeed"));
	for (const QString &name : {QStringLiteral("tutorial-zoom"), QStringLiteral("zoom-in"),
				    QStringLiteral("fade-in-out"), QStringLiteral("shake")}) {
		const QString dst = scriptsDir_ + QLatin1Char('/') + name + QStringLiteral(".js");
		QFile res(QStringLiteral(":/scripts/") + name + QStringLiteral(".js"));
		if (!res.open(QIODevice::ReadOnly))
			continue;
		const QByteArray shipped = res.readAll();
		const QString shippedHash = QString::fromLatin1(
			QCryptographicHash::hash(shipped, QCryptographicHash::Sha1).toHex());

		if (QFile::exists(dst)) {
			QFile cur(dst);
			if (!cur.open(QIODevice::ReadOnly))
				continue;
			const QString curHash = QString::fromLatin1(
				QCryptographicHash::hash(cur.readAll(), QCryptographicHash::Sha1)
					.toHex());
			cur.close();
			if (curHash == shippedHash)
				continue; // already up to date
			// Only replace it if it is byte-for-byte something WE wrote.
			// Installs made before the hash was recorded have nothing in
			// settings, so every version ever shipped is listed here too --
			// otherwise a fix would reach new installs only.
			static const QSet<QString> kShippedBefore = {
				// tutorial-zoom
				QStringLiteral("50b80228b02fcda7a9560dc4258e80f15a930a0d"),
				// zoom-in (v0.1.131, then v0.1.124)
				QStringLiteral("9fbe7e0da22a52bdea20450471851e3e9a4d2493"),
				QStringLiteral("477848758b167f01e04326f4eb424159c6b0adfa"),
				// fade-in-out
				QStringLiteral("8b357f0a220c370b064b366c6c938bf4c87f2e0e"),
				// shake
				QStringLiteral("c40093071a95eb30877cdf2e919cde0c350bdb3e"),
			};
			if (curHash != seedSt.value(name).toString() &&
			    !kShippedBefore.contains(curHash))
				continue; // the user has edited this one: it is theirs
		}
		QFile out(dst);
		if (out.open(QIODevice::WriteOnly)) {
			out.write(shipped);
			out.close();
			seedSt.setValue(name, shippedHash);
		}
	}
	seedSt.endGroup();
	return scriptsDir_;
}

QStringList VideoEditorWindow::availableScripts()
{
	QStringList names;
	for (const QFileInfo &fi :
	     QDir(scriptsDirPath()).entryInfoList({QStringLiteral("*.js")}, QDir::Files, QDir::Name))
		names << fi.completeBaseName();
	return names;
}

// Repopulate the stack list from the selected clip. The names of the scripts on
// disk only matter when the "Add script" menu opens, so nothing to refresh here
// beyond the clip's own stack.
void VideoEditorWindow::refreshScriptList()
{
	if (!scriptList_)
		return;
	const TlClip *c = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	const bool wasSyncing = syncingClip_;
	syncingClip_ = true; // repopulating must not rewrite the clip
	scriptList_->clear();
	if (c)
		for (int i = 0; i < c->scripts.size(); ++i)
			scriptList_->addItem(
				QStringLiteral("%1.  %2").arg(i + 1).arg(c->scripts[i].name));
	const int n = scriptList_->count();
	scriptSel_ = n == 0 ? -1 : std::clamp(scriptSel_, 0, n - 1);
	scriptList_->setCurrentRow(scriptSel_);
	syncingClip_ = wasSyncing;
}

// A drag finished: rebuild the clip's stack to match the list's new order.
void VideoEditorWindow::applyScriptOrderFromList()
{
	const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!scriptList_ || !sel)
		return;
	QStringList labels;
	labels.reserve(scriptList_->count());
	for (int row = 0; row < scriptList_->count(); ++row)
		labels << scriptList_->item(row)->text();

	const QVector<TlScript> reordered = reorderScriptsByLabel(labels, sel->scripts);
	if (reordered.isEmpty() || reordered == sel->scripts)
		return; // mid-drop, unreadable, or nothing actually moved
	scriptSel_ = scriptList_->currentRow();
	editSelectedClip([&](TlClip &c) { c.scripts = reordered; });
	refreshScriptList(); // renumber the labels for the new order
	rebuildScriptParams();
}

void VideoEditorWindow::addScriptToClip(const QString &name)
{
	if (name.isEmpty())
		return;
	QString err;
	if (!ensureScriptCompiled(name, &err)) {
		if (scriptError_) {
			scriptError_->setText(err);
			scriptError_->setVisible(true);
		}
		return;
	}
	if (scriptError_)
		scriptError_->setVisible(false);
	TlScript s;
	s.name = name;
	for (const ShaderParam &p : scriptParamDefs_.value(name))
		s.params[p.uniform] = p.def; // start from the declared defaults
	int added = 0;
	editSelectedClip([&](TlClip &c) {
		c.scripts.append(s);
		added = int(c.scripts.size()) - 1;
	});
	scriptSel_ = added; // select what was just added
	refreshScriptList();
	rebuildScriptParams();
}

void VideoEditorWindow::removeScriptFromClip(int index)
{
	const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!sel || index < 0 || index >= sel->scripts.size())
		return;
	editSelectedClip([index](TlClip &c) { c.scripts.removeAt(index); });
	scriptSel_ = index - 1; // fall back to the entry above the removed one
	refreshScriptList();
	rebuildScriptParams();
}

bool VideoEditorWindow::ensureScriptCompiled(const QString &name, QString *err)
{
	if (name.isEmpty() || !scriptEval_)
		return false;
	if (scriptEval_->has(name) && scriptParamDefs_.contains(name))
		return true;
	QFile f(scriptsDirPath() + QLatin1Char('/') + name + QStringLiteral(".js"));
	if (!f.open(QIODevice::ReadOnly)) {
		if (err)
			*err = QStringLiteral("Could not read %1.js").arg(name);
		return false;
	}
	const QString src = QString::fromUtf8(f.readAll());
	f.close();
	scriptParamDefs_.insert(name, parseShaderParams(src)); // same //@param format
	invalidatePreviewCache(); // frames made before this script existed are not its frames
	return scriptEval_->compile(name, src, err);
}

void VideoEditorWindow::rebuildScriptParams()
{
	if (!scriptParamBox_)
		return;
	auto *form = qobject_cast<QFormLayout *>(scriptParamBox_->layout());
	if (!form)
		return;

	const TlClip *c = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	// Controls belong to ONE entry in the stack: the selected row. Two entries can
	// run the same script with different values, so the identity that decides
	// whether a rebuild is needed is (row, name), not the name alone.
	const bool haveSel = c && scriptSel_ >= 0 && scriptSel_ < c->scripts.size();
	const QString name = haveSel ? c->scripts[scriptSel_].name : QString();
	const QString want = haveSel ? QStringLiteral("%1/%2").arg(scriptSel_).arg(name) : QString();

	// The controls only need rebuilding when that identity changes. Doing it on
	// every refresh would destroy the very slider the user is dragging, and this
	// runs once per mouse-move while reframing a clip in the preview.
	if (want == scriptParamsBuiltFor_) {
		if (!haveSel)
			return;
		const TlScript &entry = c->scripts[scriptSel_];
		for (const ShaderParam &p : scriptParamDefs_.value(name)) {
			const double val = entry.params.value(p.uniform, p.def);
			if (QCheckBox *cb = scriptParamChecks_.value(p.uniform)) {
				const QSignalBlocker b(cb);
				cb->setChecked(val != 0.0);
			}
			if (QSlider *sl = scriptParamSliders_.value(p.uniform)) {
				const double span = (p.max > p.min) ? (p.max - p.min) : 1.0;
				const QSignalBlocker b(sl);
				sl->setValue(int(std::clamp((val - p.min) / span, 0.0, 1.0) * 1000.0));
			}
			if (QLabel *vl = scriptParamValues_.value(p.uniform))
				vl->setText(QString::number(val, 'g', 3));
		}
		return;
	}

	while (form->rowCount() > 0)
		form->removeRow(0);
	scriptParamSliders_.clear();
	scriptParamValues_.clear();
	scriptParamChecks_.clear();
	scriptParamsBuiltFor_ = want;

	if (!haveSel)
		return;
	const int idx = scriptSel_;
	for (const ShaderParam &p : scriptParamDefs_.value(name)) {
		const double val = c->scripts[idx].params.value(p.uniform, p.def);
		auto *key = new QLabel(p.label, scriptParamBox_);
		key->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
		if (p.type == ShaderParam::Type::Bool) {
			auto *cb = new QCheckBox(scriptParamBox_);
			cb->setChecked(val != 0.0);
			connect(cb, &QCheckBox::toggled, this, [this, idx, u = p.uniform](bool on) {
				if (syncingClip_)
					return;
				editSelectedClip([&](TlClip &cl) {
					if (idx < cl.scripts.size())
						cl.scripts[idx].params[u] = on ? 1.0 : 0.0;
				});
			});
			scriptParamChecks_.insert(p.uniform, cb);
			form->addRow(key, cb);
		} else {
			auto *row = new QWidget(scriptParamBox_);
			auto *rl = new QHBoxLayout(row);
			rl->setContentsMargins(0, 0, 0, 0);
			rl->setSpacing(6);
			auto *sl = new QSlider(Qt::Horizontal, row);
			sl->setRange(0, 1000);
			const double span = (p.max > p.min) ? (p.max - p.min) : 1.0;
			sl->setValue(int(std::clamp((val - p.min) / span, 0.0, 1.0) * 1000.0));
			auto *vlab = new QLabel(QString::number(val, 'g', 3), row);
			vlab->setMinimumWidth(40);
			vlab->setStyleSheet(QStringLiteral("color:#c8ccd4; font-family:monospace;"));
			rl->addWidget(sl, 1);
			rl->addWidget(vlab);
			connect(sl, &QSlider::valueChanged, this,
				[this, idx, u = p.uniform, mn = p.min, sp = span, vlab](int v) {
					if (syncingClip_)
						return;
					const double d = mn + (double(v) / 1000.0) * sp;
					vlab->setText(QString::number(d, 'g', 3));
					editSelectedClip([&](TlClip &cl) {
						if (idx < cl.scripts.size())
							cl.scripts[idx].params[u] = d;
					});
				});
			scriptParamSliders_.insert(p.uniform, sl);
			scriptParamValues_.insert(p.uniform, vlab);
			form->addRow(key, row);
		}
	}
}

void VideoEditorWindow::reloadScriptsFromDisk()
{
	if (!scriptEval_)
		return;
	// Forget everything so the next evaluation recompiles from the edited files.
	for (const QString &n : scriptParamDefs_.keys())
		scriptEval_->forget(n);
	scriptParamDefs_.clear();
	scriptScanDirty_ = true; // everything must be recompiled on the next frame
	QString err;
	if (const TlClip *c = timelineView_ ? timelineView_->selectedClipPtr() : nullptr; c)
		for (const TlScript &sc : c->scripts)
			if (!sc.name.isEmpty() && !ensureScriptCompiled(sc.name, &err))
				break; // report the first failure; the rest retry lazily
	// Any other clip's scripts recompile lazily on their next frame.
	if (scriptError_) {
		scriptError_->setText(err);
		scriptError_->setVisible(!err.isEmpty());
	}
	rebuildScriptParams();
	showTimelineFrame(timelinePlayheadMs());
}

// ---- Full-editing clip inspector ----------------------------------------

namespace {
// A swatch button that opens a colour picker.
void styleSwatch(QPushButton *b, const QColor &c)
{
	const QString css = QStringLiteral("QPushButton{background:%1;border:1px solid #4a4f57;"
					  "min-width:36px;min-height:18px;}").arg(c.name(QColor::HexRgb));
	if (b->styleSheet() != css)
		b->setStyleSheet(css);
}
} // namespace

// Everything in the Inspector has to fit its narrowest width (the splitter's
// floor), or the tab grows a horizontal scrollbar and the right edge of every
// row is cut off. Two kinds of control set that floor too high on their own:
// a form row (label beside field) and a drop-down sized to its longest entry.
// Rows now wrap (the field goes under its label when they will not fit side
// by side) and drop-downs take their width from the panel, eliding the rest.
void VideoEditorWindow::fitInspectorToWidth()
{
	if (!insTabs_)
		return;
	for (QFormLayout *f : insTabs_->findChildren<QFormLayout *>())
		f->setRowWrapPolicy(QFormLayout::WrapLongRows);
	for (QComboBox *c : insTabs_->findChildren<QComboBox *>()) {
		c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		c->setMinimumContentsLength(3);
	}
}

QWidget *VideoEditorWindow::addSection(QVBoxLayout *into, const QString &title, bool expanded)
{
	// Remembered per title: see InspectorSection.
	auto *sec = new InspectorSection(title, QStringLiteral("project/") + title, expanded, this);
	into->addWidget(sec);
	return sec->body();
}

namespace {
// Recursive size of a folder (used for the project's assets).
qint64 dirSize(const QString &path)
{
	qint64 total = 0;
	QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
	while (it.hasNext()) {
		it.next();
		total += it.fileInfo().size();
	}
	return total;
}
} // namespace

namespace {

// The output-format presets. Both lists are ordered the way an editor thinks
// about them — the common ones first, the specialist ones after — and both are
// bracketed by "Auto" at the top and "Custom" at the bottom.
struct ResPreset {
	const char *label;
	int w, h;
};
const ResPreset kResPresets[] = {
	{"3840 × 2160  (4K UHD)", 3840, 2160},
	{"2560 × 1440  (QHD)", 2560, 1440},
	{"1920 × 1080  (Full HD)", 1920, 1080},
	{"1280 × 720  (HD)", 1280, 720},
	{"854 × 480  (SD)", 854, 480},
	{"1080 × 1920  (Vertical 9:16)", 1080, 1920},
	{"720 × 1280  (Vertical 720)", 720, 1280},
	{"1080 × 1080  (Square 1:1)", 1080, 1080},
	{"1080 × 1350  (Portrait 4:5)", 1080, 1350},
	{"2560 × 1080  (Ultrawide 21:9)", 2560, 1080},
};
constexpr int kResPresetCount = int(sizeof(kResPresets) / sizeof(kResPresets[0]));

struct FpsPreset {
	const char *label;
	double fps;
};
const FpsPreset kFpsPresets[] = {
	{"23.976  (film, NTSC)", 24000.0 / 1001.0},
	{"24  (film)", 24.0},
	{"25  (PAL)", 25.0},
	{"29.97  (NTSC)", 30000.0 / 1001.0},
	{"30", 30.0},
	{"48", 48.0},
	{"50  (PAL HD)", 50.0},
	{"59.94  (NTSC HD)", 60000.0 / 1001.0},
	{"60", 60.0},
	{"120", 120.0},
};
constexpr int kFpsPresetCount = int(sizeof(kFpsPresets) / sizeof(kFpsPresets[0]));

// "1920 × 1080" -> "16:9". Reduced by the greatest common divisor, and the
// handful of ratios that reduce to something unhelpful (683:384 for 2048×1152)
// are snapped to the name people actually use.
QString aspectLabel(QSize s)
{
	if (s.width() <= 0 || s.height() <= 0)
		return QStringLiteral("—");
	const double r = double(s.width()) / double(s.height());
	struct Named {
		const char *name;
		double ratio;
	};
	static const Named known[] = {{"16:9", 16.0 / 9.0},  {"9:16", 9.0 / 16.0},
				      {"4:3", 4.0 / 3.0},    {"3:4", 3.0 / 4.0},
				      {"1:1", 1.0},          {"21:9", 21.0 / 9.0},
				      {"4:5", 4.0 / 5.0},    {"5:4", 5.0 / 4.0},
				      {"3:2", 3.0 / 2.0},    {"2:3", 2.0 / 3.0}};
	for (const Named &n : known)
		if (std::abs(r - n.ratio) < 0.01)
			return QString::fromLatin1(n.name);
	const int g = std::gcd(s.width(), s.height());
	return QStringLiteral("%1:%2").arg(s.width() / g).arg(s.height() / g);
}

// 29.97 is 30000/1001, and showing it as "29.97" is right while showing
// "30.00" for it is not — so trailing zeros go, and the rest keeps two places.
QString fpsLabel(double fps)
{
	QString s = QString::number(fps, 'f', 3);
	while (s.endsWith(QLatin1Char('0')))
		s.chop(1);
	if (s.endsWith(QLatin1Char('.')))
		s.chop(1);
	return s;
}

// Hiding a widget inside a QFormLayout leaves its label sitting there on its
// own, so a row has to be hidden AS a row.
//
// The form is passed in rather than looked up from the widget: this form is a
// SUB-layout of the section's vertical layout, so parentWidget()->layout()
// answers with the vertical one and the row is never found. The row index
// itself is looked up each time, so inserting a row above cannot stale it.
void setFormRowVisible(QFormLayout *f, QWidget *w, bool on)
{
	if (!f || !w)
		return;
	int row = -1;
	QFormLayout::ItemRole role{};
	f->getWidgetPosition(w, &row, &role);
	if (row >= 0)
		f->setRowVisible(row, on);
}

} // namespace

void VideoEditorWindow::buildProjectInspector(QVBoxLayout *into)
{
	QWidget *body = addSection(into, QStringLiteral("Project"), true);
	auto *v = qobject_cast<QVBoxLayout *>(body->layout());
	projectBox_ = body;

	auto *form = new QFormLayout;
	form->setContentsMargins(0, 0, 0, 0);
	form->setHorizontalSpacing(8);
	form->setVerticalSpacing(3);
	form->setLabelAlignment(Qt::AlignLeft);
	auto mkKey = [this](const QString &t) {
		auto *l = new QLabel(t, this);
		l->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
		return l;
	};
	auto mkVal = [this](bool wrap = false) {
		auto *l = new QLabel(QStringLiteral("—"), this);
		l->setStyleSheet(QStringLiteral("color:#e8eaed;"));
		l->setTextInteractionFlags(Qt::TextSelectableByMouse);
		l->setWordWrap(wrap);
		return l;
	};
	pjName_ = mkVal();
	pjLocation_ = mkVal(true);
	pjCreated_ = mkVal();
	pjSaved_ = mkVal();
	pjVersion_ = mkVal();
	pjFormat_ = mkVal();
	pjSize_ = mkVal();
	pjAutosave_ = mkVal();
	pjAuthor_ = new QLineEdit(this);
	pjAuthor_->setPlaceholderText(QStringLiteral("(optional)"));
	connect(pjAuthor_, &QLineEdit::textEdited, this,
		[this](const QString &t) { projectAuthor_ = t; });

	form->addRow(mkKey(QStringLiteral("Name")), pjName_);
	form->addRow(mkKey(QStringLiteral("Location")), pjLocation_);
	form->addRow(mkKey(QStringLiteral("Created")), pjCreated_);
	form->addRow(mkKey(QStringLiteral("Last saved")), pjSaved_);
	form->addRow(mkKey(QStringLiteral("Author")), pjAuthor_);
	form->addRow(mkKey(QStringLiteral("Version")), pjVersion_);
	form->addRow(mkKey(QStringLiteral("Size")), pjSize_);
	form->addRow(mkKey(QStringLiteral("Autosave")), pjAutosave_);
	v->addLayout(form);

	// ---- Output format: resolution + frame rate --------------------------
	// Editable, unlike the rest of this section, because these two decide what
	// the preview and the export actually produce.
	auto *fmtHdr = new QLabel(QStringLiteral("Output"), this);
	fmtHdr->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed; margin-top:6px;"));
	v->addWidget(fmtHdr);

	auto *fmt = new QFormLayout;
	pjFmtForm_ = fmt; // kept so the Custom rows can be shown and hidden as rows
	fmt->setContentsMargins(0, 0, 0, 0);
	fmt->setHorizontalSpacing(8);
	fmt->setVerticalSpacing(3);
	fmt->setLabelAlignment(Qt::AlignLeft);

	pjResCombo_ = new QComboBox(this);
	pjResCombo_->addItem(QStringLiteral("Auto — match the first clip"));
	// fromUtf8, not fromLatin1: these labels contain a multiplication sign, and
	// Latin-1 decodes its two UTF-8 bytes as two separate characters.
	for (const ResPreset &p : kResPresets)
		pjResCombo_->addItem(QString::fromUtf8(p.label));
	pjResCombo_->addItem(QStringLiteral("Custom…"));
	pjResCombo_->setToolTip(QStringLiteral(
		"The canvas every clip is composited onto, and the size the export writes."));
	fmt->addRow(mkKey(QStringLiteral("Resolution")), pjResCombo_);

	// The two spin boxes only appear for Custom, so the common case is one row.
	pjResCustom_ = new QWidget(this);
	auto *rc = new QHBoxLayout(pjResCustom_);
	rc->setContentsMargins(0, 0, 0, 0);
	rc->setSpacing(4);
	pjResW_ = new QSpinBox(pjResCustom_);
	pjResH_ = new QSpinBox(pjResCustom_);
	for (QSpinBox *sb : {pjResW_, pjResH_}) {
		// Even numbers only: H.264 cannot encode an odd dimension with 4:2:0
		// chroma, so an odd value here would fail at export rather than here.
		sb->setRange(16, 7680);
		sb->setSingleStep(2);
		sb->setKeyboardTracking(false);
	}
	pjResW_->setToolTip(QStringLiteral("Width in pixels (even numbers only — H.264 requires it)"));
	pjResH_->setToolTip(QStringLiteral("Height in pixels (even numbers only)"));
	rc->addWidget(pjResW_);
	rc->addWidget(new QLabel(QStringLiteral("×"), pjResCustom_));
	rc->addWidget(pjResH_);
	fmt->addRow(mkKey(QString()), pjResCustom_);

	pjFpsCombo_ = new QComboBox(this);
	pjFpsCombo_->addItem(QStringLiteral("Auto — match the first clip"));
	for (const FpsPreset &p : kFpsPresets)
		pjFpsCombo_->addItem(QString::fromUtf8(p.label));
	pjFpsCombo_->addItem(QStringLiteral("Custom…"));
	pjFpsCombo_->setToolTip(QStringLiteral(
		"Frames per second for the export, and the step the arrow keys nudge the playhead by."));
	fmt->addRow(mkKey(QStringLiteral("Frame rate")), pjFpsCombo_);

	pjFpsSpin_ = new QDoubleSpinBox(this);
	pjFpsSpin_->setRange(1.0, 240.0);
	pjFpsSpin_->setDecimals(3);
	pjFpsSpin_->setSingleStep(1.0);
	pjFpsSpin_->setSuffix(QStringLiteral(" fps"));
	pjFpsSpin_->setKeyboardTracking(false);
	fmt->addRow(mkKey(QString()), pjFpsSpin_);

	pjAspect_ = mkVal();
	fmt->addRow(mkKey(QStringLiteral("Aspect")), pjAspect_);
	pjFormat_ = mkVal(true); // "1920 × 1080 @ 30 fps (auto)" is wider than the narrowest panel
	fmt->addRow(mkKey(QStringLiteral("Effective")), pjFormat_);
	v->addLayout(fmt);

	pjFormatWarn_ = new QLabel(this);
	pjFormatWarn_->setWordWrap(true);
	pjFormatWarn_->setStyleSheet(QStringLiteral("color:#e2a03f; font-size:%1px;").arg(uiCaptionPx()));
	pjFormatWarn_->setVisible(false);
	v->addWidget(pjFormatWarn_);

	auto *fmtRow = new QHBoxLayout;
	auto *matchBtn = new QPushButton(QStringLiteral("Match first clip"), this);
	matchBtn->setToolTip(QStringLiteral("Pin the canvas and frame rate to what the first "
					    "clip actually is, instead of following it."));
	fmtRow->addWidget(matchBtn);
	fmtRow->addStretch(1);
	v->addLayout(fmtRow);
	connect(matchBtn, &QPushButton::clicked, this, [this]() {
		// Freeze what Auto is resolving to right now, rather than leaving it
		// to change when another clip becomes the first one.
		projCanvas_ = timelineCanvasSize();
		projFps_ = timelineFps();
		projResCustom_ = projFpsCustom_ = false; // show the preset if one fits
		syncProjectFormatControls();
		applyProjectFormat();
	});

	auto onRes = [this](int) {
		if (syncingProject_)
			return;
		const int idx = pjResCombo_->currentIndex();
		projResCustom_ = (idx == kResPresetCount + 1);
		if (idx == 0) {
			projCanvas_ = QSize(); // auto
		} else if (idx == kResPresetCount + 1) {
			// Entering Custom keeps whatever is on screen, so the boxes start
			// from the size you were just looking at instead of jumping.
			const QSize cur = timelineCanvasSize();
			projCanvas_ = QSize(pjResW_->value() > 16 ? pjResW_->value() : cur.width(),
					    pjResH_->value() > 16 ? pjResH_->value() : cur.height());
		} else {
			const ResPreset &p = kResPresets[idx - 1];
			projCanvas_ = QSize(p.w, p.h);
		}
		syncProjectFormatControls();
		applyProjectFormat();
	};
	connect(pjResCombo_, &QComboBox::currentIndexChanged, this, onRes);

	auto onCustomRes = [this]() {
		if (syncingProject_)
			return;
		// Round to even here rather than rejecting the keystroke: typing "1081"
		// should land on something valid, not refuse to accept the digit.
		projCanvas_ = QSize(pjResW_->value() & ~1, pjResH_->value() & ~1);
		syncProjectFormatControls();
		applyProjectFormat();
	};
	connect(pjResW_, &QSpinBox::valueChanged, this, onCustomRes);
	connect(pjResH_, &QSpinBox::valueChanged, this, onCustomRes);

	connect(pjFpsCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
		if (syncingProject_)
			return;
		projFpsCustom_ = (idx == kFpsPresetCount + 1);
		if (idx == 0)
			projFps_ = 0.0; // auto
		else if (idx == kFpsPresetCount + 1)
			projFps_ = pjFpsSpin_->value();
		else
			projFps_ = kFpsPresets[idx - 1].fps;
		syncProjectFormatControls();
		applyProjectFormat();
	});
	connect(pjFpsSpin_, &QDoubleSpinBox::valueChanged, this, [this](double f) {
		if (syncingProject_)
			return;
		projFps_ = f;
		syncProjectFormatControls();
		applyProjectFormat();
	});

	autosaveChk_ = new QCheckBox(QStringLiteral("Autosave every 5 min"), this);
	autosaveChk_->setToolTip(QStringLiteral(
		"Writes a separate <project>_autosave.harpiaproj beside your project — your own file "
		"is never overwritten automatically."));
	connect(autosaveChk_, &QCheckBox::toggled, this, [this](bool on) {
		QSettings().setValue(QStringLiteral("editor/autosave"), on);
		if (on)
			autosaveTimer_->start();
		else
			autosaveTimer_->stop();
		refreshProjectInspector();
	});
	v->addWidget(autosaveChk_);

	auto *row1 = new QHBoxLayout;
	auto *saveBtn = new QPushButton(QStringLiteral("Save"), this);
	auto *saveAsBtn = new QPushButton(QStringLiteral("Save As…"), this);
	row1->addWidget(saveBtn);
	row1->addWidget(saveAsBtn);
	v->addLayout(row1);
	auto *row2 = new QHBoxLayout;
	auto *revealBtn = new QPushButton(QStringLiteral("Reveal folder"), this);
	auto *copyBtn = new QPushButton(QStringLiteral("Copy path"), this);
	row2->addWidget(revealBtn);
	row2->addWidget(copyBtn);
	v->addLayout(row2);
	connect(saveBtn, &QPushButton::clicked, this, &VideoEditorWindow::onSaveProject);
	connect(saveAsBtn, &QPushButton::clicked, this, &VideoEditorWindow::onSaveProjectAs);
	connect(revealBtn, &QPushButton::clicked, this, &VideoEditorWindow::revealProjectFolder);
	connect(copyBtn, &QPushButton::clicked, this, [this]() {
		if (!projectPath_.isEmpty())
			QApplication::clipboard()->setText(QDir::toNativeSeparators(projectPath_));
	});

	// Autosave writes a sidecar file, so it can never clobber the user's project.
	autosaveTimer_ = new QTimer(this);
	autosaveTimer_->setInterval(5 * 60 * 1000);
	connect(autosaveTimer_, &QTimer::timeout, this, &VideoEditorWindow::doAutosave);
	const bool wantAutosave = QSettings().value(QStringLiteral("editor/autosave"), false).toBool();
	autosaveChk_->setChecked(wantAutosave);
	if (wantAutosave)
		autosaveTimer_->start();

	// A new project starts on whatever format you last chose, because the next
	// video is usually for the same place as the last one. Auto stays the
	// default until you pick something, and a project you OPEN overrides this.
	const QSettings st;
	const int rw = st.value(QStringLiteral("editor/projectW"), 0).toInt();
	const int rh = st.value(QStringLiteral("editor/projectH"), 0).toInt();
	if (rw >= 16 && rh >= 16)
		projCanvas_ = QSize(rw & ~1, rh & ~1);
	const double sf = st.value(QStringLiteral("editor/projectFps"), 0.0).toDouble();
	if (sf >= 1.0 && sf <= 240.0)
		projFps_ = sf;
	syncProjectFormatControls();
}

// Push the current resolution/frame rate into the widgets, and work out
// everything derived from them (which rows are visible, the aspect, the
// warnings). One function so the controls cannot disagree with the values.
void VideoEditorWindow::syncProjectFormatControls()
{
	if (!pjResCombo_)
		return;
	const QSignalBlocker b1(pjResCombo_), b2(pjResW_), b3(pjResH_), b4(pjFpsCombo_),
		b5(pjFpsSpin_);
	syncingProject_ = true;

	// --- resolution
	int resIdx = 0; // Auto
	if (projCanvas_.isValid() && !projCanvas_.isEmpty()) {
		resIdx = kResPresetCount + 1; // Custom, unless a preset matches exactly
		if (!projResCustom_)
			for (int i = 0; i < kResPresetCount; ++i)
				if (kResPresets[i].w == projCanvas_.width() &&
				    kResPresets[i].h == projCanvas_.height()) {
					resIdx = i + 1;
					break;
				}
	}
	pjResCombo_->setCurrentIndex(resIdx);
	const QSize eff = timelineCanvasSize();
	pjResW_->setValue(eff.width());
	pjResH_->setValue(eff.height());
	setFormRowVisible(pjFmtForm_, pjResCustom_, resIdx == kResPresetCount + 1);

	// --- frame rate
	int fpsIdx = 0;
	if (projFps_ >= 1.0) {
		fpsIdx = kFpsPresetCount + 1;
		if (!projFpsCustom_)
			for (int i = 0; i < kFpsPresetCount; ++i)
				if (std::abs(kFpsPresets[i].fps - projFps_) < 0.005) {
					fpsIdx = i + 1;
					break;
				}
	}
	pjFpsCombo_->setCurrentIndex(fpsIdx);
	pjFpsSpin_->setValue(timelineFps());
	setFormRowVisible(pjFmtForm_, pjFpsSpin_, fpsIdx == kFpsPresetCount + 1);

	// --- derived readouts
	pjAspect_->setText(QStringLiteral("%1   (%2 × %3)")
				   .arg(aspectLabel(eff))
				   .arg(eff.width())
				   .arg(eff.height()));
	pjFormat_->setText(QStringLiteral("%1 × %2 @ %3 fps%4")
				   .arg(eff.width())
				   .arg(eff.height())
				   .arg(fpsLabel(timelineFps()))
				   .arg(projCanvas_.isValid() || projFps_ >= 1.0 ? QString()
										: QStringLiteral(" (auto)")));

	// --- warnings, worth saying because they cost quality rather than failing
	QStringList warn;
	if (!sources_.empty() && sources_.front().width > 0 && sources_.front().height > 0) {
		const QSize src(sources_.front().width, sources_.front().height);
		if (aspectLabel(src) != aspectLabel(eff))
			warn << QStringLiteral("Your first clip is %1 (%2×%3) — it will be "
					       "letterboxed onto a %4 canvas.")
					.arg(aspectLabel(src))
					.arg(src.width())
					.arg(src.height())
					.arg(aspectLabel(eff));
		if (eff.width() > src.width() * 2)
			warn << QStringLiteral("The canvas is more than twice your source's width; "
					       "the extra pixels are upscaled, not real detail.");
	}
	if (eff.width() % 2 || eff.height() % 2)
		warn << QStringLiteral("Odd dimensions cannot be encoded as H.264.");
	pjFormatWarn_->setText(warn.join(QLatin1Char('\n')));
	pjFormatWarn_->setVisible(!warn.isEmpty());

	syncingProject_ = false;
}

// A format change moves everything downstream: the compositor's canvas, the
// preview render size, the timeline's frame step, and what export will write.
void VideoEditorWindow::applyProjectFormat()
{
	QSettings st;
	if (projCanvas_.isValid() && !projCanvas_.isEmpty()) {
		st.setValue(QStringLiteral("editor/projectW"), projCanvas_.width());
		st.setValue(QStringLiteral("editor/projectH"), projCanvas_.height());
	} else {
		st.remove(QStringLiteral("editor/projectW"));
		st.remove(QStringLiteral("editor/projectH"));
	}
	if (projFps_ >= 1.0)
		st.setValue(QStringLiteral("editor/projectFps"), projFps_);
	else
		st.remove(QStringLiteral("editor/projectFps"));

	if (timelineView_)
		timelineView_->setFrameRate(timelineFps());

	// Re-COMPOSE, not just re-shade. refreshPreviewFrame() runs the effect chain
	// over the last composited frame, which is still the old canvas — so the
	// readouts would change and the picture would not, which is the one thing
	// this panel exists to show you.
	//
	// The canvas is told its new size first: it letterboxes to that aspect, and
	// the crop rectangle and the preview-drag maths are expressed against it.
	//
	// previewCanvasSize(), not timelineCanvasSize(): the project resolution is
	// the FULL EDITING output. Simple Trim writes the active source at its own
	// size and Multi-Cut takes its shape from the primary clip, so handing
	// either of them the project's shape made the preview claim a frame the
	// export would never produce -- and the picture was then drawn stretched
	// into it. Setting a 9:16 project while trimming a 16:9 clip is exactly
	// that case.
	const QSize c = previewCanvasSize();
	if (canvas_)
		canvas_->setVideoSize(c.width(), c.height());
	if (fullEdit())
		showTimelineFrame(timelinePlayheadMs());
	else
		refreshPreviewFrame();
	refreshProjectInspector();
}

void VideoEditorWindow::refreshProjectInspector()
{
	if (!pjName_)
		return;
	const bool saved = !projectPath_.isEmpty();
	const QFileInfo fi(projectPath_);
	pjName_->setText(saved ? fi.completeBaseName() : QStringLiteral("Untitled (not saved yet)"));
	pjLocation_->setText(saved ? QDir::toNativeSeparators(fi.absolutePath())
				   : QStringLiteral("—"));
	pjCreated_->setText(projectCreated_.isValid()
				    ? projectCreated_.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
				    : QStringLiteral("—"));
	pjSaved_->setText(saved ? fi.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm"))
				: QStringLiteral("Never"));
	pjAuthor_->setText(projectAuthor_);
	pjVersion_->setText(QStringLiteral("Harpia project v3 · app %1").arg(appVersion()));


	// Project size = the project file + its assets folder (voiceover takes).
	qint64 bytes = 0;
	if (saved) {
		bytes += fi.size();
		const QString assets = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() +
				       QStringLiteral("_assets");
		if (QFileInfo::exists(assets))
			bytes += dirSize(assets);
	}
	qint64 media = 0;
	for (const EditorSource &s : sources_)
		media += QFileInfo(s.path).size();
	pjSize_->setText(saved ? QStringLiteral("%1  (media %2)")
					 .arg(humanFileSize(bytes), humanFileSize(media))
			       : QStringLiteral("—  (media %1)").arg(humanFileSize(media)));

	// The Auto values and the letterbox warning both read the first source, so
	// they are recomputed whenever anything about the project is refreshed.
	if (!syncingProject_)
		syncProjectFormatControls();

	if (!autosaveChk_ || !autosaveChk_->isChecked())
		pjAutosave_->setText(QStringLiteral("Off"));
	else if (!saved)
		pjAutosave_->setText(QStringLiteral("On — waiting for a first save"));
	else if (lastAutosave_.isValid())
		pjAutosave_->setText(
			QStringLiteral("On — last %1").arg(lastAutosave_.toString(QStringLiteral("HH:mm"))));
	else
		pjAutosave_->setText(QStringLiteral("On"));
}

void VideoEditorWindow::doAutosave()
{
	if (loadingDialog_)
		return;
	// Only once we know where the project lives, and never onto that file itself.
	if (!valid_ || projectPath_.isEmpty())
		return;
	const QFileInfo fi(projectPath_);
	const QString side = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() +
			     QStringLiteral("_autosave.harpiaproj");
	saveProjectTo(side, /*quiet=*/true);
	refreshProjectInspector();
}

void VideoEditorWindow::revealProjectFolder()
{
	const QString target = projectPath_.isEmpty() ? inPath_ : projectPath_;
	if (target.isEmpty())
		return;
	revealInFolder(target);
}

// The selected transition. There is no duration control: the overlap IS the
// duration, so it is reported rather than edited — you change it by dragging
// either clip, which is the whole point of the Vegas model.
void VideoEditorWindow::buildTransitionInspector(QVBoxLayout *into)
{
	trBox_ = new QWidget(this);
	trBox_->setVisible(false);
	auto *v = new QVBoxLayout(trBox_);
	v->setContentsMargins(0, 6, 0, 0);
	v->setSpacing(5);

	auto *hdr = new QLabel(QStringLiteral("Transition"), trBox_);
	hdr->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed;"));
	v->addWidget(hdr);
	trInfo_ = new QLabel(trBox_);
	trInfo_->setWordWrap(true);
	trInfo_->setStyleSheet(QStringLiteral("color:#7f858e;"));
	v->addWidget(trInfo_);

	auto *form = new QFormLayout;
	form->setHorizontalSpacing(8);
	form->setVerticalSpacing(4);

	trType_ = new QComboBox(trBox_);
	for (int i = 0; i < kTransitionTypeCount; ++i)
		trType_->addItem(QString::fromLatin1(transitionName(TransitionType(i))));
	form->addRow(QStringLiteral("Type"), trType_);
	connect(trType_, &QComboBox::currentIndexChanged, this, [this](int idx) {
		if (syncingTr_)
			return;
		// Changing the type keeps the overlap, so the transition's length is
		// unaffected — exactly as asked.
		const TransitionType t = transitionFromInt(idx);
		editSelectedTransition([t](TlTransition &tr) { tr.type = t; });
	});

	auto mkEase = [&](const QString &label, QComboBox *&out, bool incoming) {
		out = new QComboBox(trBox_);
		for (int i = 0; i < kTlEaseCount; ++i)
			out->addItem(QString::fromLatin1(tlEaseName(TlEase(i))));
		form->addRow(label, out);
		connect(out, &QComboBox::currentIndexChanged, this, [this, incoming](int idx) {
			if (syncingTr_)
				return;
			const TlEase e = tlEaseFromInt(idx);
			editSelectedTransition([e, incoming](TlTransition &tr) {
				(incoming ? tr.easeIn : tr.easeOut) = e;
			});
		});
	};
	mkEase(QStringLiteral("Outgoing curve"), trEaseOut_, false);
	mkEase(QStringLiteral("Incoming curve"), trEaseIn_, true);

	trSoftness_ = new QDoubleSpinBox(trBox_);
	trSoftness_->setRange(0.0, 1.0);
	trSoftness_->setSingleStep(0.05);
	trSoftness_->setDecimals(2);
	trSoftness_->setKeyboardTracking(false);
	trSoftness_->setToolTip(QStringLiteral("Softens the moving edge. Only the types with "
					       "an edge (wipes, iris, circle) use it."));
	form->addRow(QStringLiteral("Softness"), trSoftness_);
	connect(trSoftness_, &QDoubleSpinBox::valueChanged, this, [this](double d) {
		if (syncingTr_)
			return;
		editSelectedTransition([d](TlTransition &tr) { tr.softness = d; });
	});

	trReverse_ = new QCheckBox(QStringLiteral("Reverse"), trBox_);
	form->addRow(QString(), trReverse_);
	connect(trReverse_, &QCheckBox::toggled, this, [this](bool on) {
		if (syncingTr_)
			return;
		editSelectedTransition([on](TlTransition &tr) { tr.reverse = on; });
	});
	trEnabled_ = new QCheckBox(QStringLiteral("Enabled"), trBox_);
	trEnabled_->setToolTip(QStringLiteral(
		"Turn the transition off without moving the clips: the overlap stays, but the "
		"later clip simply covers the earlier one."));
	form->addRow(QString(), trEnabled_);
	connect(trEnabled_, &QCheckBox::toggled, this, [this](bool on) {
		if (syncingTr_)
			return;
		editSelectedTransition([on](TlTransition &tr) { tr.enabled = on; });
	});
	v->addLayout(form);

	trRemove_ = new QPushButton(QStringLiteral("Remove transition (snap clips together)"),
				    trBox_);
	connect(trRemove_, &QPushButton::clicked, this, [this]() {
		if (timelineView_)
			timelineView_->removeSelectedTransition();
	});
	v->addWidget(trRemove_);

	into->addWidget(trBox_);
}

void VideoEditorWindow::editSelectedTransition(const std::function<void(TlTransition &)> &fn)
{
	if (!timelineView_ || !timelineView_->transitionSelected())
		return;
	const int t = timelineView_->selectedTransitionTrack();
	const int ci = timelineView_->selectedTransitionClip();
	TimelineModel m = timelineView_->model();
	if (t < 0 || t >= m.tracks.size() || ci < 0 || ci >= m.tracks[t].clips.size())
		return;
	fn(m.tracks[t].clips[ci].transition);
	timelineView_->setModel(m);
	// setModel drops the selection, so put the user back where they were.
	timelineView_->selectClip(t, ci);
	syncTransitionInspector();
	showTimelineFrame(timelinePlayheadMs());
	commitSnapshot();
}

void VideoEditorWindow::syncTransitionInspector()
{
	if (!trBox_ || !timelineView_)
		return;
	const bool on = fullEdit() && timelineView_->transitionSelected();
	trBox_->setVisible(on);
	if (!on)
		return;
	const int t = timelineView_->selectedTransitionTrack();
	const int ci = timelineView_->selectedTransitionClip();
	const auto &tracks = timelineView_->model().tracks;
	if (t < 0 || t >= tracks.size() || ci < 0 || ci >= tracks[t].clips.size())
		return;
	const TlClip &c = tracks[t].clips[ci];
	const qint64 span = tracks[t].overlapBefore(ci);

	const bool was = syncingTr_;
	syncingTr_ = true;
	trInfo_->setText(QStringLiteral(
				 "%1 s — the length of the overlap. Drag either clip to change it; "
				 "pull them apart and the transition goes away.")
				 .arg(span / 1000.0, 0, 'f', 2));
	trType_->setCurrentIndex(int(c.transition.type));
	trEaseOut_->setCurrentIndex(int(c.transition.easeOut));
	trEaseIn_->setCurrentIndex(int(c.transition.easeIn));
	trReverse_->setChecked(c.transition.reverse);
	trEnabled_->setChecked(c.transition.enabled);
	trSoftness_->setValue(c.transition.softness);
	trSoftness_->setEnabled(Transitions::hasSoftEdge(c.transition.type));
	syncingTr_ = was;
}




void VideoEditorWindow::buildSpotlightInspector(QVBoxLayout *into)
{
	spotBox_ = new QWidget(this);
	auto *v = new QVBoxLayout(spotBox_);
	v->setContentsMargins(0, 6, 0, 0);
	v->setSpacing(5);

	// Both of these are rewritten by syncSpotlightInspector to name whichever
	// of the two subjects is currently being edited.
	spotHdr_ = new QLabel(QStringLiteral("Inverse Selection"), spotBox_);
	spotHdr_->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed;"));
	v->addWidget(spotHdr_);
	v->addWidget(infoHint(QStringLiteral("Dim everything except the chosen areas — the tutorial "
					     "spotlight. Renders identically in the export."),
			      spotBox_),
		     0, Qt::AlignLeft);
	spotScopeHint_ = new QLabel(spotBox_);
	spotScopeHint_->setWordWrap(true);
	spotScopeHint_->setStyleSheet(QStringLiteral("color:#7f858e;"));
	v->addWidget(spotScopeHint_);

	spotOn_ = new QCheckBox(QStringLiteral("Enabled"), spotBox_);
	v->addWidget(spotOn_);
	connect(spotOn_, &QCheckBox::toggled, this, [this](bool on) {
		if (syncingSpot_)
			return;
		editSpotlight([on](SpotlightSpec &s) { s.enabled = on; });
	});
	spotInvert_ = new QCheckBox(QStringLiteral("Invert (dim inside instead)"), spotBox_);
	v->addWidget(spotInvert_);
	connect(spotInvert_, &QCheckBox::toggled, this, [this](bool on) {
		if (syncingSpot_)
			return;
		editSpotlight([on](SpotlightSpec &s) { s.invert = on; });
	});

	auto mkSpin = [this](double lo, double hi, double step, int dec) {
		auto *sp = new QDoubleSpinBox(spotBox_);
		sp->setRange(lo, hi);
		sp->setSingleStep(step);
		sp->setDecimals(dec);
		sp->setKeyboardTracking(false);
		return sp;
	};

	auto *gf = new QFormLayout;
	gf->setHorizontalSpacing(8);
	gf->setVerticalSpacing(4);
	spotDim_ = mkSpin(0.0, 1.0, 0.05, 2);
	gf->addRow(QStringLiteral("Dim"), spotDim_);
	connect(spotDim_, &QDoubleSpinBox::valueChanged, this, [this](double d) {
		if (syncingSpot_)
			return;
		editSpotlight([d](SpotlightSpec &s) { s.dimOpacity = d; });
	});
	spotBlur_ = mkSpin(0.0, 1.0, 0.05, 2);
	spotBlur_->setToolTip(QStringLiteral(
		"Blur the dimmed part. Costs real time per frame — leave it at 0 unless you "
		"want it."));
	gf->addRow(QStringLiteral("Blur"), spotBlur_);
	connect(spotBlur_, &QDoubleSpinBox::valueChanged, this, [this](double d) {
		if (syncingSpot_)
			return;
		editSpotlight([d](SpotlightSpec &s) { s.blur = d; });
	});
	spotColor_ = new QPushButton(spotBox_);
	gf->addRow(QStringLiteral("Colour"), spotColor_);
	connect(spotColor_, &QPushButton::clicked, this, [this]() {
		const QColor cur = timelineView_ ? spotTarget().dimColor
						 : QColor(Qt::black);
		// Live, like every other colour here: a dim colour is judged against
		// the picture it is dimming, not against the picker's own swatch.
		pickColorLive(this, QStringLiteral("Dim colour"), cur, [this](const QColor &c) {
			editSpotlight([c](SpotlightSpec &s) { s.dimColor = c; });
		});
	});
	v->addLayout(gf);

	// ---- the areas ----
	auto *areasHdr = new QLabel(QStringLiteral("Areas"), spotBox_);
	areasHdr->setStyleSheet(QStringLiteral("color:#c8ccd2;"));
	v->addWidget(areasHdr);

	spotList_ = new QListWidget(spotBox_);
	spotList_->setFixedHeight(90);
	v->addWidget(spotList_);
	connect(spotList_, &QListWidget::currentRowChanged, this,
		[this](int) { syncSpotlightInspector(); });
	// The checkbox in each row enables/disables that area on its own.
	connect(spotList_, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
		if (syncingSpot_ || !it)
			return;
		const int row = spotList_->row(it);
		const bool on = it->checkState() == Qt::Checked;
		editSpotlight([row, on](SpotlightSpec &s) {
			if (row >= 0 && row < s.masks.size())
				s.masks[row].enabled = on;
		});
	});

	auto *row = new QHBoxLayout;
	spotPreset_ = new QComboBox(spotBox_);
	for (const QString &n : Spotlight::presetNames())
		spotPreset_->addItem(n);
	row->addWidget(spotPreset_, 1);
	auto *addBtn = new QPushButton(QStringLiteral("Add"), spotBox_);
	connect(addBtn, &QPushButton::clicked, this, [this]() {
		const QString name = spotPreset_->currentText();
		editSpotlight([name](SpotlightSpec &s) {
			s.masks.append(Spotlight::preset(name));
			s.enabled = true; // adding the first area should just work
		});
		spotList_->setCurrentRow(spotList_->count() - 1);
	});
	row->addWidget(addBtn);
	auto *dupBtn = new QPushButton(QStringLiteral("Duplicate"), spotBox_);
	connect(dupBtn, &QPushButton::clicked, this, [this]() {
		const int r = selectedMaskRow();
		if (r < 0)
			return;
		editSpotlight([r](SpotlightSpec &s) {
			SpotMask m = s.masks[r];
			m.pose.cx = std::clamp(m.pose.cx + 0.05, 0.0, 1.0);
			m.pose.cy = std::clamp(m.pose.cy + 0.05, 0.0, 1.0);
			s.masks.insert(r + 1, m);
		});
	});
	row->addWidget(dupBtn);
	auto *delBtn = new QPushButton(QStringLiteral("Delete"), spotBox_);
	connect(delBtn, &QPushButton::clicked, this, [this]() {
		const int r = selectedMaskRow();
		if (r < 0)
			return;
		editSpotlight([r](SpotlightSpec &s) { s.masks.remove(r); });
	});
	row->addWidget(delBtn);
	v->addLayout(row);

	// ---- the selected area ----
	spotMaskBox_ = new QWidget(spotBox_);
	auto *mf = new QFormLayout(spotMaskBox_);
	mf->setContentsMargins(0, 0, 0, 0);
	mf->setHorizontalSpacing(8);
	mf->setVerticalSpacing(4);
	spotShape_ = new QComboBox(spotMaskBox_);
	for (int i = 0; i < kSpotShapeCount; ++i)
		spotShape_->addItem(QString::fromLatin1(spotShapeName(SpotShape(i))));
	mf->addRow(QStringLiteral("Shape"), spotShape_);
	connect(spotShape_, &QComboBox::currentIndexChanged, this, [this](int idx) {
		if (syncingSpot_)
			return;
		const int r = selectedMaskRow();
		if (r < 0)
			return;
		editSpotlight([r, idx](SpotlightSpec &s) { s.masks[r].shape = spotShapeFromInt(idx); });
	});
	spotX_ = mkSpin(-1.0, 2.0, 0.01, 3);
	spotY_ = mkSpin(-1.0, 2.0, 0.01, 3);
	auto *pos = new QWidget(spotMaskBox_);
	auto *ph = new QHBoxLayout(pos);
	ph->setContentsMargins(0, 0, 0, 0);
	ph->addWidget(spotX_, 1);
	ph->addWidget(spotY_, 1);
	mf->addRow(QStringLiteral("Centre"), pos);
	spotW_ = mkSpin(0.01, 4.0, 0.01, 3);
	spotH_ = mkSpin(0.01, 4.0, 0.01, 3);
	auto *sz = new QWidget(spotMaskBox_);
	auto *sh = new QHBoxLayout(sz);
	sh->setContentsMargins(0, 0, 0, 0);
	sh->addWidget(spotW_, 1);
	sh->addWidget(spotH_, 1);
	mf->addRow(QStringLiteral("Size"), sz);
	spotRot_ = mkSpin(-360.0, 360.0, 1.0, 1);
	spotRot_->setSuffix(QStringLiteral("°"));
	mf->addRow(QStringLiteral("Rotation"), spotRot_);
	spotRadius_ = mkSpin(0.0, 0.5, 0.01, 2);
	spotRadius_->setToolTip(QStringLiteral(
		"Corner radius as a fraction of the shorter side, so it scales with the shape."));
	mf->addRow(QStringLiteral("Corner radius"), spotRadius_);
	v->addWidget(spotMaskBox_);

	// A mask's pose can be animated (SpotKey) — and dragging a keyframed mask in
	// the preview writes one — but there was nowhere to see, retime or remove it.
	spotKeys_ = new KeyList(QStringLiteral("this area's position and size"), spotBox_);
	connect(spotKeys_, &KeyList::addRequested, this, [this]() {
		const int r = spotList_ ? spotList_->currentRow() : -1;
		const qint64 now = timelinePlayheadMs();
		editSpotlight([r, now](SpotlightSpec &sp) {
			if (r >= 0 && r < sp.masks.size())
				sp.masks[r].setKeyAt(now);
		});
	});
	connect(spotKeys_, &KeyList::removeRequested, this, [this](qint64 ms) {
		const int r = spotList_ ? spotList_->currentRow() : -1;
		editSpotlight([r, ms](SpotlightSpec &sp) {
			if (r >= 0 && r < sp.masks.size())
				sp.masks[r].removeKeyAt(ms);
		});
	});
	connect(spotKeys_, &KeyList::jumpRequested, this, [this](qint64 ms) {
		if (timelineView_)
			timelineView_->setPlayhead(ms);
		showTimelineFrame(timelinePlayheadMs());
		syncSpotlightInspector();
	});
	v->addWidget(spotKeys_);

	auto applyPose = [this]() {
		if (syncingSpot_)
			return;
		const int r = selectedMaskRow();
		if (r < 0)
			return;
		SpotPose p;
		p.cx = spotX_->value();
		p.cy = spotY_->value();
		p.w = spotW_->value();
		p.h = spotH_->value();
		p.rotation = spotRot_->value();
		p.radius = spotRadius_->value();
		const qint64 now = timelinePlayheadMs();
		editSpotlight([r, p, now](SpotlightSpec &s) {
			SpotMask &m = s.masks[r];
			// `visible` is only reachable from a keyframe, so the pose
			// panel must not stamp over whatever it currently holds.
			SpotPose q = p;
			if (m.keys.isEmpty()) {
				q.visible = m.pose.visible;
				m.pose = q;
				return;
			}
			// Once the mask is animated, `pose` is dead weight — poseAt()
			// answers from the keys and never looks at it. Writing there
			// would make typing a number do nothing at all, which is what it
			// used to do. Edit the key at the playhead instead, adding one if
			// there is none, so the fields stay live.
			for (SpotKey &k : m.keys)
				if (k.tMs == now) {
					q.visible = k.pose.visible;
					k.pose = q;
					return;
				}
			SpotKey k;
			k.tMs = now;
			k.pose = q;
			k.pose.visible = m.poseAt(now).visible;
			m.keys.append(k);
			std::sort(m.keys.begin(), m.keys.end(),
				  [](const SpotKey &a, const SpotKey &b) { return a.tMs < b.tMs; });
		});
	};
	for (QDoubleSpinBox *sp : {spotX_, spotY_, spotW_, spotH_, spotRot_, spotRadius_})
		connect(sp, &QDoubleSpinBox::valueChanged, this, [applyPose](double) { applyPose(); });

	into->addWidget(spotBox_);
	syncSpotlightInspector();
}

// An Inverse Selection effect clip carries its own areas. When one is selected
// the panel edits THOSE; otherwise it edits the project-wide spec.
bool VideoEditorWindow::spotTargetIsClip() const
{
	if (!fullEdit() || !timelineView_)
		return false;
	const TlClip *c = timelineView_->selectedClipPtr();
	return c && c->type == TlClip::Type::Effect && c->fx.type == FxType::InverseSelection;
}

// Only ever a clip's own areas now. The empty spec is what the panel reads when
// nothing suitable is selected -- it is hidden in that case, but a reference has
// to point at something.
const SpotlightSpec &VideoEditorWindow::spotTarget() const
{
	static const SpotlightSpec kNone;
	return spotTargetIsClip() ? timelineView_->selectedClipPtr()->fx.spot : kNone;
}

int VideoEditorWindow::selectedMaskRow() const
{
	if (!spotList_ || !timelineView_)
		return -1;
	const int r = spotList_->currentRow();
	return (r >= 0 && r < spotTarget().masks.size()) ? r : -1;
}

// Every spotlight change goes through here, so all of them repaint the preview
// and land as one undo step -- the same contract the clip editors have.
void VideoEditorWindow::editSpotlight(const std::function<void(SpotlightSpec &)> &fn)
{
	editSpotlight(fn, /*commit=*/true);
}

// `commit` false is for the middle of a drag: the model and the preview update
// on every mouse-move, but the undo entry is written once, when the mouse comes
// up. Without that a single gesture would leave a hundred entries behind.
void VideoEditorWindow::editSpotlight(const std::function<void(SpotlightSpec &)> &fn, bool commit)
{
	if (!timelineView_)
		return;
	if (!spotTargetIsClip())
		return; // nothing selected to edit; the panel is hidden anyway
	// editSelectedClip already repaints and snapshots -- and its snapshot is
	// debounced, which is exactly what `commit` is asking for during a drag, so
	// the flag has nothing left to decide now that the clip is the only target.
	Q_UNUSED(commit);
	editSelectedClip([&fn](TlClip &c) { fn(c.fx.spot); });
	syncSpotlightInspector();
}

// A mask dragged in the preview. Where the new pose goes depends on whether the
// mask is animated: an un-keyframed mask just moves, but writing a resting pose
// onto a KEYFRAMED mask would be invisible — the keys would immediately
// override it — so for those the drag lands on a key at the playhead, replacing
// one if it is already there. This is the same rule the clip transform follows,
// which is why dragging a mask and dragging a clip feel the same.
void VideoEditorWindow::onSpotlightPoseDragged(int index, const SpotPose &pose)
{
	const qint64 now = timelinePlayheadMs();
	editSpotlight(
		[&](SpotlightSpec &s) {
			if (index < 0 || index >= s.masks.size())
				return;
			SpotMask &m = s.masks[index];
			if (m.keys.isEmpty()) {
				m.pose = pose;
				return;
			}
			for (SpotKey &k : m.keys)
				if (k.tMs == now) {
					k.pose = pose;
					return;
				}
			SpotKey k;
			k.tMs = now;
			k.pose = pose;
			m.keys.append(k);
			std::sort(m.keys.begin(), m.keys.end(),
				  [](const SpotKey &a, const SpotKey &b) { return a.tMs < b.tMs; });
		},
		/*commit=*/false);
}

// Gather one value across every selected clip: either they all agree, or they
// do not and the Inspector shows an em dash. Templated on the getter so the
// same three lines serve floats, bools and every property type added later —
// which is what makes "any future type multi-edits for free" true rather than
// a promise.
template <typename F>
static ComponentPanel::Mixed gather(int n, F get)
{
	ComponentPanel::Mixed m;
	for (int i = 0; i < n; ++i) {
		const QVariant v = get(i);
		if (i == 0)
			m.value = v;
		else if (v != m.value) {
			m.mixed = true;
			return m;
		}
	}
	return m;
}

// What the Inspector should show for the current selection.
//
// Shared means present on EVERY selected clip, matched by (type, ordinal) so
// the second Blur on one clip lines up with the second Blur on another rather
// than with its first. Anything on only some of them is named in the
// not-shared line instead: there is no coherent value to edit, but leaving it
// out silently would let the list read as the whole truth about the clips.
ComponentPanel::View VideoEditorWindow::buildComponentView() const
{
	ComponentPanel::View v;
	if (!timelineView_ || !fullEdit())
		return v;
	const TimelineModel &m = timelineView_->model();
	QVector<const TlClip *> clips;
	for (const auto &p : timelineView_->selectedPairs())
		if (p.first >= 0 && p.first < m.tracks.size() && p.second >= 0 &&
		    p.second < m.tracks[p.first].clips.size())
			clips.append(&m.tracks[p.first].clips[p.second]);
	v.clipCount = clips.size();
	if (clips.isEmpty())
		return v;

	// The instant being described, not the playhead: while the pointer is over
	// the timeline these rows report the frame under it, which is the frame the
	// preview is showing. Values only -- every EDIT path still asks for the
	// playhead, so what a keyframe button writes never moves with the mouse.
	const qint64 ph = inspectTimeMs();
	const int n = clips.size();
	// Each clip's own time, not the raw playhead: two clips at different
	// positions are at different points in their own animations.
	auto clipTime = [&](int i) { return ph - clips[i]->outStartMs; };

	v.loadErrors = componentErrors_;

	// ---- the essentials every clip has ---------------------------------
	{
		ComponentPanel::PinnedRow pose;
		pose.typeId = QStringLiteral("harpia.transform");
		auto at = [&](int i) { return clips[i]->transformAt(ph); };
		pose.values = {
			{QStringLiteral("posX"), gather(n, [&](int i) { return at(i).posX; })},
			{QStringLiteral("posY"), gather(n, [&](int i) { return at(i).posY; })},
			{QStringLiteral("scale"), gather(n, [&](int i) { return at(i).scale; })},
			{QStringLiteral("rotation"), gather(n, [&](int i) { return at(i).rotation; })},
			{QStringLiteral("opacity"), gather(n, [&](int i) { return at(i).opacity; })}};
		// Only meaningful for one clip: with several selected there is no single
		// script stack to describe.
		if (n == 1)
			pose.driven = scriptDrivenPoseKeys(*clips[0], &pose.drivenTip);
		v.pinned.append(pose);
	}
	{
		bool anyClocked = false;
		for (const TlClip *c : clips)
			if (!c->freeDuration())
				anyClocked = true;
		if (anyClocked) {
			ComponentPanel::PinnedRow sp;
			sp.typeId = QStringLiteral("harpia.speed");
			sp.values = {{QStringLiteral("factor"),
				      gather(n, [&](int i) { return clips[i]->speed; })}};
			v.pinned.append(sp);
		}
	}

	// ---- the components they have in common -----------------------------
	const ComponentRegistry &reg = ComponentRegistry::instance();
	// Ordinal of each (typeId, k) on one clip, so the k-th Blur matches the
	// k-th Blur elsewhere.
	auto nth = [](const TlClip *c, const QString &id, int ordinal) -> const ComponentInstance * {
		int seen = 0;
		for (const ComponentInstance &ci : c->components)
			if (ci.typeId == id && seen++ == ordinal)
				return &ci;
		return nullptr;
	};

	QSet<QString> sharedNames;
	QMap<QString, int> countOnFirst;
	for (const ComponentInstance &ci : clips[0]->components) {
		const int ordinal = countOnFirst[ci.typeId]++;
		bool everywhere = true;
		for (int i = 1; i < n && everywhere; ++i)
			everywhere = nth(clips[i], ci.typeId, ordinal) != nullptr;
		if (!everywhere)
			continue;

		ComponentPanel::SharedComponent sc;
		sc.typeId = ci.typeId;
		sc.ordinal = ordinal;
		sc.enabled = gather(n, [&](int i) {
			const ComponentInstance *x = nth(clips[i], sc.typeId, sc.ordinal);
			return x && x->enabled;
		});
		sc.inMs = gather(n, [&](int i) {
			const ComponentInstance *x = nth(clips[i], sc.typeId, sc.ordinal);
			return x ? QVariant(qlonglong(x->inMs)) : QVariant();
		});
		sc.outMs = gather(n, [&](int i) {
			const ComponentInstance *x = nth(clips[i], sc.typeId, sc.ordinal);
			return x ? QVariant(qlonglong(x->outMs)) : QVariant();
		});
		if (const ComponentType *t = reg.find(ci.typeId)) {
			for (const PropDef &d : t->props)
				sc.values.insert(d.key, gather(n, [&](int i) {
							 const ComponentInstance *x =
								 nth(clips[i], sc.typeId, sc.ordinal);
							 return x ? propAt(*t, *x, d.key, clipTime(i))
								  : QVariant();
						 }));
			// The diamond fills only when EVERY selected clip has a key here;
			// a half-keyed selection is not keyed.
			for (const PropDef &d : t->props) {
				bool allKeyed = true;
				for (int i = 0; i < n && allKeyed; ++i) {
					const ComponentInstance *x = nth(clips[i], sc.typeId, sc.ordinal);
					allKeyed = false;
					if (x) {
						const auto k = x->keys.find(d.key);
						if (k != x->keys.end())
							for (const PropKey &pk : *k)
								if (std::abs(pk.tMs - clipTime(i)) <= 1)
									allKeyed = true;
					}
				}
				if (allKeyed)
					sc.keyedHere.append(d.key);
			}
		}
		// A Sound component's buttons name the sound they hold.
		if (sc.typeId == QLatin1String(kSoundComponentId)) {
			auto nameOf = [&](const char *key) -> QString {
				const ComponentPanel::Mixed mv = sc.values.value(QString::fromLatin1(key));
				if (mv.mixed)
					return QStringLiteral("(different)");
				const int id = mv.value.toInt();
				for (const EditorSource &es : sources_)
					if (es.id == id)
						return es.name;
				return QString();
			};
			auto shortName = [](QString n) {
				return n.size() > 22 ? n.left(20) + QStringLiteral("\u2026") : n;
			};
			const QString in = shortName(nameOf("inSound")), out = shortName(nameOf("outSound"));
			sc.actionLabels.insert(QStringLiteral("pickIn"),
					       in.isEmpty() ? QStringLiteral("Choose In sound\u2026")
							    : QStringLiteral("In: %1").arg(in));
			sc.actionLabels.insert(QStringLiteral("pickOut"),
					       out.isEmpty() ? QStringLiteral("Choose Out sound\u2026")
							     : QStringLiteral("Out: %1").arg(out));
		}
		v.shared.append(sc);
		sharedNames.insert(ci.typeId + QChar('#') + QString::number(ordinal));
	}

	// Anything on some clips but not all.
	if (n > 1) {
		QSet<QString> partial;
		for (const TlClip *c : clips) {
			QMap<QString, int> seen;
			for (const ComponentInstance &ci : c->components) {
				const int ordinal = seen[ci.typeId]++;
				const QString tag = ci.typeId + QChar('#') + QString::number(ordinal);
				if (sharedNames.contains(tag))
					continue;
				const ComponentType *t = reg.find(ci.typeId);
				partial.insert(t ? t->displayName : ci.typeId);
			}
		}
		v.notShared = QStringList(partial.cbegin(), partial.cend());
		v.notShared.sort();
	}

	// Ordering warnings come from the primary clip: with a mixed selection
	// there is no one order to describe, and repeating five of them would be
	// noise rather than help.
	if (n == 1) {
		QStringList w;
		resolveOrder(clips[0]->components, reg, &w);
		v.warnings = w + findConflicts(clips[0]->components, reg);
	}
	return v;
}

void VideoEditorWindow::syncComponentPanel()
{
	if (!componentPanel_ || !timelineView_)
		return;
	// What kinds of clip are selected, so the Add menu can offer only what
	// applies to all of them.
	unsigned kinds = 0;
	const TimelineModel &m = timelineView_->model();
	for (const auto &p : timelineView_->selectedPairs())
		if (p.first >= 0 && p.first < m.tracks.size() && p.second >= 0 &&
		    p.second < m.tracks[p.first].clips.size())
			switch (m.tracks[p.first].clips[p.second].type) {
			case TlClip::Type::Text: kinds |= ClipKindText; break;
			case TlClip::Type::Image: kinds |= ClipKindImage; break;
			case TlClip::Type::Effect: kinds |= ClipKindEffect; break;
			default: kinds |= ClipKindVideo; break;
			}
	componentPanel_->setAllowedKinds(kinds);

	const ComponentPanel::View v = buildComponentView();
	componentPanel_->setVisible(v.clipCount > 0);
	componentPanel_->setView(v);
}

// Every component operation lands here: find the matching component on EACH
// selected clip and apply the change to all of them, in one edit and therefore
// one undo step. `fn` may leave the instance untouched.
void VideoEditorWindow::editSharedComponent(const QString &typeId, int ordinal,
					    const std::function<void(ComponentInstance &)> &fn)
{
	if (!timelineView_)
		return;
	timelineView_->applyToSelection([&](TlClip &c) {
		int seen = 0;
		for (ComponentInstance &ci : c.components)
			if (ci.typeId == typeId && seen++ == ordinal) {
				fn(ci);
				return;
			}
	});
	afterComponentEdit();
}

// Shared tail for every component operation: repaint, refresh the panel, and
// take ONE snapshot however many clips were touched.
// Which instant a keyframe edit lands on. Keys are CLIP-relative, so with two
// clips selected at different positions the same playhead is a different moment
// in each — the offset has to come from the clip the instance is on.
qint64 VideoEditorWindow::componentEditTimeFor(const ComponentInstance &ci) const
{
	if (!timelineView_)
		return 0;
	const TimelineModel &m = timelineView_->model();
	for (const TlTrack &t : m.tracks)
		for (const TlClip &c : t.clips)
			for (const ComponentInstance &x : c.components)
				if (&x == &ci)
					return timelinePlayheadMs() - c.outStartMs;
	return timelinePlayheadMs();
}

void VideoEditorWindow::afterComponentEdit()
{
	syncPreviewTransformTarget();
	showTimelineFrame(timelinePlayheadMs());
	syncComponentPanel();
	scheduleSnapshot();
}

void VideoEditorWindow::syncSpotlightInspector()
{
	if (!spotBox_ || !timelineView_)
		return;
	// Only meaningful in Full editing: the other modes have no composition to
	// dim.
	// A spotlight is a clip, so this panel edits the selected one and shows
	// nothing when there is no such clip -- rather than quietly falling back to a
	// project-wide setting that no longer exists.
	const bool onClip = spotTargetIsClip();
	spotBox_->setVisible(fullEdit() && onClip);
	if (!fullEdit() || !onClip)
		return;
	const SpotlightSpec &s = spotTarget();
	const bool wasSyncing = syncingSpot_;
	syncingSpot_ = true;

	// Say which of the two this panel is pointed at. Without it the controls
	// look identical in both cases and there is no way to tell whether you are
	// about to dim the whole project or just one clip's stretch of it.
	if (spotHdr_)
		spotHdr_->setText(QStringLiteral("Inverse Selection — this clip"));
	if (spotScopeHint_)
		spotScopeHint_->setText(
			QStringLiteral("These areas belong to the selected effect clip: they "
				       "last as long as it does and dim only the tracks below "
				       "it. Keyframes are in clip time, so moving the clip "
				       "takes the animation with it. To dim the whole video, "
				       "drag the clip out to the full length."));

	spotOn_->setChecked(s.enabled);
	spotInvert_->setChecked(s.invert);
	spotDim_->setValue(s.dimOpacity);
	spotBlur_->setValue(s.blur);
	spotColor_->setText(s.dimColor.name(QColor::HexRgb).toUpper());
	{
		// Only when it changed: setStyleSheet re-styles the button even for the
		// same text, and this runs on every selection change.
		const QString css = QStringLiteral("background:%1; color:%2; border:1px solid #444; padding:3px;")
					    .arg(s.dimColor.name(QColor::HexRgb),
						 s.dimColor.lightness() > 140 ? QStringLiteral("#101214")
									      : QStringLiteral("#f0f0f0"));
		if (spotColor_->styleSheet() != css)
			spotColor_->setStyleSheet(css);
	}

	// Rebuild the list only when it no longer matches: rebuilding on every sync
	// would drop the selection mid-edit.
	const int keep = spotList_->currentRow();
	if (spotList_->count() != s.masks.size()) {
		spotList_->clear();
		for (int i = 0; i < s.masks.size(); ++i)
			spotList_->addItem(new QListWidgetItem);
	}
	for (int i = 0; i < s.masks.size(); ++i) {
		QListWidgetItem *it = spotList_->item(i);
		const QString label = QStringLiteral("%1  ·  %2")
					      .arg(s.masks[i].name.isEmpty()
							   ? QStringLiteral("Area %1").arg(i + 1)
							   : s.masks[i].name)
					      .arg(QString::fromLatin1(spotShapeName(s.masks[i].shape)));
		if (it->text() != label)
			it->setText(label);
		it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
		it->setCheckState(s.masks[i].enabled ? Qt::Checked : Qt::Unchecked);
	}
	if (keep >= 0 && keep < s.masks.size())
		spotList_->setCurrentRow(keep);
	else if (!s.masks.isEmpty() && spotList_->currentRow() < 0)
		spotList_->setCurrentRow(0);

	const int r = (spotList_->currentRow() >= 0 && spotList_->currentRow() < s.masks.size())
			      ? spotList_->currentRow()
			      : -1;
	spotMaskBox_->setVisible(r >= 0);
	if (r >= 0) {
		const SpotMask &m = s.masks[r];
		spotShape_->setCurrentIndex(int(m.shape));
		spotX_->setValue(m.pose.cx);
		spotY_->setValue(m.pose.cy);
		spotW_->setValue(m.pose.w);
		spotH_->setValue(m.pose.h);
		spotH_->setEnabled(m.shape != SpotShape::Circle); // a circle is round
		spotRot_->setValue(m.pose.rotation);
		spotRadius_->setValue(m.pose.radius);
		spotRadius_->setEnabled(m.shape == SpotShape::RoundRect);
		// Every pose field is driven by the keys once there are any, so editing
		// the numbers directly would be overwritten on the next frame.
		const bool animated = !m.keys.isEmpty();
		for (QDoubleSpinBox *sb : {spotX_, spotY_, spotW_, spotH_, spotRot_})
			sb->setToolTip(animated
					       ? QStringLiteral("Animated: this edits the key at "
								"the playhead, adding one if there "
								"is none.")
					       : QString());
	}
	if (spotKeys_) {
		QVector<qint64> times;
		if (r >= 0) {
			times.reserve(s.masks[r].keys.size());
			for (const SpotKey &k : s.masks[r].keys)
				times.append(k.tMs);
		}
		spotKeys_->setVisible(r >= 0);
		spotKeys_->setTimes(times, timelinePlayheadMs());
	}

	// Hand the preview what to draw handles for. The poses are resolved AT THE
	// PLAYHEAD, so a keyframed mask's handles sit on the shape you can see
	// rather than on its resting pose.
	if (canvas_) {
		// The instant on screen: the handles have to sit on the shape you can
		// SEE, and while the timeline is hovered that is the hovered frame.
		const qint64 now = inspectTimeMs();
		QVector<PreviewCanvas::SpotDraw> draw;
		draw.reserve(s.masks.size());
		for (const SpotMask &m : s.masks) {
			PreviewCanvas::SpotDraw sd;
			sd.shape = m.shape;
			sd.pose = m.poseAt(now);
			sd.enabled = m.enabled;
			draw.append(sd);
		}
		canvas_->setSpotlightMode(fullEdit() && s.enabled && !s.masks.isEmpty());
		canvas_->setSpotlightMasks(draw, r);
	}
	syncingSpot_ = wasSyncing;
}

void VideoEditorWindow::buildClipInspector(QVBoxLayout *into)
{
	clipBox_ = new QWidget(this);
	clipBox_->setVisible(false);
	auto *clipOuter = new QVBoxLayout(clipBox_);
	clipOuter->setContentsMargins(0, 6, 0, 0);
	clipOuter->setSpacing(5);

	// Below the clip's own content and its components: the TAIL of the tab,
	// the things you set once in a while (animation keys, tags). Shown and
	// hidden with clipBox_, but placed after the component list, which is
	// where a picture's Transform and Speed live.
	clipTailBox_ = new QWidget(this);
	clipTailBox_->setVisible(false);
	auto *tailLayout = new QVBoxLayout(clipTailBox_);
	tailLayout->setContentsMargins(0, 4, 0, 0);
	tailLayout->setSpacing(5);

	// Tags, for any clip -- picture, caption, effect or audio.
	auto *tagsSec = new InspectorSection(QStringLiteral("Tags"), QStringLiteral("clip/tags"), true, clipTailBox_);
	clipTagsBox_ = tagsSec;
	{
		auto *tl = tagsSec->bodyLayout();
		tl->setSpacing(3);
		clipTagChips_ = new QLabel(clipTagsBox_);
		clipTagChips_->setWordWrap(true);
		clipTagChips_->setTextFormat(Qt::RichText);
		clipTagChips_->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
		clipTagChips_->setToolTip(QStringLiteral("Click the \u2715 on a tag to take it off."));
		connect(clipTagChips_, &QLabel::linkActivated, this, [this](const QString &link) {
			if (link.startsWith(QLatin1String("rm:")))
				tagSelection(link.mid(3).toInt(), false);
		});
		tl->addWidget(clipTagChips_);
		clipTagEdit_ = new QLineEdit(clipTagsBox_);
		clipTagEdit_->setPlaceholderText(QStringLiteral("Add a tag: type a name, Enter"));
		clipTagEdit_->setToolTip(QStringLiteral(
			"Tags every selected clip. A new name makes the tag. Sound rules can fire on a tag: "
			"see the Sounds tab or the Tags section of the Project tab."));
		tagNames_ = new QStringListModel(this);
		auto *comp = new QCompleter(tagNames_, clipTagEdit_);
		comp->setCaseSensitivity(Qt::CaseInsensitive);
		comp->setFilterMode(Qt::MatchContains);
		clipTagEdit_->setCompleter(comp);
		connect(clipTagEdit_, &QLineEdit::returnPressed, this, [this]() {
			const QString name = clipTagEdit_->text().trimmed();
			if (name.isEmpty() || !timelineView_)
				return;
			int id = 0;
			if (const TlTag *t = timelineView_->model().tagNamed(name))
				id = t->id;
			if (!id) {
				// Made and applied in one edit, so it is one undo step.
				const auto sel = timelineView_->selectedPairs();
				timelineView_->editModel([&](TimelineModel &m) {
					const int nid = m.ensureTag(name, TimelineView::randomPastel());
					for (const auto &p : sel)
						if (p.first >= 0 && p.first < m.tracks.size() && !m.tracks[p.first].locked &&
						    p.second >= 0 && p.second < m.tracks[p.first].clips.size())
							m.tracks[p.first].clips[p.second].setTag(nid, true);
				});
			} else {
				tagSelection(id, true);
			}
			clipTagEdit_->clear();
		});
		tl->addWidget(clipTagEdit_);
	}

	// Animation applies to a picture: an audio clip has no transform or
	// keyframes, so the section hides and the Audio section takes its place.
	auto *animSec = new InspectorSection(QStringLiteral("Animation"), QStringLiteral("clip/animation"), true,
					     clipTailBox_);
	videoClipBox_ = animSec;
	tailLayout->addWidget(animSec);
	tailLayout->addWidget(tagsSec);
	auto *v = animSec->bodyLayout();
	v->setSpacing(5);

	v->addWidget(infoHint(QStringLiteral("Scroll on the preview to zoom, drag to reposition."), clipBox_), 0,
		     Qt::AlignLeft);

	// Zoom, Position, Rotation, Opacity and Speed all used to live here in a
	// form of their own. They are the pinned Transform and Speed rows of the
	// component list now — one place to look for everything a clip does, rather
	// than some of it here and the rest there. Nothing is left of the form.

	// Reset transform used to be a button here. It is the Transform component's
	// own action now, so it sits inside that component's box with the values it
	// resets rather than floating above them.

	// ---- Keyframes -------------------------------------------------------
	autoKeyChk_ = new QCheckBox(QStringLiteral("Auto-keyframe"), clipBox_);
	autoKeyChk_->setToolTip(QStringLiteral(
		"Record a keyframe at the playhead whenever the zoom/position changes, so the clip "
		"animates between them (e.g. slowly zoom into a menu)."));
	connect(autoKeyChk_, &QCheckBox::toggled, this, [this](bool on) { autoKeyframe_ = on; });
	v->addWidget(autoKeyChk_);

	auto *kfRow = new QHBoxLayout;
	auto *kfPrev = new QPushButton(clipBox_);
	kfPrev->setIcon(uiIcon(Glyph::StepBack, 13));
	auto *kfAdd = new QPushButton(QStringLiteral("Key"), clipBox_);
	kfAdd->setIcon(uiIcon(Glyph::Diamond, 13));
	auto *kfDel = new QPushButton(clipBox_);
	kfDel->setIcon(uiIcon(Glyph::Cross, 13));
	auto *kfNext = new QPushButton(clipBox_);
	kfNext->setIcon(uiIcon(Glyph::StepForward, 13));
	kfPrev->setToolTip(QStringLiteral("Jump to the previous keyframe"));
	kfAdd->setToolTip(QStringLiteral("Add/update a keyframe at the playhead"));
	kfDel->setToolTip(QStringLiteral("Delete the keyframe at the playhead"));
	kfNext->setToolTip(QStringLiteral("Jump to the next keyframe"));
	kfPrev->setFixedWidth(30);
	kfDel->setFixedWidth(30);
	kfNext->setFixedWidth(30);
	// "◆ Key" needs about 50 px; Fusion holds every text button at 80, which
	// made this row the widest thing in the Clip tab once the section became
	// an inset card. It still stretches to fill the row.
	kfAdd->setMinimumWidth(56);
	kfRow->addWidget(kfPrev);
	kfRow->addWidget(kfAdd, 1);
	kfRow->addWidget(kfDel);
	kfRow->addWidget(kfNext);
	v->addLayout(kfRow);
	// The motion path's shape for the whole clip. Per key: right-click a key
	// on the preview's path.
	{
		auto *pathRow = new QHBoxLayout;
		auto *curveBtn = new QPushButton(QStringLiteral("Curve path"), clipBox_);
		curveBtn->setToolTip(QStringLiteral(
			"Move between position keys along smooth curves instead of straight lines. Drag the "
			"round handles on the preview to bend them; right-click a key to change just that "
			"segment."));
		auto *straightBtn = new QPushButton(QStringLiteral("Straight path"), clipBox_);
		straightBtn->setToolTip(QStringLiteral("Straight lines between the position keys again"));
		pathRow->addWidget(curveBtn, 1);
		pathRow->addWidget(straightBtn, 1);
		v->addLayout(pathRow);
		connect(curveBtn, &QPushButton::clicked, this, [this]() { setSelectedClipPathCurved(true); });
		connect(straightBtn, &QPushButton::clicked, this, [this]() { setSelectedClipPathCurved(false); });
	}
	connect(kfAdd, &QPushButton::clicked, this, &VideoEditorWindow::addKeyframeAtPlayhead);
	connect(kfDel, &QPushButton::clicked, this, &VideoEditorWindow::removeKeyframeAtPlayhead);
	connect(kfPrev, &QPushButton::clicked, this, [this]() { stepKeyframe(-1); });
	connect(kfNext, &QPushButton::clicked, this, [this]() { stepKeyframe(1); });
	// Every keyframe, listed. "3 keyframes" tells you how many there are and
	// nothing about WHERE they are, so finding the one you want meant stepping
	// through them with the arrows and watching the preview. Each row is its
	// time and the channels it pins; clicking one jumps the playhead to it.
	keyList_ = new QListWidget(clipBox_);
	keyList_->setObjectName(QStringLiteral("keyList"));
	keyList_->setToolTip(QStringLiteral(
		"Every keyframe on this clip. Click one to jump to it. Drag a row to change the order "
		"the framings play in (the times stay put); Delete removes the highlighted key; "
		"right-click for more."));
	keyList_->setAlternatingRowColors(true);
	keyList_->setUniformItemSizes(true);
	keyList_->setMaximumHeight(120);
	// Drag to reorder. Qt moves the row; the model follows once the drop is
	// over (queued: the list is rebuilt from the clip, and doing that inside
	// the drop would pull the rows out from under the drag still finishing).
	keyList_->setSelectionMode(QAbstractItemView::SingleSelection);
	keyList_->setDragDropMode(QAbstractItemView::InternalMove);
	keyList_->setDefaultDropAction(Qt::MoveAction);
	connect(keyList_->model(), &QAbstractItemModel::rowsMoved, this,
		[this](const QModelIndex &, int start, int, const QModelIndex &, int row) {
			if (syncingClip_ || !keyList_)
				return;
			QVector<int> order;
			order.reserve(keyList_->count());
			for (int i = 0; i < keyList_->count(); ++i)
				order.append(keyList_->item(i)->data(Qt::UserRole + 1).toInt());
			const int landed = row > start ? row - 1 : row;
			QTimer::singleShot(0, this, [this, order, landed]() { applyKeyRowOrder(order, landed); });
		});
	keyList_->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(keyList_, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
		QListWidgetItem *it = keyList_->itemAt(pos);
		if (!it)
			return;
		keyList_->setCurrentItem(it);
		keyRowMenu(keyList_->row(it), keyList_->viewport()->mapToGlobal(pos));
	});
	connect(keyList_, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
		if (syncingClip_ || !it)
			return;
		const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
		if (!sel)
			return;
		bool ok = false;
		const qint64 rel = it->data(Qt::UserRole).toLongLong(&ok);
		if (!ok)
			return;
		onTimelineScrub(sel->outStartMs + rel);
		syncClipInspector();
	});
	v->addWidget(keyList_);

	keyInfo_ = new QLabel(QString(), clipBox_);
	keyInfo_->setWordWrap(true); // "No animation -- the clip holds one fixed framing." is wider than the panel
	keyInfo_->setStyleSheet(QStringLiteral("color:#7f858e;"));
	v->addWidget(keyInfo_);

	// The transform-script panel used to live here. Scripts are components now
	// ("Add Component -> Script"), so the stack, its drag-to-reorder and its
	// parameter controls are the ones every component gets rather than a second
	// set built only for scripts.

	// ---- Text style (text clips only) ------------------------------------
	textBox_ = new QWidget(clipBox_);
	textBox_->setVisible(false);
	auto *textOuter = new QVBoxLayout(textBox_);
	textOuter->setContentsMargins(0, 0, 0, 0);
	textSection_ = new InspectorSection(QStringLiteral("Text"), QStringLiteral("clip/text"), true, textBox_);
	textOuter->addWidget(textSection_);
	auto *tv = textSection_->bodyLayout();
	tv->setSpacing(4);
	textBatchNote_ = new QLabel(textBox_);
	textBatchNote_->setObjectName(QStringLiteral("textBatchNote"));
	textBatchNote_->setWordWrap(true);
	textBatchNote_->setStyleSheet(QStringLiteral("color:#7dd3fc;"));
	textBatchNote_->setVisible(false);
	tv->addWidget(textBatchNote_);

	// Built-in social styles and the user's own saved looks share one picker.
	auto *presetRow = new QHBoxLayout;
	textPresetCombo_ = new QComboBox(textBox_);
	textPresetCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	textPresetCombo_->setMinimumContentsLength(8);
	textPresetCombo_->setObjectName(QStringLiteral("textStylePreset"));
	textPresetCombo_->setToolTip(
		QStringLiteral("Choose a social media style or one of your saved styles. Applies to selected captions and keeps their words."));
	auto *presetSave = new QPushButton(QStringLiteral("Save…"), textBox_);
	presetSave->setObjectName(QStringLiteral("saveTextStylePreset"));
	presetSave->setToolTip(QStringLiteral("Save this caption's style under a name"));
	// Fusion's 80 px floor for a text button, not the label, set this row's
	// width; it may give way when the panel is narrow (see the Key button).
	presetSave->setMinimumWidth(56);
	auto *presetDel = new QPushButton(textBox_);
	textPresetDelete_ = presetDel;
	presetDel->setObjectName(QStringLiteral("deleteTextStylePreset"));
	presetDel->setIcon(uiIcon(Glyph::Cross, 13));
	presetDel->setFixedWidth(28);
	presetDel->setToolTip(QStringLiteral("Delete the selected saved style. Built-in styles are always available."));
	presetRow->addWidget(textPresetCombo_, 1);
	presetRow->addWidget(presetSave);
	presetRow->addWidget(presetDel);
	tv->addLayout(presetRow);
	connect(textPresetCombo_, &QComboBox::activated, this, [this](int i) {
		if (i > 0 && !syncingClip_)
			applyTextPreset(textPresetCombo_->itemData(i).toString());
	});
	connect(textPresetCombo_, &QComboBox::currentIndexChanged, this, [this](int i) {
		textPresetDelete_->setEnabled(textPresetCombo_->itemData(i).toString().startsWith(QStringLiteral("saved/")));
	});
	connect(presetSave, &QPushButton::clicked, this,
		&VideoEditorWindow::saveTextPresetFromSelection);
	connect(presetDel, &QPushButton::clicked, this, &VideoEditorWindow::deleteSelectedTextPreset);
	refreshTextPresets();

	// Quick tags, above every text field of the clip: wrap the selected words
	// in the field being typed in (or put an empty pair at its caret). The
	// buttons never take the focus, so the selection stays where it was.
	{
		// Wraps onto a second line in a narrow Inspector rather than pushing
		// the panel into a horizontal scroll.
		auto *rowHost = new QWidget(textBox_);
		quickTagsRow_ = rowHost;
		auto *row = new FlowLayout(rowHost, 5, 5);
		const auto wrapWith = [this](const QString &open, const QString &close) {
			QPlainTextEdit *ed = tagTarget_ ? tagTarget_.data() : textEdit_;
			if (!ed)
				return;
			QTextCursor cur = ed->textCursor();
			const QString sel = cur.selectedText().replace(QChar(0x2029), QLatin1Char('\n'));
			cur.beginEditBlock();
			cur.insertText(open + sel + close);
			cur.endEditBlock();
			if (sel.isEmpty()) // the caret between the pair, ready to type
				cur.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, int(close.size()));
			ed->setTextCursor(cur);
			ed->setFocus();
		};
		const auto tagButton = [&](const QString &label, const QString &tip, const QString &style,
					    std::function<void()> act) {
			auto *b = new QPushButton(label, textBox_);
			// Big enough to hit and to read: seven of them still fit across
			// the Inspector.
			b->setFixedSize(42, 36);
			b->setFocusPolicy(Qt::NoFocus);
			b->setToolTip(tip);
			// The size in the style sheet too: a style sheet's own min-width
			// is applied after setFixedSize and wins, which is how these
			// ended up squeezed to a sliver.
			b->setStyleSheet(QStringLiteral("padding:0px; min-width:42px; max-width:42px; min-height:36px; "
							"max-height:36px; font-size:16px;") +
					 style);
			connect(b, &QPushButton::clicked, this, std::move(act));
			row->addWidget(b);
		};
		tagButton(QStringLiteral("B"), QStringLiteral("Bold the selection: <b>…</b>"),
			  QStringLiteral("font-weight:bold;"), [wrapWith]() { wrapWith(QStringLiteral("<b>"), QStringLiteral("</b>")); });
		tagButton(QStringLiteral("I"), QStringLiteral("Italic: <i>…</i>"), QStringLiteral("font-style:italic;"),
			  [wrapWith]() { wrapWith(QStringLiteral("<i>"), QStringLiteral("</i>")); });
		tagButton(QStringLiteral("U"), QStringLiteral("Underline: <u>…</u>"), QStringLiteral("text-decoration:underline;"),
			  [wrapWith]() { wrapWith(QStringLiteral("<u>"), QStringLiteral("</u>")); });
		tagButton(QStringLiteral("S"), QStringLiteral("Strike through: <s>…</s>"),
			  QStringLiteral("text-decoration:line-through;"),
			  [wrapWith]() { wrapWith(QStringLiteral("<s>"), QStringLiteral("</s>")); });
		tagButton(QStringLiteral("A"), QStringLiteral("Colour the selection: <color=#rrggbb>…</color>"),
			  QStringLiteral("color:#ff8a3d;font-weight:bold;"), [this, wrapWith]() {
				  const QColor c = QColorDialog::getColor(QColor(0xff, 0xd2, 0x3f), this,
									  QStringLiteral("Text colour"),
									  QColorDialog::ShowAlphaChannel);
				  if (!c.isValid())
					  return;
				  QString hex = QStringLiteral("#%1%2%3")
							.arg(c.red(), 2, 16, QLatin1Char('0'))
							.arg(c.green(), 2, 16, QLatin1Char('0'))
							.arg(c.blue(), 2, 16, QLatin1Char('0'));
				  if (c.alpha() != 255)
					  hex += QStringLiteral("%1").arg(c.alpha(), 2, 16, QLatin1Char('0'));
				  wrapWith(QStringLiteral("<color=%1>").arg(hex), QStringLiteral("</color>"));
			  });
		tagButton(QStringLiteral("A+"), QStringLiteral("Bigger: <size=150%>…</size>"), QString(),
			  [wrapWith]() { wrapWith(QStringLiteral("<size=150%>"), QStringLiteral("</size>")); });
		tagButton(QStringLiteral("H"), QStringLiteral("Highlight: <mark=#ffff0080>…</mark>"),
			  QStringLiteral("color:#1b1b1b;background:#e8d44d;font-weight:bold;"),
			  [wrapWith]() { wrapWith(QStringLiteral("<mark=#ffff0080>"), QStringLiteral("</mark>")); });
		tv->addWidget(rowHost);
		// The field the tags go into: whichever of this clip's text fields
		// was typed in last (the main text until another is clicked).
		connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
			auto *ed = qobject_cast<QPlainTextEdit *>(now);
			if (ed && (ed == textEdit_ || variantEdits_.contains(ed)))
				tagTarget_ = ed;
		});
	}

	// "Version 1" over the main text, for a Random text only: it is the first
	// of the texts, not something apart from them.
	textMainCap_ = new QLabel(QStringLiteral("Text 1"), textBox_);
	textMainCap_->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	tv->addWidget(textMainCap_);
	textEdit_ = new QPlainTextEdit(textBox_);
	textEdit_->setPlaceholderText(QStringLiteral("Type your caption… (Enter for a new line)"));
	textEdit_->setFixedHeight(56);
	textEdit_->setToolTip(kRichTextTip);
	tv->addWidget(textEdit_);
	connect(textEdit_, &QPlainTextEdit::textChanged, this, [this]() {
		// A whole caption lane is being styled: the box is hidden then, and
		// what it holds is one caption's words, not the lane's.
		if (syncingClip_ || captionLaneTrack_ >= 0)
			return;
		const QString t = textEdit_->toPlainText();
		const QSize canvas = timelineCanvasSize();
		editSelectedClip([&t, canvas](TlClip &c) {
			// A caption grows DOWNWARD as you type: when a line wraps (or
			// Enter adds one) the top edge stays where it was and the new
			// line appears below, instead of the whole block creeping up
			// to stay centred. Only an un-keyed caption is moved -- a keyed
			// one has its position on the keys, which are not this edit's.
			const int oldH = TimelineCompositor::textNaturalSize(c.text, canvas).height();
			c.text.text = t;
			const int newH = TimelineCompositor::textNaturalSize(c.text, canvas).height();
			if (newH != oldH && c.keys.isEmpty() && canvas.height() > 0 &&
			    std::abs(c.rotation) < 0.001)
				c.posY += double(newH - oldH) * std::max(0.001, c.scale) / 2.0 / canvas.height();
		});
	});
	// Clicking into a Random text's first text previews it.
	connect(textEdit_, &QPlainTextEdit::cursorPositionChanged, this, [this]() {
		if (!syncingClip_ && textEdit_->hasFocus())
			showPreviewVariant(0);
	});

	// Random text: the other texts, each in its own box (a text can run over
	// several lines), + to add one, × to remove one. Every one is its own
	// version of the video on export (Export → Variations), styled like the
	// first and taking the same tags.
	variationsBox_ = new QWidget(textBox_);
	{
		auto *vl = new QVBoxLayout(variationsBox_);
		vl->setContentsMargins(0, 0, 0, 0);
		vl->setSpacing(4);
		auto *list = new QWidget(variationsBox_);
		variantListLayout_ = new QVBoxLayout(list);
		variantListLayout_->setContentsMargins(0, 0, 0, 0);
		variantListLayout_->setSpacing(4);
		vl->addWidget(list);
		auto *add = new QPushButton(QStringLiteral("+  Add text"), variationsBox_);
		add->setObjectName(QStringLiteral("addVariantButton"));
		add->setToolTip(QStringLiteral("Add another text: one more version of the video"));
		vl->addWidget(add);
		connect(add, &QPushButton::clicked, this, &VideoEditorWindow::addVariantText);

		auto *row = new QHBoxLayout;
		variantPrev_ = new QPushButton(variationsBox_);
		variantNext_ = new QPushButton(variationsBox_);
		variantPrev_->setIcon(uiIcon(Glyph::StepBack, 14));
		variantNext_->setIcon(uiIcon(Glyph::StepForward, 14));
		for (QPushButton *b : {variantPrev_, variantNext_}) {
			b->setFixedSize(42, 32);
			b->setStyleSheet(QStringLiteral(
				"padding:0px; min-width:42px; max-width:42px; min-height:32px; max-height:32px;"));
		}
		variantPrev_->setToolTip(QStringLiteral("Preview the previous text"));
		variantNext_->setToolTip(QStringLiteral("Preview the next text"));
		variantLabel_ = new QLabel(variationsBox_);
		row->addWidget(variantPrev_);
		row->addWidget(variantLabel_, 1, Qt::AlignCenter);
		row->addWidget(variantNext_);
		vl->addLayout(row);
		connect(variantPrev_, &QPushButton::clicked, this, [this]() { stepPreviewVariant(-1); });
		connect(variantNext_, &QPushButton::clicked, this, [this]() { stepPreviewVariant(+1); });

		// Saved lists: type the texts once, put them on any Random text again.
		auto *presetRow = new QHBoxLayout;
		variationPresetCombo_ = new QComboBox(variationsBox_);
		variationPresetCombo_->setToolTip(
			QStringLiteral("Load a saved list of texts (Text 2 onwards). Text 1 stays as it is."));
		auto *vSave = new QPushButton(QStringLiteral("Save…"), variationsBox_);
		vSave->setToolTip(QStringLiteral("Save Text 2 onwards under a name"));
		vSave->setMinimumWidth(56); // as the style row's Save… above
		auto *vDel = new QPushButton(variationsBox_);
		vDel->setIcon(uiIcon(Glyph::Cross, 13));
		vDel->setFixedWidth(28);
		vDel->setToolTip(QStringLiteral("Delete the selected saved list"));
		presetRow->addWidget(variationPresetCombo_, 1);
		presetRow->addWidget(vSave);
		presetRow->addWidget(vDel);
		vl->addLayout(presetRow);
		connect(variationPresetCombo_, &QComboBox::activated, this, [this](int i) {
			if (i > 0 && !syncingClip_)
				applyVariationPreset(variationPresetCombo_->currentText());
		});
		connect(vSave, &QPushButton::clicked, this, &VideoEditorWindow::saveVariationPresetFromSelection);
		connect(vDel, &QPushButton::clicked, this, &VideoEditorWindow::deleteSelectedVariationPreset);
		refreshVariationPresets();
	}
	tv->addWidget(variationsBox_);

	auto *animationForm = new QFormLayout;
	animationForm->setContentsMargins(0, 0, 0, 0);
	animationForm->setHorizontalSpacing(8);
	animationForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
	captionAnimationCombo_ = new QComboBox(textBox_);
	captionAnimationCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	captionAnimationCombo_->setMinimumContentsLength(8);
	captionAnimationCombo_->setObjectName(QStringLiteral("captionAnimation"));
	captionAnimationCombo_->addItems({QStringLiteral("Whole phrase"), QStringLiteral("One word at a time"),
		QStringLiteral("Build up word by word"), QStringLiteral("Highlight current word"),
		QStringLiteral("Highlight spoken words")});
	captionAnimationCombo_->setToolTip(QStringLiteral(
		"Animate the selected captions using spoken word timings. Typed captions use an even pace. "
		"Use the text colour button to give individual keywords their own colour."));
	animationForm->addRow(QStringLiteral("Animation"), captionAnimationCombo_);
	connect(captionAnimationCombo_, &QComboBox::currentIndexChanged, this, [this](int mode) {
		if (!syncingClip_ && mode >= 0)
			setCaptionAnimationMode(mode);
	});
	captionHighlightColor_ = new QPushButton(textBox_);
	captionHighlightColor_->setObjectName(QStringLiteral("captionHighlightColor"));
	captionHighlightColor_->setToolTip(QStringLiteral("Colour of the highlighted spoken word or words"));
	animationForm->addRow(QStringLiteral("Highlight colour"), captionHighlightColor_);
	connect(captionHighlightColor_, &QPushButton::clicked, this, &VideoEditorWindow::pickCaptionHighlightColor);
	captionPopCheck_ = new QCheckBox(QStringLiteral("Subtle pop on each word"), textBox_);
	captionPopCheck_->setObjectName(QStringLiteral("captionWordPop"));
	captionPopCheck_->setToolTip(QStringLiteral("Give each spoken word a short, gentle zoom, then return to the caption's normal size."));
	animationForm->addRow(captionPopCheck_);
	connect(captionPopCheck_, &QCheckBox::toggled, this, [this](bool enabled) {
		if (!syncingClip_)
			setCaptionPopEnabled(enabled);
	});
	tv->addLayout(animationForm);

	fontCombo_ = new QFontComboBox(textBox_);
	tv->addWidget(fontCombo_);
	connect(fontCombo_, &QFontComboBox::currentFontChanged, this, [this](const QFont &f) {
		if (syncingClip_)
			return;
		editTextStyle([&f](TlClip &c) { c.text.fontFamily = f.family(); });
	});

	auto *tForm = new QFormLayout;
	tForm->setContentsMargins(0, 0, 0, 0);
	tForm->setHorizontalSpacing(8);
	tForm->setVerticalSpacing(4);
	textForm_ = tForm; // the box rows are hidden through it, see syncTextBoxRows

	fontSizeSpin_ = new QSpinBox(textBox_);
	fontSizeSpin_->setRange(6, 400);
	fontSizeSpin_->setKeyboardTracking(false);
	tForm->addRow(QStringLiteral("Size"), fontSizeSpin_);
	connect(fontSizeSpin_, &QSpinBox::valueChanged, this, [this](int v) {
		if (syncingClip_)
			return;
		editTextStyle([v](TlClip &c) { c.text.fontPx = v; });
	});

	auto *styleRow = new FlowLayout(nullptr, 10, 2); // wraps in a narrow panel
	boldChk_ = new QCheckBox(QStringLiteral("Bold"), textBox_);
	italicChk_ = new QCheckBox(QStringLiteral("Italic"), textBox_);
	titleCaseChk_ = new QCheckBox(QStringLiteral("Title Case"), textBox_);
	titleCaseChk_->setToolTip(QStringLiteral(
		"Draw Every Word With A Capital. A style, not a rewrite: your typed wording is kept."));
	styleRow->addWidget(boldChk_);
	styleRow->addWidget(italicChk_);
	styleRow->addWidget(titleCaseChk_);
	tForm->addRow(QStringLiteral("Style"), [&] {
		auto *w = new QWidget(textBox_);
		w->setLayout(styleRow);
		return w;
	}());
	connect(boldChk_, &QCheckBox::toggled, this, [this](bool on) {
		if (syncingClip_)
			return;
		editTextStyle([on](TlClip &c) { c.text.bold = on; });
	});
	connect(italicChk_, &QCheckBox::toggled, this, [this](bool on) {
		if (syncingClip_)
			return;
		editTextStyle([on](TlClip &c) { c.text.italic = on; });
	});
	connect(titleCaseChk_, &QCheckBox::toggled, this, [this](bool on) {
		if (syncingClip_)
			return;
		editTextStyle([on](TlClip &c) { c.text.textCase = on ? TlCaseTitle : TlCaseAsTyped; });
	});

	alignCombo_ = new QComboBox(textBox_);
	alignCombo_->addItems({QStringLiteral("Left"), QStringLiteral("Centre"), QStringLiteral("Right")});
	tForm->addRow(QStringLiteral("Align"), alignCombo_);
	connect(alignCombo_, &QComboBox::currentIndexChanged, this, [this](int i) {
		if (syncingClip_)
			return;
		editTextStyle([i](TlClip &c) { c.text.align = i; });
	});

	textColorBtn_ = new QPushButton(textBox_);
	tForm->addRow(QStringLiteral("Colour"), textColorBtn_);
	connect(textColorBtn_, &QPushButton::clicked, this,
		[this]() { pickTextColor(QStringLiteral("Text colour"), &TlText::color); });

	outlineWSpin_ = new QDoubleSpinBox(textBox_);
	outlineWSpin_->setRange(0.0, 30.0);
	outlineWSpin_->setSingleStep(0.5);
	outlineWSpin_->setDecimals(1);
	outlineWSpin_->setKeyboardTracking(false);
	tForm->addRow(QStringLiteral("Outline"), outlineWSpin_);
	connect(outlineWSpin_, &QDoubleSpinBox::valueChanged, this, [this](double w) {
		if (syncingClip_)
			return;
		editTextStyle([w](TlClip &c) { c.text.outlineWidth = w; });
	});

	outlineColorBtn_ = new QPushButton(textBox_);
	tForm->addRow(QStringLiteral("Outline colour"), outlineColorBtn_);
	connect(outlineColorBtn_, &QPushButton::clicked, this, [this]() {
		pickTextColor(QStringLiteral("Outline colour"), &TlText::outlineColor);
	});

	boxChk_ = new QCheckBox(QStringLiteral("Background"), textBox_);
	boxChk_->setObjectName(QStringLiteral("textBackgroundEnabled"));
	tForm->addRow(QString(), boxChk_);
	connect(boxChk_, &QCheckBox::toggled, this, [this](bool on) {
		// Before the syncingClip_ guard: the rows follow the CHECKBOX, whether
		// it was clicked or set from the clip being selected.
		syncTextBoxRows();
		if (syncingClip_)
			return;
		editTextStyle([on](TlClip &c) { c.text.boxEnabled = on; });
	});

	boxShapeCombo_ = new QComboBox(textBox_);
	boxShapeCombo_->setObjectName(QStringLiteral("textBackgroundShape"));
	boxShapeCombo_->addItem(QStringLiteral("Rectangle"), TlTextBoxRectangle);
	boxShapeCombo_->addItem(QStringLiteral("Fit text"), TlTextBoxFitText);
	boxShapeCombo_->setToolTip(QStringLiteral(
		"Rectangle covers the whole text block. Fit text follows each line's width "
		"with a joined background, like social video captions."));
	tForm->addRow(QStringLiteral("Shape"), boxShapeCombo_);
	connect(boxShapeCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (syncingClip_ || index < 0)
			return;
		const int shape = boxShapeCombo_->itemData(index).toInt();
		editTextStyle([shape](TlClip &c) { c.text.boxShape = shape; });
	});

	boxColorBtn_ = new QPushButton(textBox_);
	tForm->addRow(QStringLiteral("Box colour"), boxColorBtn_);
	// No alpha channel in the picker: the transparency is its own control below,
	// so two places can't disagree about how see-through the box is.
	connect(boxColorBtn_, &QPushButton::clicked, this,
		[this]() { pickTextColor(QStringLiteral("Box colour"), &TlText::boxColor); });

	boxOpacityRow_ = new QWidget(textBox_);
	auto *boxOpacityLayout = new QHBoxLayout(boxOpacityRow_);
	boxOpacityLayout->setContentsMargins(0, 0, 0, 0);
	boxOpacityLayout->setSpacing(6);
	boxOpacitySlider_ = new QSlider(Qt::Horizontal, boxOpacityRow_);
	boxOpacitySlider_->setObjectName(QStringLiteral("textBoxOpacity"));
	boxOpacitySlider_->setAccessibleName(QStringLiteral("Box opacity"));
	boxOpacitySlider_->setRange(0, 100);
	boxOpacitySlider_->setPageStep(5);
	boxOpacitySlider_->setTracking(true);
	boxOpacitySlider_->setMinimumWidth(40);
	boxOpacitySlider_->setToolTip(QStringLiteral("Background box opacity: 0% is transparent, 100% is solid."));
	boxOpacityValue_ = new QLabel(QStringLiteral("0%"), boxOpacityRow_);
	boxOpacityValue_->setObjectName(QStringLiteral("textBoxOpacityValue"));
	boxOpacityValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	boxOpacityValue_->setMinimumWidth(boxOpacityValue_->fontMetrics().horizontalAdvance(QStringLiteral("100%")));
	boxOpacityLayout->addWidget(boxOpacitySlider_, 1);
	boxOpacityLayout->addWidget(boxOpacityValue_);
	tForm->addRow(QStringLiteral("Box opacity"), boxOpacityRow_);
	connect(boxOpacitySlider_, &QSlider::valueChanged, this, [this](int value) {
		boxOpacityValue_->setText(QStringLiteral("%1%").arg(value));
		if (syncingClip_)
			return;
		editTextStyle([value](TlClip &c) { c.text.boxOpacity = value / 100.0; });
	});

	// Horizontal and vertical padding separately: a caption usually wants more
	// breathing room at the sides than above and below.
	boxPadXSpin_ = new QSpinBox(textBox_);
	boxPadXSpin_->setRange(0, 200);
	boxPadXSpin_->setKeyboardTracking(false);
	boxPadXSpin_->setToolTip(QStringLiteral(
		"In the same units as the font size, so the box keeps its proportions "
		"whatever the output resolution."));
	tForm->addRow(QStringLiteral("Box padding X"), boxPadXSpin_);
	connect(boxPadXSpin_, &QSpinBox::valueChanged, this, [this](int v) {
		if (syncingClip_)
			return;
		editTextStyle([v](TlClip &c) { c.text.boxPadX = v; });
	});
	boxPadYSpin_ = new QSpinBox(textBox_);
	boxPadYSpin_->setRange(0, 200);
	boxPadYSpin_->setKeyboardTracking(false);
	tForm->addRow(QStringLiteral("Box padding Y"), boxPadYSpin_);
	connect(boxPadYSpin_, &QSpinBox::valueChanged, this, [this](int v) {
		if (syncingClip_)
			return;
		editTextStyle([v](TlClip &c) { c.text.boxPadY = v; });
	});

	boxRadiusSpin_ = new QSpinBox(textBox_);
	boxRadiusSpin_->setRange(0, 200);
	boxRadiusSpin_->setKeyboardTracking(false);
	boxRadiusSpin_->setToolTip(QStringLiteral(
		"Corner radius. Clamped to half the shorter side, so turning it up gives "
		"a clean pill rather than an artefact."));
	tForm->addRow(QStringLiteral("Corner radius"), boxRadiusSpin_);
	connect(boxRadiusSpin_, &QSpinBox::valueChanged, this, [this](int v) {
		if (syncingClip_)
			return;
		editTextStyle([v](TlClip &c) { c.text.boxRadius = v; });
	});

	tv->addLayout(tForm);

	// ---- Every caption on a lane (batch style) ----------------------------
	// Shown instead of a clip's panel when a caption lane's header is
	// selected: a heading that says what is being edited, where the captions
	// sit, and then the same Text controls as one caption has, each applied to
	// every caption on the lane.
	captionLaneBox_ = new QWidget(clipBox_);
	captionLaneBox_->setVisible(false);
	{
		auto *lv = new QVBoxLayout(captionLaneBox_);
		lv->setContentsMargins(0, 0, 0, 0);
		lv->setSpacing(4);
		captionLaneTitle_ = new QLabel(captionLaneBox_);
		captionLaneTitle_->setWordWrap(true);
		captionLaneTitle_->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed;"));
		lv->addWidget(captionLaneTitle_);
		captionLaneNote_ = new QLabel(QStringLiteral(
			"Every change below applies to all the captions on this lane at once; their words "
			"stay their own. Click one caption to edit just that one."), captionLaneBox_);
		captionLaneNote_->setWordWrap(true);
		captionLaneNote_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
		lv->addWidget(captionLaneNote_);
		auto *pf = new QFormLayout;
		pf->setContentsMargins(0, 4, 0, 0);
		pf->setHorizontalSpacing(8);
		pf->setVerticalSpacing(4);
		captionLanePos_ = new QComboBox(captionLaneBox_);
		captionLanePos_->addItems({QStringLiteral("As they are"), QStringLiteral("Bottom"),
					   QStringLiteral("Middle"), QStringLiteral("Top")});
		captionLanePos_->setToolTip(QStringLiteral(
			"Moves every caption to the same height. Fine-tune with Horizontal and Vertical."));
		pf->addRow(QStringLiteral("Position"), captionLanePos_);
		captionLaneX_ = new QDoubleSpinBox(captionLaneBox_);
		captionLaneY_ = new QDoubleSpinBox(captionLaneBox_);
		for (QDoubleSpinBox *sb : {captionLaneX_, captionLaneY_}) {
			sb->setRange(0.0, 100.0);
			sb->setDecimals(1);
			sb->setSingleStep(1.0);
			sb->setSuffix(QStringLiteral(" %"));
			sb->setKeyboardTracking(false);
		}
		captionLaneX_->setToolTip(QStringLiteral("Centre of the captions across the picture: 0 % left edge, 100 % right."));
		captionLaneY_->setToolTip(QStringLiteral("Centre of the captions down the picture: 0 % top, 100 % bottom."));
		pf->addRow(QStringLiteral("Horizontal"), captionLaneX_);
		pf->addRow(QStringLiteral("Vertical"), captionLaneY_);
		lv->addLayout(pf);
		// Position lives on the clip (and on its keys, if a caption was
		// animated): all of them move together so an animated caption does not
		// snap back to where it was on its next key.
		const auto setPos = [this](bool horizontal, double frac) {
			editTextStyle([horizontal, frac](TlClip &c) {
				(horizontal ? c.posX : c.posY) = frac;
				for (TlKeyframe &k : c.keys)
					(horizontal ? k.tf.posX : k.tf.posY) = frac;
			});
		};
		connect(captionLaneX_, &QDoubleSpinBox::valueChanged, this, [this, setPos](double v) {
			if (!syncingClip_)
				setPos(true, v / 100.0);
		});
		connect(captionLaneY_, &QDoubleSpinBox::valueChanged, this, [this, setPos](double v) {
			if (!syncingClip_)
				setPos(false, v / 100.0);
		});
		connect(captionLanePos_, &QComboBox::activated, this, [this, setPos](int i) {
			if (syncingClip_ || i <= 0)
				return;
			// The same heights the Subtitles window uses (12 % in from the edge).
			const double y = i == 1 ? 0.88 : (i == 2 ? 0.5 : 0.12);
			setPos(false, y);
		});
	}
	// The clip's own content first: a caption's words, a lane's captions.
	clipOuter->insertWidget(0, captionLaneBox_);
	clipOuter->insertWidget(1, textBox_);
	buildRandomClipSection(clipOuter); // at 2: a Random clip's choices, before its sound

	// ---- Audio clip: level and fades -------------------------------------
	// These already existed in the model, were saved with the project and were
	// applied at export — there was simply no way to reach them.
	auto *audioSec = new InspectorSection(QStringLiteral("Audio"), QStringLiteral("clip/audio"), true, clipBox_);
	audioClipBox_ = audioSec;
	audioClipBox_->setVisible(false);
	clipOuter->addWidget(audioClipBox_);
	auto *av = audioSec->bodyLayout();
	av->setSpacing(5);

	auto *aForm = new QFormLayout;
	aForm->setContentsMargins(0, 2, 0, 0);
	aForm->setHorizontalSpacing(10);
	aForm->setVerticalSpacing(4);

	// A slider, like every other bounded value in this Inspector. A level is
	// something you sweep while listening, not a number you guess and re-guess
	// -- and ParamSlider keeps the spin box beside it, so "exactly 1.00" is
	// still one keystroke away.
	clipVolSlider_ = new ParamSlider(0.0, 2.0, 2, audioClipBox_);
	clipVolSlider_->setToolTip(
		QStringLiteral("Level for this clip. 1.00 leaves it as recorded."));
	aForm->addRow(QStringLiteral("Volume"), clipVolSlider_);
	connect(clipVolSlider_, &ParamSlider::valueChanged, this, [this](double val) {
		if (syncingClip_)
			return;
		editSelectedClip([val](TlClip &c) { c.volume = val; });
	});

	clipFadeInSpin_ = new QSpinBox(audioClipBox_);
	clipFadeInSpin_->setRange(0, 60000);
	clipFadeInSpin_->setSuffix(QStringLiteral(" ms"));
	clipFadeInSpin_->setSingleStep(50);
	clipFadeInSpin_->setKeyboardTracking(false);
	aForm->addRow(QStringLiteral("Fade in"), clipFadeInSpin_);
	connect(clipFadeInSpin_, &QSpinBox::valueChanged, this, [this](int val) {
		if (syncingClip_)
			return;
		editSelectedClip([val](TlClip &c) { c.fadeInMs = val; });
	});

	clipFadeOutSpin_ = new QSpinBox(audioClipBox_);
	clipFadeOutSpin_->setRange(0, 60000);
	clipFadeOutSpin_->setSuffix(QStringLiteral(" ms"));
	clipFadeOutSpin_->setSingleStep(50);
	clipFadeOutSpin_->setKeyboardTracking(false);
	aForm->addRow(QStringLiteral("Fade out"), clipFadeOutSpin_);
	connect(clipFadeOutSpin_, &QSpinBox::valueChanged, this, [this](int val) {
		if (syncingClip_)
			return;
		editSelectedClip([val](TlClip &c) { c.fadeOutMs = val; });
	});

	// Curve pickers. The list comes from FadeCurve.hpp so the combo can never
	// drift from what the mixer actually implements.
	auto addCurveCombo = [&](const QString &label, QComboBox *&out, bool isIn) {
		out = new QComboBox(audioClipBox_);
		for (int i = 0; i < kFadeCurveCount; ++i)
			out->addItem(QString::fromLatin1(fadeCurveName(FadeCurve(i))));
		aForm->addRow(label, out);
		connect(out, &QComboBox::currentIndexChanged, this, [this, isIn](int idx) {
			if (syncingClip_)
				return;
			const FadeCurve fc = fadeCurveFromInt(idx);
			editSelectedClip([fc, isIn](TlClip &c) {
				(isIn ? c.fadeInCurve : c.fadeOutCurve) = fc;
			});
		});
	};
	addCurveCombo(QStringLiteral("Fade in curve"), clipFadeInCurve_, true);
	addCurveCombo(QStringLiteral("Fade out curve"), clipFadeOutCurve_, false);
	av->addLayout(aForm);

	av->addWidget(infoHint(QStringLiteral("Drag the round grips in an audio clip's top corners to set "
			       "the fades (double-click one to clear it, Shift for a "
			       "precise drag). Fades are applied to the mix, so the "
			       "preview plays what the export will render."), audioClipBox_), 0, Qt::AlignLeft);

	into->addWidget(clipBox_);
}

// Hide background controls and their form labels together when disabled.
void VideoEditorWindow::syncTextBoxRows()
{
	if (!textForm_ || !boxChk_)
		return;
	const bool on = boxChk_->isChecked();
	for (QWidget *w : {static_cast<QWidget *>(boxShapeCombo_), static_cast<QWidget *>(boxColorBtn_),
			   boxOpacityRow_,
			   static_cast<QWidget *>(boxPadXSpin_), static_cast<QWidget *>(boxPadYSpin_),
			   static_cast<QWidget *>(boxRadiusSpin_)})
		setFormRowVisible(textForm_, w, on);
}

// Pick one of a caption's colours, with the canvas following the picker.
//
// The static QColorDialog::getColor only answers on OK, so choosing a text or
// outline colour meant picking against a swatch, pressing OK, looking at the
// result and going back in -- on a caption whose legibility is the entire point
// of the colour. This is the live picker the Dev panel's timeline colours
// already use: every movement inside the dialog applies to the clip and
// repaints the preview, and Cancel puts the original back exactly.
//
// The colour is reached by member pointer so text, outline and box share one
// implementation; three copies of this would be three chances for one of them
// to keep the old dead-end behaviour.
void VideoEditorWindow::pickTextColor(const QString &title, QColor TlText::*field)
{
	const TlClip *sel = inspectedClip();
	if (!sel)
		return;
	const QColor original = sel->text.*field;
	// The shared picker (ui/ColorField.hpp) previews on every movement and
	// calls back once more with the settled colour -- the chosen one, or the
	// original on Cancel, undoing whatever the live preview painted.
	//
	// Each call goes through editSelectedClip like every other inspector edit,
	// so the snapshot coalescing that already covers a dragged slider covers a
	// dragged colour too, and the selection is re-read every time: `sel` was
	// taken before the dialog opened and every preview since has replaced it.
	pickColorLive(this, title, original, [this, field](const QColor &c) {
		const TlClip *now = inspectedClip();
		if (now && now->text.*field != c)
			editTextStyle([c, field](TlClip &cl) { cl.text.*field = c; });
	});
}

namespace {
ComponentInstance &ensureCaptionControlComponent(TlClip &clip, const QString &typeId)
{
	for (ComponentInstance &component : clip.components)
		if (component.typeId == typeId)
			return component;
	ComponentInstance component;
	component.typeId = typeId;
	component.instanceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	clip.components.append(component);
	return clip.components.last();
}

// A batch edit is STYLE only. Every caption keeps its own words whatever the
// edit was: the Inspector's text fields hold one caption's text in a batch,
// and anything that reached them (a quick-tag button did) used to write that
// one caption's words into all of them.
std::function<void(TlClip &)> styleOnly(const std::function<void(TlClip &)> &fn)
{
	return [&fn](TlClip &c) {
		const QString words = c.text.text;
		const QStringList variations = c.text.variations;
		const bool random = c.text.random;
		const QVector<ClipWordTime> times = c.words;
		fn(c);
		c.text.text = words;
		c.text.variations = variations;
		c.text.random = random;
		c.words = times;
	};
}
} // namespace

void VideoEditorWindow::setCaptionAnimationMode(int mode)
{
	mode = std::clamp(mode, 0, 4);
	commitSnapshot();
	editTextStyle([mode](TlClip &clip) {
		if (clip.type != TlClip::Type::Text)
			return;
		if (mode == 0 && std::none_of(clip.components.begin(), clip.components.end(), [](const ComponentInstance &ci) {
			return ci.typeId == QStringLiteral("harpia.subtitle");
		}))
			return;
		ComponentInstance &component = ensureCaptionControlComponent(clip, QStringLiteral("harpia.subtitle"));
		component.enabled = true;
		component.props.insert(QStringLiteral("mode"), mode);
		component.keys.remove(QStringLiteral("mode"));
	});
	commitSnapshot();
}

void VideoEditorWindow::setCaptionPopEnabled(bool enabled)
{
	commitSnapshot();
	editTextStyle([enabled](TlClip &clip) {
		if (clip.type != TlClip::Type::Text)
			return;
		if (enabled) {
			ensureCaptionControlComponent(clip, QStringLiteral("harpia.captionPop")).enabled = true;
		} else {
			for (ComponentInstance &component : clip.components)
				if (component.typeId == QStringLiteral("harpia.captionPop"))
					component.enabled = false;
		}
	});
	commitSnapshot();
}

void VideoEditorWindow::pickCaptionHighlightColor()
{
	if (!inspectedClip())
		return;
	const QColor original = captionHighlightColor_->property("harpiaColor").value<QColor>();
	const QColor chosen = QColorDialog::getColor(original.isValid() ? original : QColor(QStringLiteral("#ffe45e")),
		this, QStringLiteral("Caption highlight colour"));
	if (!chosen.isValid())
		return;
	commitSnapshot();
	editTextStyle([chosen](TlClip &clip) {
		if (clip.type != TlClip::Type::Text)
			return;
		ComponentInstance &component = ensureCaptionControlComponent(clip, QStringLiteral("harpia.subtitle"));
		component.props.insert(QStringLiteral("activeColor"), chosen);
		component.keys.remove(QStringLiteral("activeColor"));
	});
	commitSnapshot();
}

QVector<QPair<int, int>> VideoEditorWindow::selectedTextBatch() const
{
	QVector<QPair<int, int>> out;
	if (!timelineView_ || !fullEdit() || timelineView_->transitionSelected())
		return out;
	const TlClip *primary = timelineView_->selectedClipPtr();
	if (!primary || primary->type != TlClip::Type::Text)
		return out;
	const TimelineModel &m = timelineView_->model();
	for (const auto &p : timelineView_->selectedPairs())
		if (m.tracks[p.first].clips[p.second].type == TlClip::Type::Text)
			out.append(p);
	if (out.size() < 2)
		out.clear();
	return out;
}

void VideoEditorWindow::editTextStyle(const std::function<void(TlClip &)> &fn)
{
	const QVector<QPair<int, int>> texts = selectedTextBatch();
	if (texts.isEmpty()) {
		editSelectedClip(fn); // one clip, or a whole caption lane
		return;
	}
	// Several texts selected: the same look on each, as one undo step.
	if (timelineView_->editClips(texts, styleOnly(fn)) == 0)
		return;
	showTimelineFrame(timelineEditMs());
	syncClipInspector();
	scheduleSnapshot();
}

void VideoEditorWindow::editSelectedClip(const std::function<void(TlClip &)> &fn)
{
	if (!timelineView_)
		return;
	const TlClip *sel = timelineView_->selectedClipPtr();
	if (!sel && captionLaneTrack_ >= 0) {
		// Batch style: the same edit on every caption of the selected lane.
		if (timelineView_->editLaneCaptions(captionLaneTrack_, styleOnly(fn)) == 0)
			return;
		showTimelineFrame(timelinePlayheadMs());
		syncClipInspector();
		scheduleSnapshot();
		return;
	}
	if (!sel)
		return;
	TlClip c = *sel;
	fn(c);
	timelineView_->updateSelectedClip(c);
	syncPreviewTransformTarget();
	// Park the playhead where the edit actually applies, so the preview shows
	// the clip being changed instead of whatever (or nothing) is at the raw
	// playhead, and the marker agrees with what is on screen.
	const qint64 at = timelineEditMs();
	timelineView_->setPlayhead(at);
	showTimelineFrame(at);
	syncClipInspector();
	scheduleSnapshot();
}

// The floating keyframe editor. Not modal and not owned by the clip: it edits a
// copy and pushes each change straight back through updateSelectedClip, which is
// the same path the Inspector uses — so the preview refreshes and each edit is
// one undo step. Closing it therefore cannot lose anything.
// The floating shortcuts panel. Kept once created, so it remembers where it was
// and what was being searched for.
void VideoEditorWindow::openShortcutPanel()
{
	if (!shortcutPanel_)
		shortcutPanel_ = new ShortcutPanel(shortcuts_, this);
	shortcutPanel_->show();
	shortcutPanel_->raise();
	shortcutPanel_->activateWindow();
}

// Put each button's current shortcut in its tooltip. Driven by the registry, so
// rebinding one updates the tooltip immediately rather than leaving a stale
// hint baked in at construction.
void VideoEditorWindow::refreshShortcutHints()
{
	if (!shortcuts_)
		return;
	struct Hint {
		QPushButton *btn;
		const char *id;
		const char *base;
		const char *mouse = nullptr;
	};
	const Hint hints[] = {
		{playBtn_, "playback.playPause", "Play / pause the preview"},
		{undoBtn_, "edit.undo", "Undo", "Mouse Back"},
		{redoBtn_, "edit.redo", "Redo", "Mouse Forward"},
		{snapBtn_, "timeline.snap", nullptr}, // has its own on/off tooltip
		{fitBtn_, "timeline.fit", "Zoom the timeline out so the whole edit fits"},
	};
	for (const Hint &h : hints) {
		if (!h.btn || !h.base)
			continue;
		QString keys = shortcuts_->displayText(QLatin1String(h.id));
		if (h.mouse) {
			if (!keys.isEmpty())
				keys += QStringLiteral(" / ");
			keys += QLatin1String(h.mouse);
		}
		h.btn->setToolTip(keys.isEmpty() ? QLatin1String(h.base)
						 : QStringLiteral("%1 (%2)")
							   .arg(QLatin1String(h.base), keys));
	}
}

void VideoEditorWindow::openKeyframeEditor()
{
	if (!timelineView_)
		return;
	const TlClip *sel = timelineView_->selectedClipPtr();
	if (!sel)
		return;
	if (!keyEditor_) {
		keyEditor_ = new KeyframeEditor(this);
		keyEditor_->setLayoutParams(keyframeLayout_);
		connect(keyEditor_, &KeyframeEditor::clipChanged, this, [this](const TlClip &c) {
			if (!timelineView_ || !timelineView_->selectedClipPtr())
				return;
			timelineView_->updateSelectedClip(c);
			syncPreviewTransformTarget();
			// Show the frame the edit actually applies to, as the Inspector
			// does, so the change is visible rather than off-screen.
			const qint64 at = timelineEditMs();
			timelineView_->setPlayhead(at);
			showTimelineFrame(at);
			syncClipInspector();
			// A keyframe edit is a finished action: close its undo entry now
			// rather than letting the next one join it.
			commitSnapshot();
		});
		connect(keyEditor_, &KeyframeEditor::scrubRequested, this, [this](qint64 outMs) {
			if (!timelineView_)
				return;
			timelineView_->setPlayhead(outMs);
			requestPreview(-1, outMs);
		});
	}
	refreshKeyframeEditor();
	keyEditor_->show();
	keyEditor_->raise();
	keyEditor_->activateWindow();
}

void VideoEditorWindow::refreshKeyframeEditor()
{
	if (!keyEditor_ || !keyEditor_->isVisible() || !timelineView_)
		return;
	const TlClip *sel = timelineView_->selectedClipPtr();
	if (!sel)
		return;
	const int t = timelineView_->selectedTrack();
	const auto &tracks = timelineView_->model().tracks;
	const QString label = (t >= 0 && t < tracks.size())
				      ? QStringLiteral("%1 · %2").arg(tracks[t].name, clipLabel(*sel))
				      : clipLabel(*sel);
	keyEditor_->setClip(*sel, label);
	keyEditor_->setPlayheadOut(timelinePlayheadMs());
}

// What to call a clip in a panel's heading: its file, its words, or what the
// effect does. Shared by the Source tab and the keyframe editor's title bar,
// which were building the same string two different ways.
QString VideoEditorWindow::clipLabel(const TlClip &c) const
{
	switch (c.type) {
	case TlClip::Type::Text:
		// The first line only: a caption can be a paragraph, and a heading that
		// grows to five lines pushes the panel around.
		return tlPlainText(c.text).split(QLatin1Char('\n')).value(0);
	case TlClip::Type::Effect:
		return c.effectLabel([](const QString &id) {
			const ComponentType *t = ComponentRegistry::instance().find(id);
			return t ? t->displayName : id;
		});
	default:
		break;
	}
	for (const EditorSource &s : sources_)
		if (s.id == c.sourceId)
			return s.name.isEmpty() ? QFileInfo(s.path).fileName() : s.name;
	return QStringLiteral("#%1").arg(c.sourceId);
}

// A script that defines a channel computes that channel's final value, so the
// number in the Transform row is no longer what you see on screen — it becomes
// the script's STARTING point (the script reads it as ctx.base). The row says
// so rather than leaving the mismatch to be discovered.
//
// Returns the property keys a script is driving, and fills *tip with which
// scripts they are. The pose rows live in the component panel now, so this
// reports rather than restyles.
QStringList VideoEditorWindow::scriptDrivenPoseKeys(const TlClip &c, QString *tip) const
{
	int mask = 0;
	QStringList driving;
	if (scriptEval_) {
		for (const TlScript &s : c.scripts) {
			if (s.name.isEmpty())
				continue;
			const int m = scriptEval_->channelsOf(s.name);
			if (m) {
				mask |= m;
				driving << s.name;
			}
		}
	}
	driving.removeDuplicates();

	QStringList keys;
	if (mask & TransformEvaluator::ChanScale)
		keys << QStringLiteral("scale");
	if (mask & TransformEvaluator::ChanPosition)
		keys << QStringLiteral("posX") << QStringLiteral("posY");
	if (mask & TransformEvaluator::ChanRotation)
		keys << QStringLiteral("rotation");
	if (mask & TransformEvaluator::ChanOpacity)
		keys << QStringLiteral("opacity");
	if (tip)
		*tip = keys.isEmpty()
			       ? QString()
			       : QStringLiteral("Driven by %1. This value is where the script "
						"starts from (it reads it as ctx.base), not the "
						"framing you see — change it and the whole move "
						"shifts with it.")
					 .arg(driving.join(QStringLiteral(", ")));
	return keys;
}

void VideoEditorWindow::syncClipInspector()
{
	// The component list follows the clip like every other per-clip control, so
	// it refreshes on the same call — after an edit AND after a selection
	// change, which arrive through different paths.
	syncComponentPanel();

	if (!clipBox_ || !timelineView_)
		return;
	syncTransitionInspector();
	// A transition belongs to neither clip, so the clip panel steps aside while
	// one is selected.
	const bool onTransition = fullEdit() && timelineView_->transitionSelected();
	const TlClip *c = (fullEdit() && !onTransition) ? timelineView_->selectedClipPtr() : nullptr;
	// No clip, but a caption lane's header selected: style all of its captions.
	captionLaneTrack_ = (fullEdit() && !onTransition && !c) ? captionLaneForBatch() : -1;
	const bool batch = captionLaneTrack_ >= 0;
	if (batch)
		c = inspectedClip();
	// Several texts selected: the clicked one is shown, and its style
	// controls reach all of them -- said at the top of its Text section.
	if (textBatchNote_) {
		const int texts = batch ? 0 : int(selectedTextBatch().size());
		textBatchNote_->setVisible(texts >= 2);
		if (texts >= 2)
			textBatchNote_->setText(
				QStringLiteral("%1 texts selected: the style below applies to all of them. The words "
					       "box edits this one only.")
					.arg(texts));
	}
	if (captionLaneBox_)
		captionLaneBox_->setVisible(batch);
	if (clipTagsBox_)
		clipTagsBox_->setVisible(!batch);
	if (textEdit_)
		textEdit_->setVisible(!batch);
	// The quick tags wrap words in one caption's text box: with a whole lane
	// selected there is no such box, and nothing for them to do.
	if (quickTagsRow_)
		quickTagsRow_->setVisible(!batch);
	// A Random text's other texts belong to that one caption: shown for a
	// Random text only, and never for a whole lane of captions at once.
	const bool randomText = !batch && c && c->type == TlClip::Type::Text && c->text.random;
	if (variationsBox_)
		variationsBox_->setVisible(randomText);
	if (textMainCap_)
		textMainCap_->setVisible(randomText);
	if (!randomText && textEdit_)
		textEdit_->setStyleSheet(QString()); // no "being previewed" outline
	if (textSection_)
		textSection_->setTitle(randomText ? QStringLiteral("Random text") : QStringLiteral("Text"));
	clipBox_->setVisible(c != nullptr);
	if (clipTailBox_)
		clipTailBox_->setVisible(c != nullptr);
	// The tab is never blank: with nothing selected it says so, rather than
	// showing an empty column that reads as a panel that failed to load.
	if (clipEmpty_)
		clipEmpty_->setVisible(!c && !onTransition &&
				       (!componentPanel_ || componentPanel_->isHidden()));
	if (!c)
		return;
	if (batch) {
		videoClipBox_->setVisible(false);
		if (audioClipBox_)
			audioClipBox_->setVisible(false);
		if (randomClipBox_)
			randomClipBox_->setVisible(false);
		const bool wasSyncingLane = syncingClip_;
		syncingClip_ = true;
		const TimelineModel &lm = timelineView_->model();
		captionLaneTitle_->setText(QStringLiteral("All %1 captions on %2")
						   .arg(timelineView_->laneCaptionCount(captionLaneTrack_))
						   .arg(lm.tracks[captionLaneTrack_].name));
		captionLanePos_->setCurrentIndex(0);
		captionLaneX_->setValue(c->posX * 100.0);
		captionLaneY_->setValue(c->posY * 100.0);
		textBox_->setVisible(true);
		syncTextInspector(*c);
		syncingClip_ = wasSyncingLane;
		return;
	}
	syncClipTags();

	// Save/restore rather than clear: a live edit can reach here while an outer
	// sync is already in progress, and clearing the flag would let the widgets
	// being repopulated write back into the clip.
	const bool wasSyncing = syncingClip_;
	syncingClip_ = true;
	const qint64 ph = inspectTimeMs();
	autoKeyChk_->setChecked(autoKeyframe_);
	const int here = c->keyframeIndexAt(ph);
	keyInfo_->setText(c->keys.isEmpty()
				  ? QStringLiteral("No animation — the clip holds one fixed framing.")
				  : QStringLiteral("%1 keyframe%2%3")
					    .arg(c->keys.size())
					    .arg(c->keys.size() == 1 ? QString() : QStringLiteral("s"))
					    .arg(here >= 0 ? QStringLiteral(" · on one now") : QString()));

	// The list itself: one row per key, in time order, with the channels it
	// pins. Rebuilt rather than patched -- a few keys is a few rows, and a
	// partial update is where a stale entry would come from.
	if (keyList_) {
		keyList_->clear();
		for (int i = 0; i < c->keys.size(); ++i) {
			const TlKeyframe &k = c->keys[i];
			QStringList lanes;
			for (int l = 0; l < kTlLaneCount; ++l)
				if (k.channel(l).on)
					lanes << QString::fromLatin1(tlLaneName(l));
			// A key that pins everything says so once instead of listing four.
			const QString what = lanes.size() == kTlLaneCount
						     ? QStringLiteral("all")
						     : (lanes.isEmpty() ? QStringLiteral("—")
									: lanes.join(QStringLiteral(", ")));
			auto *item = new QListWidgetItem(
				QStringLiteral("%1   %2   %3")
					.arg(i + 1, 2)
					.arg(timeTextCentis(k.tMs), -8)
					.arg(what));
			item->setData(Qt::UserRole, qlonglong(k.tMs));
			item->setData(Qt::UserRole + 1, i); // which key, for drag-to-reorder
			keyList_->addItem(item);
		}
		// Highlight the one the instant being described is on, so the list and
		// the preview agree about where you are.
		keyList_->setCurrentRow(here);
		keyList_->setVisible(!c->keys.isEmpty());
	}

	// Transform script stack + the selected entry's parameter controls.
	refreshScriptList();
	rebuildScriptParams();


	// Which halves of the panel apply. A clip on an audio track has no picture,
	// so showing it zoom/rotation controls that do nothing would be a lie. The
	// Audio section goes with any MEDIA clip on a lane that carries sound: a
	// video clip's footage has a level and fades like a music clip's, and the
	// mix has always applied them -- there was simply no control for them on
	// a video lane.
	const int selTrack = timelineView_->selectedTrack();
	const auto &tracks = timelineView_->model().tracks;
	const bool onAudioTrack = selTrack >= 0 && selTrack < tracks.size() &&
				  tracks[selTrack].kind == TlTrack::Kind::Audio;
	const bool hasSound = selTrack >= 0 && selTrack < tracks.size() &&
			      TlTrack::kindHasSound(tracks[selTrack].kind) &&
			      c->type == TlClip::Type::Video; // a media clip, not a still or caption
	if (videoClipBox_)
		videoClipBox_->setVisible(!onAudioTrack);
	if (audioClipBox_) {
		audioClipBox_->setVisible(hasSound);
		if (hasSound) {
			clipVolSlider_->setValue(c->volume);
			// Set the ceiling before the values, or a fade longer than the
			// last clip's would be silently truncated on the way in.
			// A fade may run the whole clip; overlapping fades multiply,
			// which dips the middle rather than clicking.
			const int full = int(std::max<qint64>(1, c->outDurationMs()));
			clipFadeInSpin_->setMaximum(full);
			clipFadeOutSpin_->setMaximum(full);
			clipFadeInSpin_->setValue(c->fadeInMs);
			clipFadeOutSpin_->setValue(c->fadeOutMs);
			clipFadeInCurve_->setCurrentIndex(int(c->fadeInCurve));
			clipFadeOutCurve_->setCurrentIndex(int(c->fadeOutCurve));
		}
	}

	// An effect clip's transform used to be hidden, on the grounds that a clip
	// with no picture of its own cannot be posed. It can: the pose is the AREA
	// the grade lands in, so position, zoom and rotation decide which part of the
	// composite gets the effect, and hiding them left the whole feature
	// unreachable.
	const bool isFx = c->type == TlClip::Type::Effect;
	if (videoClipBox_)
		videoClipBox_->setVisible(!onAudioTrack);

	const bool isText = !onAudioTrack && !isFx && c->type == TlClip::Type::Text;
	textBox_->setVisible(isText);
	if (isText)
		syncTextInspector(*c);
	syncRandomClipInspector(*c);
	syncingClip_ = wasSyncing;
}

// The Text section's controls, from one caption: the selected clip, or in
// batch style the lane's first caption (the one the others are compared to).
void VideoEditorWindow::syncTextInspector(const TlClip &c)
{
	refreshTextPresets();
	const ComponentInstance *subtitle = nullptr;
	bool popEnabled = false;
	for (const ComponentInstance &component : c.components) {
		if (component.typeId == QStringLiteral("harpia.subtitle") && !subtitle)
			subtitle = &component;
		if (component.typeId == QStringLiteral("harpia.captionPop") && component.enabled)
			popEnabled = true;
	}
	int animationMode = 0;
	QColor activeColor(QStringLiteral("#ffe45e"));
	if (subtitle) {
		const qint64 at = std::clamp<qint64>(timelinePlayheadMs() - c.outStartMs, 0, c.outDurationMs());
		const ComponentType *type = ComponentRegistry::instance().find(subtitle->typeId);
		const QVariant mode = type ? propAt(*type, *subtitle, QStringLiteral("mode"), at)
					   : subtitle->props.value(QStringLiteral("mode"), 0);
		animationMode = subtitle->enabled ? std::clamp(mode.toInt(), 0, 4) : 0;
		const QVariant colour = type ? propAt(*type, *subtitle, QStringLiteral("activeColor"), at)
					     : subtitle->props.value(QStringLiteral("activeColor"), activeColor);
		activeColor = colour.canConvert<QColor>() ? colour.value<QColor>() : QColor::fromRgba(colour.toUInt());
	}
	captionAnimationCombo_->setCurrentIndex(animationMode);
	captionPopCheck_->setChecked(popEnabled);
	captionHighlightColor_->setEnabled(animationMode >= 3);
	styleSwatch(captionHighlightColor_, activeColor);
	captionHighlightColor_->setProperty("harpiaColor", activeColor);
	if (textEdit_->toPlainText() != c.text.text)
		textEdit_->setPlainText(c.text.text);
	syncVariationsInspector(c);
	// Only when it actually differs: setCurrentFont on every refresh makes the
	// combo re-resolve the family, which is needless work now that this runs
	// on each mouse-move of a drag.
	if (!c.text.fontFamily.isEmpty() &&
	    fontCombo_->currentFont().family() != c.text.fontFamily)
		fontCombo_->setCurrentFont(QFont(c.text.fontFamily));
	fontSizeSpin_->setValue(c.text.fontPx);
	boldChk_->setChecked(c.text.bold);
	italicChk_->setChecked(c.text.italic);
	alignCombo_->setCurrentIndex(std::clamp(c.text.align, 0, 2));
	titleCaseChk_->setChecked(c.text.textCase == TlCaseTitle);
	outlineWSpin_->setValue(c.text.outlineWidth);
	boxChk_->setChecked(c.text.boxEnabled);
	boxShapeCombo_->setCurrentIndex(c.text.boxShape == TlTextBoxFitText ? 1 : 0);
	// setChecked only emits when the value CHANGES, so selecting a second
	// clip with the same box setting would leave the rows as the last one
	// left them. Called directly, it is right either way.
	syncTextBoxRows();
	boxPadXSpin_->setValue(c.text.boxPadX);
	boxPadYSpin_->setValue(c.text.boxPadY);
	boxRadiusSpin_->setValue(c.text.boxRadius);
	boxOpacitySlider_->setValue(int(std::lround(std::clamp(c.text.boxOpacity, 0.0, 1.0) * 100.0)));
	styleSwatch(textColorBtn_, c.text.color);
	styleSwatch(outlineColorBtn_, c.text.outlineColor);
	styleSwatch(boxColorBtn_, c.text.boxColor);
}

// ---- Random clip ----------------------------------------------------------------
//
// One slot, several clips: each is a version of the video on export, like a
// Random text's texts. The rules (what is shared, what may be added, how the
// shown one and the list stay in step) are in timeline/ClipVariations.hpp;
// this is the list in the Clip tab.

QString VideoEditorWindow::sourceLabel(int sourceId) const
{
	for (const EditorSource &s : sources_)
		if (s.id == sourceId)
			return QFileInfo(s.name.isEmpty() ? s.path : s.name).completeBaseName();
	return QString();
}

void VideoEditorWindow::buildRandomClipSection(QVBoxLayout *clipOuter)
{
	auto *sec = new InspectorSection(QStringLiteral("Random clip"), QStringLiteral("clip/random"), true, clipBox_);
	randomClipBox_ = sec;
	randomClipBox_->setVisible(false);
	clipOuter->insertWidget(2, randomClipBox_);
	auto *v = sec->bodyLayout();
	v->setSpacing(5);
	auto *intro = new QLabel(QStringLiteral("Each clip is one version of the video (Export \u2192 Variations). "
						"They share this slot's place, length and look. Show one, then "
						"right-click the clip \u203A Slip\u2026 to pick its moment."),
				 randomClipBox_);
	intro->setWordWrap(true);
	intro->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	v->addWidget(intro);
	auto *rows = new QWidget(randomClipBox_);
	randomClipRows_ = new QVBoxLayout(rows);
	randomClipRows_->setContentsMargins(0, 0, 0, 0);
	randomClipRows_->setSpacing(3);
	v->addWidget(rows);

	randomClipAdd_ = new QPushButton(QStringLiteral("+  Add clip"), randomClipBox_);
	randomClipAdd_->setObjectName(QStringLiteral("addRandomClipButton"));
	randomClipAdd_->setToolTip(QStringLiteral("Another clip for this slot: from the project's media, or a file"));
	auto *menu = new QMenu(randomClipAdd_);
	menu->setToolTipsVisible(true);
	connect(menu, &QMenu::aboutToShow, this, [this, menu]() { fillRandomClipAddMenu(menu); });
	randomClipAdd_->setMenu(menu);
	v->addWidget(randomClipAdd_);

	auto *row = new QHBoxLayout;
	randomClipPrev_ = new QPushButton(randomClipBox_);
	randomClipNext_ = new QPushButton(randomClipBox_);
	randomClipPrev_->setIcon(uiIcon(Glyph::StepBack, 14));
	randomClipNext_->setIcon(uiIcon(Glyph::StepForward, 14));
	for (QPushButton *b : {randomClipPrev_, randomClipNext_}) {
		b->setFixedSize(42, 32);
		b->setStyleSheet(QStringLiteral(
			"padding:0px; min-width:42px; max-width:42px; min-height:32px; max-height:32px;"));
	}
	randomClipPrev_->setToolTip(QStringLiteral("Show the previous clip"));
	randomClipNext_->setToolTip(QStringLiteral("Show the next clip"));
	randomClipCount_ = new QLabel(randomClipBox_);
	row->addWidget(randomClipPrev_);
	row->addWidget(randomClipCount_, 1, Qt::AlignCenter);
	row->addWidget(randomClipNext_);
	v->addLayout(row);
	const auto step = [this](int d) {
		const TlClip *c = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
		if (!c || !c->isRandomClip())
			return;
		const int n = int(c->clipOptions.size());
		showRandomClipOption(((c->clipShown + d) % n + n) % n);
	};
	connect(randomClipPrev_, &QPushButton::clicked, this, [step]() { step(-1); });
	connect(randomClipNext_, &QPushButton::clicked, this, [step]() { step(+1); });
}

void VideoEditorWindow::syncRandomClipInspector(const TlClip &c)
{
	if (!randomClipBox_)
		return;
	const bool on = c.type == TlClip::Type::Video && c.isRandomClip();
	randomClipBox_->setVisible(on);
	if (!on)
		return;
	const int n = int(c.clipOptions.size());
	// A row per choice, made only when the count changes. Rebuilt then rather
	// than patched, and a row's own click (its ×) is often what got us here:
	// deleteLater lets that click finish first.
	if (randomClipPicks_.size() != n) {
		while (QLayoutItem *it = randomClipRows_->takeAt(0)) {
			if (QWidget *w = it->widget()) {
				w->hide();
				w->deleteLater();
			}
			delete it;
		}
		randomClipPicks_.clear();
		randomClipDels_.clear();
		for (int k = 0; k < n; ++k) {
			auto *row = new QWidget(randomClipBox_);
			auto *h = new QHBoxLayout(row);
			h->setContentsMargins(0, 0, 0, 0);
			h->setSpacing(4);
			auto *pick = new QPushButton(row);
			pick->setObjectName(QStringLiteral("randomClipOption"));
			pick->setCheckable(true);
			// Never wider than the Inspector: a long file name is cut, in full on hover.
			pick->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
			pick->setMinimumWidth(60);
			pick->setStyleSheet(QStringLiteral("QPushButton{text-align:left; padding:4px 8px;}"
							   "QPushButton:checked{border:2px solid #22c55e;}"));
			connect(pick, &QPushButton::clicked, this, [this, k]() { showRandomClipOption(k); });
			h->addWidget(pick, 1);
			auto *del = new QPushButton(row);
			del->setIcon(uiIcon(Glyph::Cross, 13));
			del->setFixedWidth(28);
			connect(del, &QPushButton::clicked, this, [this, k]() { removeRandomClipOption(k); });
			h->addWidget(del);
			randomClipRows_->addWidget(row);
			randomClipPicks_.append(pick);
			randomClipDels_.append(del);
		}
	}
	// What each row says. setText and the rest do nothing when unchanged.
	for (int k = 0; k < n; ++k) {
		const TlClipOption o = c.clipOptionAt(k);
		const QString full =
			QStringLiteral("%1.  %2").arg(k + 1).arg(clip_variations::label(sourceLabel(o.sourceId), o));
		QPushButton *pick = randomClipPicks_[k];
		pick->setText(full.size() > 38 ? full.left(36) + QStringLiteral("\u2026") : full);
		pick->setChecked(k == c.clipShown);
		pick->setToolTip(k == c.clipShown
					 ? full + QStringLiteral("\nShowing now. Right-click the clip \u203A Slip\u2026 to pick "
								 "its moment.")
					 : full + QStringLiteral("\nShow this clip in the slot."));
		QPushButton *del = randomClipDels_[k];
		del->setEnabled(n > 1);
		del->setToolTip(n > 1 ? QStringLiteral("Remove this clip")
				      : QStringLiteral("The last clip stays. To stop this being a Random clip, "
						       "right-click it \u203A Make normal clip."));
	}
	randomClipCount_->setText(QStringLiteral("Clip %1 of %2").arg(c.clipShown + 1).arg(n));
	randomClipPrev_->setEnabled(n > 1);
	randomClipNext_->setEnabled(n > 1);
}

void VideoEditorWindow::showRandomClipOption(int k)
{
	const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!sel || !sel->isRandomClip())
		return;
	const int track = timelineView_->selectedTrack();
	const auto &tracks = timelineView_->model().tracks;
	const bool audioLane = track >= 0 && track < tracks.size() && tracks[track].kind == TlTrack::Kind::Audio;
	editSelectedClip([this, k, audioLane](TlClip &c) {
		if (!clip_variations::show(c, k))
			return;
		// An audio lane draws a waveform, and it belongs to the file now playing.
		if (audioLane)
			if (const EditorSource *s = sourceById(c.sourceId))
				c.peaks = randomClipPeaks(s->path);
	});
}

QVector<float> VideoEditorWindow::randomClipPeaks(const QString &path)
{
	// The size and the time are in the key, so a file written over since is
	// read again rather than drawn as it was.
	const QFileInfo fi(path);
	const QString key = path + QLatin1Char('|') + QString::number(fi.size()) + QLatin1Char('|') +
			    QString::number(fi.lastModified().toMSecsSinceEpoch());
	const auto it = randomClipPeaks_.constFind(key);
	if (it != randomClipPeaks_.constEnd())
		return it.value();
	const QVector<float> peaks = VoiceoverTrack::loadPeaks(path, 600);
	if (randomClipPeaks_.size() >= 64) // a project's worth; never without bound
		randomClipPeaks_.clear();
	randomClipPeaks_.insert(key, peaks);
	return peaks;
}

void VideoEditorWindow::addRandomClipOption(int sourceId)
{
	const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	const EditorSource *s = sourceById(sourceId);
	if (!sel || sel->type != TlClip::Type::Video || !s)
		return;
	QString why;
	if (!clip_variations::fits(*sel, s->durationMs, &why)) {
		warnAndLog(this, QStringLiteral("Random clip"),
			   QStringLiteral("%1 can't be added. %2").arg(sourceLabel(sourceId), why));
		return;
	}
	const qint64 total = s->durationMs;
	int added = -1;
	editSelectedClip([&added, sourceId, total](TlClip &c) {
		added = clip_variations::add(c, sourceId, total);
		if (added >= 0)
			clip_variations::show(c, added); // shown at once, ready to slip
	});
	// The waveform, on an audio lane, through the same path a click takes.
	if (added >= 0)
		showRandomClipOption(added);
}

void VideoEditorWindow::removeRandomClipOption(int k)
{
	const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!sel || !sel->isRandomClip())
		return;
	const int shownBefore = sel->clipShown;
	editSelectedClip([k](TlClip &c) { clip_variations::remove(c, k); });
	const TlClip *now = timelineView_->selectedClipPtr();
	if (now && k == shownBefore)
		showRandomClipOption(now->clipShown); // reload the waveform of the one now showing
}

void VideoEditorWindow::fillRandomClipAddMenu(QMenu *menu)
{
	menu->clear();
	const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!sel || sel->type != TlClip::Type::Video)
		return;
	const int track = timelineView_->selectedTrack();
	const auto &tracks = timelineView_->model().tracks;
	const bool audioLane = track >= 0 && track < tracks.size() && tracks[track].kind == TlTrack::Kind::Audio;
	for (const EditorSource &es : sources_) {
		if (es.durationMs <= 0)
			continue; // a still has no time to fill a slot with
		if (!audioLane && es.width <= 0)
			continue; // a picture lane needs pictures
		QAction *a = menu->addAction(QStringLiteral("%1  (%2 s)")
						     .arg(sourceLabel(es.id))
						     .arg(es.durationMs / 1000.0, 0, 'f', 1));
		QString why;
		const bool ok = clip_variations::fits(*sel, es.durationMs, &why);
		a->setEnabled(ok);
		a->setToolTip(ok ? QStringLiteral("Add it, starting at its beginning; slip it afterwards.") : why);
		const int id = es.id;
		connect(a, &QAction::triggered, this, [this, id]() { addRandomClipOption(id); });
	}
	if (!menu->isEmpty())
		menu->addSeparator();
	connect(menu->addAction(QStringLiteral("Import a file…")), &QAction::triggered, this,
		&VideoEditorWindow::importRandomClipOption);
}

void VideoEditorWindow::importRandomClipOption()
{
	const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!sel || sel->type != TlClip::Type::Video)
		return;
	const int track = timelineView_->selectedTrack();
	const auto &tracks = timelineView_->model().tracks;
	const bool audioLane = track >= 0 && track < tracks.size() && tracks[track].kind == TlTrack::Kind::Audio;
	const QStringList paths = QFileDialog::getOpenFileNames(
		this, QStringLiteral("Add clips"), QString(),
		audioLane ? audioOpenFilter()
			  : QStringLiteral("Video files (*.mp4 *.mov *.mkv *.webm *.avi *.m4v *.gif *.wmv *.flv *.ts);;"
					   "All files (*)"));
	for (const QString &p : paths) {
		const int id = isAudioFile(p) ? addAudioSource(p) : addSource(p);
		if (id >= 0)
			addRandomClipOption(id);
	}
}

int VideoEditorWindow::captionLaneForBatch() const
{
	if (!timelineView_)
		return -1;
	const int t = timelineView_->selectedHeaderTrack();
	return timelineView_->laneCaptionCount(t) > 0 ? t : -1;
}

const TlClip *VideoEditorWindow::inspectedClip() const
{
	if (!timelineView_)
		return nullptr;
	if (const TlClip *sel = timelineView_->selectedClipPtr())
		return sel;
	if (captionLaneTrack_ < 0)
		return nullptr;
	const TimelineModel &m = timelineView_->model();
	if (captionLaneTrack_ >= m.tracks.size())
		return nullptr;
	for (const TlClip &c : m.tracks[captionLaneTrack_].clips)
		if (c.type == TlClip::Type::Text)
			return &c;
	return nullptr;
}

void VideoEditorWindow::addKeyframeAtPlayhead()
{
	const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!sel)
		return;
	const qint64 ph = std::clamp<qint64>(timelinePlayheadMs(), sel->outStartMs, sel->outEndMs());
	const TlTransform tf = sel->transformAt(ph);
	editSelectedClip([ph, tf](TlClip &c) {
		if (c.keys.isEmpty()) {
			TlKeyframe seed;
			seed.tMs = 0;
			seed.tf = c.baseTransform();
			c.keys.append(seed);
		}
		c.setKeyframeAt(ph, tf);
	});
}

void VideoEditorWindow::removeKeyframeAtPlayhead()
{
	const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!sel)
		return;
	const int idx = sel->keyframeIndexAt(timelinePlayheadMs());
	if (idx < 0)
		return;
	editSelectedClip([idx](TlClip &c) { removeKeyframe(c, idx); });
}

void VideoEditorWindow::keyRowAction(int row, KeyRowAction act)
{
	const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!sel || row < 0 || row >= sel->keys.size())
		return;
	if (playing_)
		stopPlayback();
	const int last = int(sel->keys.size()) - 1;
	int showRow = -1; // the key to park the playhead on afterwards
	switch (act) {
	case KeyRowAction::MoveFirst:
	case KeyRowAction::MoveLast: {
		const int to = act == KeyRowAction::MoveFirst ? 0 : last;
		if (to == row)
			return;
		editSelectedClip([row, to](TlClip &c) { moveKeyPose(c.keys, row, to); });
		showRow = to;
		break;
	}
	case KeyRowAction::Reset:
		editSelectedClip([row](TlClip &c) {
			if (row < c.keys.size())
				resetKeyFraming(c.keys[row]);
		});
		showRow = row;
		break;
	case KeyRowAction::Delete:
		editSelectedClip([row](TlClip &c) { removeKeyframe(c, row); });
		pathKeyPicked_ = -1;
		// The next key along takes the highlight (the last one if this was the
		// end), so pressing Delete again keeps going down the list.
		if (const TlClip *c = timelineView_->selectedClipPtr(); c && !c->keys.isEmpty())
			showRow = std::min(row, int(c->keys.size()) - 1);
		break;
	}
	commitSnapshot();
	refreshMotionPath();
	// Show the pose that moved (or was reset) where it now plays, so the
	// preview and the highlighted row are the thing that just changed.
	if (const TlClip *c = timelineView_->selectedClipPtr(); c && showRow >= 0 && showRow < c->keys.size()) {
		onTimelineScrub(c->outStartMs + c->keys[showRow].tMs);
		syncClipInspector();
	}
}

void VideoEditorWindow::applyKeyRowOrder(const QVector<int> &order, int landed)
{
	const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	QVector<TlKeyframe> probe = sel ? sel->keys : QVector<TlKeyframe>();
	if (!sel || !reorderKeyPoses(probe, order)) {
		syncClipInspector(); // a drop that changed nothing: put the rows back
		return;
	}
	if (playing_)
		stopPlayback();
	editSelectedClip([order](TlClip &c) { reorderKeyPoses(c.keys, order); });
	commitSnapshot();
	refreshMotionPath();
	if (const TlClip *c = timelineView_->selectedClipPtr(); c && landed >= 0 && landed < c->keys.size()) {
		onTimelineScrub(c->outStartMs + c->keys[landed].tMs);
		syncClipInspector();
	}
}

void VideoEditorWindow::keyRowMenu(int row, const QPoint &globalPos)
{
	const TlClip *c = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!c || row < 0 || row >= c->keys.size())
		return;
	const int last = int(c->keys.size()) - 1;
	QMenu menu(this);
	menu.addSection(QStringLiteral("Keyframe %1 at %2").arg(row + 1).arg(timeTextCentis(c->keys[row].tMs)));
	QAction *first = menu.addAction(QStringLiteral("Move to first"));
	first->setEnabled(row > 0);
	first->setToolTip(QStringLiteral("This framing plays first; the times stay where they are."));
	QAction *lastAct = menu.addAction(QStringLiteral("Move to last"));
	lastAct->setEnabled(row < last);
	lastAct->setToolTip(QStringLiteral("This framing plays last; the times stay where they are."));
	menu.addSeparator();
	QAction *reset = menu.addAction(QStringLiteral("Reset value (100%, centred)"));
	reset->setToolTip(QStringLiteral("Frame the whole picture again at this key: full size, centred, upright."));
	menu.addSeparator();
	QAction *del = menu.addAction(QStringLiteral("Delete"));
	del->setShortcut(QKeySequence(Qt::Key_Delete)); // shown, as a reminder; the window's Delete does it
	menu.setToolTipsVisible(true);
	QAction *chosen = menu.exec(globalPos);
	if (chosen == first)
		keyRowAction(row, KeyRowAction::MoveFirst);
	else if (chosen == lastAct)
		keyRowAction(row, KeyRowAction::MoveLast);
	else if (chosen == reset)
		keyRowAction(row, KeyRowAction::Reset);
	else if (chosen == del)
		keyRowAction(row, KeyRowAction::Delete);
}

void VideoEditorWindow::stepKeyframe(int dir)
{
	const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!sel || sel->keys.isEmpty())
		return;
	const qint64 rel = timelinePlayheadMs() - sel->outStartMs;
	// Wrapping and the deadband both live in stepKeyIndex, where they can be
	// checked without a window.
	const int i = stepKeyIndex(sel->keys, rel, dir);
	if (i < 0)
		return;
	onTimelineScrub(sel->outStartMs + sel->keys[i].tMs);
	syncClipInspector();
}

void VideoEditorWindow::openSubtitles()
{
	if (!timelineView_)
		return;
	if (!fullEdit())
		setEditMode(EditMode::Full);
	if (!subtitleDialog_) {
		subtitleDialog_ = new SubtitleDialog(this);
		connect(subtitleDialog_, &SubtitleDialog::captionsReady, this,
			&VideoEditorWindow::addSubtitleClips);
	}
	// The selected media clips (video or audio lane) with the file each one
	// plays. Captions, stills and effects have no speech.
	QVector<SubtitleDialog::Target> targets;
	const TimelineModel &m = timelineView_->model();
	for (const auto &p : timelineView_->selectedPairs()) {
		const TlClip &c = m.tracks[p.first].clips[p.second];
		if (c.type != TlClip::Type::Video)
			continue;
		const EditorSource *s = sourceById(c.sourceId);
		if (!s || s->path.isEmpty())
			continue;
		SubtitleDialog::Target t;
		t.media = c;
		t.path = s->path;
		t.name = s->name.isEmpty() ? QFileInfo(s->path).fileName() : s->name;
		targets.append(t);
	}
	subtitleDialog_->setTargets(targets);
	subtitleDialog_->setPreviewBackground(lastPreviewRaw_, timelineCanvasSize());
	subtitleDialog_->show();
	subtitleDialog_->raise();
	subtitleDialog_->activateWindow();
}

// ---- The menu bar ------------------------------------------------------------
//
// File / Edit / Add / View / Tools / Help, like every application. The items
// that had been buttons on two rows are here now; the rows keep the controls
// used while editing (transport, modes, Snap, Fit, Add). Shortcuts show on
// the right of each item, read from the registry the shortcut panel edits,
// so rebinding a key updates the menu -- and they are DISPLAYED, not bound
// here: the registry's own QShortcuts do the firing, and a second binding on
// the same key would make both ambiguous and dead.

namespace {
QString withShortcut(const QString &text, const QString &keys)
{
	// A tab in a QAction's text is drawn as the shortcut column.
	return keys.isEmpty() ? text : text + QLatin1Char('\t') + keys;
}
} // namespace

void VideoEditorWindow::buildMenuBar(QVBoxLayout *root)
{
	menuBar_ = new QMenuBar(this);
	root->setMenuBar(menuBar_);
	menuShortcutActs_.clear();
	fullOnlyActs_.clear();
	auto keyed = [this](QAction *a, const char *cmdId) {
		menuShortcutActs_.append({a, QLatin1String(cmdId)});
		return a;
	};
	auto fullOnly = [this](QAction *a) {
		fullOnlyActs_.append(a);
		a->setProperty(kWhyDisabledProp,
			       QStringLiteral("Only in Full editing. Press the Full editing button "
					      "at the top of the editor and this entry wakes up."));
		return a;
	};
	auto run = [this](const char *cmdId) {
		return [this, cmdId]() {
			if (const ShortcutCommand *c = shortcuts_->command(QLatin1String(cmdId)))
				if (c->run)
					c->run();
		};
	};

	// ---- File ----
	QMenu *file = menuBar_->addMenu(QStringLiteral("&File"));
	connect(file->addAction(QStringLiteral("Load project…")), &QAction::triggered, this,
		[this]() { openProjBtn_->click(); });
	connect(keyed(file->addAction(QStringLiteral("Save project")), "project.save"), &QAction::triggered, this,
		[this]() { saveProjBtn_->click(); });
	connect(file->addAction(QStringLiteral("Save project as…")), &QAction::triggered, this,
		&VideoEditorWindow::onSaveProjectAs);
	file->addSeparator();
	connect(keyed(file->addAction(QStringLiteral("Export…")), "project.export"), &QAction::triggered, this,
		[this]() { exportBtn_->click(); });
	file->addSeparator();
	connect(file->addAction(QStringLiteral("Close")), &QAction::triggered, this, [this]() { closeBtn_->click(); });

	// ---- Edit ----
	QMenu *edit = menuBar_->addMenu(QStringLiteral("&Edit"));
	connect(keyed(edit->addAction(QStringLiteral("Undo")), "edit.undo"), &QAction::triggered, this, run("edit.undo"));
	connect(keyed(edit->addAction(QStringLiteral("Redo")), "edit.redo"), &QAction::triggered, this, run("edit.redo"));
	edit->addSeparator();
	connect(keyed(fullOnly(edit->addAction(QStringLiteral("Cut clips"))), "edit.cut"), &QAction::triggered, this,
		run("edit.cut"));
	connect(keyed(fullOnly(edit->addAction(QStringLiteral("Copy clips"))), "edit.copy"), &QAction::triggered, this,
		run("edit.copy"));
	connect(keyed(edit->addAction(QStringLiteral("Paste")), "edit.paste"), &QAction::triggered, this, run("edit.paste"));
	connect(keyed(edit->addAction(QStringLiteral("Delete selection")), "edit.delete"), &QAction::triggered, this,
		run("edit.delete"));
	connect(keyed(fullOnly(edit->addAction(QStringLiteral("Select all clips"))), "selection.all"), &QAction::triggered,
		this, run("selection.all"));
	edit->addSeparator();
	connect(keyed(fullOnly(edit->addAction(QStringLiteral("Split clip at playhead"))), "timeline.split"),
		&QAction::triggered, this, run("timeline.split"));
	randomizeAct_ = edit->addAction(QStringLiteral("Randomize clips…"));
	randomizeAct_->setProperty(kWhyDisabledProp,
				   QStringLiteral("Only in Multi-Cut or Full editing: there is nothing to reorder "
						  "in Simple Trim. Press Multi-Cut or Full editing at the top."));
	connect(keyed(randomizeAct_, "timeline.randomize"), &QAction::triggered, this,
		&VideoEditorWindow::openRandomizePanel);
	connect(keyed(fullOnly(edit->addAction(QStringLiteral("Console"))), "view.console"), &QAction::triggered, this,
		&VideoEditorWindow::openConsole);

	// ---- Add: the same menu the Add button shows ----
	if (addMenuBtn_ && addMenuBtn_->menu()) {
		QMenu *add = menuBar_->addMenu(QStringLiteral("&Add"));
		for (QAction *a : addMenuBtn_->menu()->actions())
			add->addAction(a); // shared QActions: one enabled state, one handler
		// Not greyed outside Full editing: every entry switches to Full
		// editing on its own, so from Trim or Multi-Cut "Add > Text" simply
		// takes you there with the clip added. A greyed menu title read as
		// broken.
	}

	// ---- View ----
	QMenu *view = menuBar_->addMenu(QStringLiteral("&View"));
	auto panelToggle = [&](const QString &text, QPushButton *btn, const char *cmdId) {
		QAction *a = view->addAction(text);
		a->setCheckable(true);
		a->setChecked(btn->isChecked());
		if (cmdId)
			keyed(a, cmdId);
		connect(a, &QAction::toggled, btn, &QPushButton::setChecked);
		connect(btn, &QPushButton::toggled, a, &QAction::setChecked);
		return a;
	};
	panelToggle(QStringLiteral("Sources"), sourcesBtn_, nullptr);
	panelToggle(QStringLiteral("Effects"), effectsBtn_, nullptr);
	panelToggle(QStringLiteral("Inspector"), inspectorBtn_, "view.inspector");
	QMenu *safeAreas = view->addMenu(QStringLiteral("Social safe areas"));
	safeAreas->setToolTipsVisible(true);
	auto *safeAreaGroup = new QActionGroup(safeAreas);
	safeAreaGroup->setExclusive(true);
	QSettings viewSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	const int storedGuide = viewSettings.value(QStringLiteral("editor/socialSafeArea"), 0).toInt();
	const int guide = storedGuide >= 0 && storedGuide <= 3 ? storedGuide : 0;
	canvas_->setSocialSafeArea(SocialSafeArea(guide));
	const QStringList safeNames{QStringLiteral("Off"), QStringLiteral("TikTok"),
		QStringLiteral("Instagram Reels"), QStringLiteral("YouTube Shorts")};
	const QStringList safeObjects{QStringLiteral("socialSafeAreaOff"), QStringLiteral("socialSafeAreaTikTok"),
		QStringLiteral("socialSafeAreaReels"), QStringLiteral("socialSafeAreaShorts")};
	for (int i = 0; i < safeNames.size(); ++i) {
		QAction *action = safeAreas->addAction(safeNames[i]);
		action->setObjectName(safeObjects[i]);
		action->setCheckable(true);
		action->setChecked(i == guide);
		action->setToolTip(QStringLiteral("Approximate platform interface margins. Preview guide only; never included in exports."));
		safeAreaGroup->addAction(action);
		connect(action, &QAction::triggered, this, [this, i]() {
			canvas_->setSocialSafeArea(SocialSafeArea(i));
			QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"))
				.setValue(QStringLiteral("editor/socialSafeArea"), i);
		});
	}
	view->addSeparator();
	connect(keyed(fullOnly(view->addAction(QStringLiteral("Zoom timeline to fit"))), "timeline.fit"),
		&QAction::triggered, this, run("timeline.fit"));
	connect(keyed(fullOnly(view->addAction(QStringLiteral("Toggle snapping"))), "timeline.snap"),
		&QAction::triggered, this, run("timeline.snap"));
	view->addSeparator();
	connect(keyed(view->addAction(QStringLiteral("Full screen")), "view.fullscreen"), &QAction::triggered, this,
		[this]() { setFullScreen(!isFullScreen()); });

	// ---- Tools ----
	QMenu *tools = menuBar_->addMenu(QStringLiteral("&Tools"));
	QAction *silence = fullOnly(tools->addAction(QStringLiteral("Trim silence…")));
	silence->setObjectName(QStringLiteral("trimSilenceAction"));
	silence->setToolTip(QStringLiteral("Review long pauses, preview the trimmed result, then ripple-cut every track together as one undo action."));
	connect(silence, &QAction::triggered, this, &VideoEditorWindow::openSilenceTrim);
	QAction *beats = fullOnly(tools->addAction(QStringLiteral("Detect beats…")));
	beats->setObjectName(QStringLiteral("detectBeatsAction"));
	beats->setToolTip(QStringLiteral("Find musical beats in the selected audio or video clip, then add snapping markers."));
	connect(beats, &QAction::triggered, this, [this]() {
		openBeatDetection(timelineView_ ? timelineView_->selectedTrack() : -1,
				  timelineView_ ? timelineView_->selectedClip() : -1);
	});
	tools->addSeparator();
	connect(keyed(tools->addAction(QStringLiteral("Keyboard shortcuts…")), "view.shortcuts"), &QAction::triggered,
		this, &VideoEditorWindow::openShortcutPanel);
	connect(tools->addAction(QStringLiteral("Developer panel")), &QAction::triggered, this, [this]() {
		if (devBtn_)
			devBtn_->click();
	});

	// ---- Help ----
	QMenu *help = menuBar_->addMenu(QStringLiteral("&Help"));
	QAction *logs = help->addAction(QStringLiteral("Error logs…"));
	logs->setToolTip(QStringLiteral("This session's log: recorder and editor errors, warnings and info."));
	connect(logs, &QAction::triggered, this, [this]() {
		// A child window of the editor: the editor runs modally, so a
		// separate top-level window would be blocked behind it.
		if (!errorLogs_) {
			errorLogs_ = new ErrorLogsPanel(this);
			errorLogs_->setWindowFlags(Qt::Window);
			errorLogs_->setAttribute(Qt::WA_DeleteOnClose);
		}
		errorLogs_->show();
		errorLogs_->raise();
		errorLogs_->activateWindow();
		errorLogs_->refresh();
	});
	help->addSeparator();
	connect(help->addAction(QStringLiteral("About Harpia…")), &QAction::triggered, this, &VideoEditorWindow::showAbout);

	refreshMenuLabels();
	for (QAction *a : fullOnlyActs_)
		a->setEnabled(fullEdit());
	if (randomizeAct_)
		randomizeAct_->setEnabled(fullEdit() || multiCut());
	// A greyed entry answers a click with why it is greyed.
	for (QAction *top : menuBar_->actions())
		if (QMenu *m = top->menu())
			explainDisabled(m);
}

void VideoEditorWindow::refreshMenuLabels()
{
	if (!shortcuts_)
		return;
	for (const auto &pr : menuShortcutActs_) {
		QAction *a = pr.first;
		const QString base = a->text().section(QLatin1Char('\t'), 0, 0);
		a->setText(withShortcut(base, shortcuts_->displayText(pr.second)));
	}
}

void VideoEditorWindow::openBeatDetection(int track, int clip)
{
	if (!fullEdit() || !timelineView_ || track < 0 || track >= timelineView_->model().tracks.size() ||
	    clip < 0 || clip >= timelineView_->model().tracks[track].clips.size()) {
		QMessageBox::information(this, QStringLiteral("Detect beats"),
			QStringLiteral("Select a music or video clip on the Full Editing timeline first."));
		return;
	}
	const TlClip target = timelineView_->model().tracks[track].clips[clip];
	if (target.type != TlClip::Type::Video || target.sourceId <= 0 || target.srcLenMs() <= 0) {
		QMessageBox::information(this, QStringLiteral("Detect beats"),
			QStringLiteral("Choose an audio or video clip with a soundtrack to find its beats."));
		return;
	}
	const EditorSource *source = sourceById(target.sourceId);
	const QString path = source ? source->path : QString();
	stopPlayback();
	commitSnapshot();
	const TimelineModel original = timelineView_->model();
	BeatDetectionDialog dialog(path, target, this);
	if (dialog.exec() != QDialog::Accepted)
		return;
	// Analysis uses a snapshot. Timers and queued callbacks still run while a
	// modal window is open, so do not apply stale source positions to a new edit.
	if (timelineView_->model() != original) {
		warnAndLog(this, QStringLiteral("Detect beats"),
			QStringLiteral("The timeline changed during analysis. Detect beats again to align the markers with the current clip."));
		return;
	}
	TimelineModel next = original;
	QMap<qint64, bool> positions;
	for (qint64 at : original.markers)
		positions.insert(at, true);
	int added = 0;
	int number = 0;
	for (qint64 at : dialog.selectedBeatTimes()) {
		++number;
		if (at < 0 || at < target.outStartMs || at >= target.outEndMs())
			continue;
		// Rerunning detection, or a nearby hand-placed section marker, should
		// not produce doubled snap targets or overwrite a marker's name.
		const auto after = positions.lowerBound(at);
		if ((after != positions.end() && after.key() - at <= 20) ||
		    (after != positions.begin() && at - std::prev(after).key() <= 20))
			continue;
		positions.insert(at, true);
		next.markers.append(at);
		next.markerLabels.insert(at, QStringLiteral("Beat %1").arg(number));
		++added;
	}
	if (added > 0) {
		std::sort(next.markers.begin(), next.markers.end());
		timelineView_->setModelAndCommit(next);
	}
	if (infoLabel_)
		infoLabel_->setText(added > 0 ? QStringLiteral("Added %1 beat markers. Undo removes them together.").arg(added)
					     : QStringLiteral("These beats already have nearby markers."));
}

void VideoEditorWindow::openSilenceTrim()
{
	if (!fullEdit() || !timelineView_ || timelineView_->model().isEmpty()) {
		QMessageBox::information(this, QStringLiteral("Trim silence"),
			QStringLiteral("Add video or audio to the Full Editing timeline first."));
		return;
	}
	stopPlayback();
	commitSnapshot();
	const TimelineModel original = timelineView_->model();
	const TlSelection selection = timelineView_->selectionState();
	const qint64 playhead = timelinePlayheadMs();
	bool previewing = false;
	auto restorePreview = [&]() {
		if (!previewing)
			return;
		stopPlayback();
		const bool wasRestoring = restoring_;
		restoring_ = true;
		timelineView_->setModelQuiet(original);
		timelineView_->setSelectionState(selection);
		timelineView_->setPlayhead(playhead);
		restoring_ = wasRestoring;
		previewing = false;
		scriptScanDirty_ = true;
		invalidateAudioMix();
		updateInfoLabel();
		syncPreviewTransformTarget();
		syncClipInspector();
		showTimelineFrame(playhead);
	};
	SilenceTrimDialog dialog(original, [this](int id) {
		const EditorSource *source = sourceById(id);
		return source ? source->path : QString();
	}, timelineView_->selectedPairs(), this);
	connect(&dialog, &SilenceTrimDialog::previewStopped, this, restorePreview);
	connect(&dialog, &SilenceTrimDialog::previewRequested, this, [&](const QVector<TlSpan> &cuts) {
		const auto result = silence_trim::apply(original, cuts);
		if (!result.ok())
			return; // the dialog presents the same validation before enabling Preview
		stopPlayback();
		const bool wasRestoring = restoring_;
		restoring_ = true;
		timelineView_->setModelQuiet(result.model);
		timelineView_->setSelectionState(TlSelection{});
		timelineView_->setPlayhead(0);
		restoring_ = wasRestoring;
		previewing = true;
		scriptScanDirty_ = true;
		invalidateAudioMix();
		updateInfoLabel();
		syncPreviewTransformTarget();
		showTimelineFrame(0);
		onPlayPause();
	});
	// Keep the picture visible while the review window controls its playback.
	dialog.adjustSize();
	dialog.move(mapToGlobal(QPoint(std::max(0, width() - dialog.width() - 16),
		std::max(0, height() - dialog.height() - 24))));
	const int answer = dialog.exec();
	restorePreview();
	if (answer != QDialog::Accepted)
		return;
	const auto result = silence_trim::apply(original, dialog.selectedCuts());
	if (!result.ok()) {
		warnAndLog(this, QStringLiteral("Trim silence"), result.error);
		return;
	}
	if (result.removedMs <= 0)
		return;
	timelineView_->setModelAndCommit(result.model);
	const qint64 at = silence_trim::mapTime(playhead, result.removed);
	timelineView_->setPlayhead(at);
	showTimelineFrame(at);
	syncClipInspector();
}

void VideoEditorWindow::showAbout()
{
	QMessageBox box(this);
	box.setWindowTitle(QStringLiteral("About Harpia"));
	box.setIconPixmap(QPixmap());
	box.setText(QStringLiteral("<b>" HARPIA_APP_DISPLAY_NAME "</b>  v%1").arg(QString::fromUtf8(appVersion())));
	box.setInformativeText(QStringLiteral(
		"Built %1 %2.<br>Screen recorder and editor.<br><br>"
		"Log folder: %3<br>Scripts: %4<br>Templates: %5")
				       .arg(QStringLiteral(__DATE__), QStringLiteral(__TIME__),
					    QDir::toNativeSeparators(QString::fromStdString(Logger::instance().logDir())),
					    QDir::toNativeSeparators(scriptsDir_), QDir::toNativeSeparators(consoleTemplatesDir())));
	QPushButton *logs = box.addButton(QStringLiteral("Open log folder"), QMessageBox::ActionRole);
	QPushButton *scripts = box.addButton(QStringLiteral("Open scripts folder"), QMessageBox::ActionRole);
	QPushButton *templates = box.addButton(QStringLiteral("Open templates folder"), QMessageBox::ActionRole);
	box.addButton(QMessageBox::Close);
	box.exec();
	const auto open = [](const QString &dir) {
		if (!dir.isEmpty())
			QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
	};
	if (box.clickedButton() == logs)
		open(QString::fromStdString(Logger::instance().logDir()));
	else if (box.clickedButton() == scripts)
		open(scriptsDir_);
	else if (box.clickedButton() == templates)
		open(consoleTemplatesDir());
}

void VideoEditorWindow::openUrlDownload()
{
	if (!timelineView_)
		return;
	if (!fullEdit())
		setEditMode(EditMode::Full);
	if (!urlDialog_) {
		urlDialog_ = new UrlDownloadDialog(this);
		// The finished file goes through the same path as a pasted file: a
		// source is made (or reused), the clip lands at the playhead on the
		// first picture lane free for it -- or a new one on top -- and it is
		// one undo step.
		connect(urlDialog_, &UrlDownloadDialog::downloaded, this,
			[this](const QString &path, const YtVideoInfo &) {
				pasteMediaFiles({path}, timelinePlayheadMs());
				commitSnapshot();
			});
	}
	urlDialog_->show();
	urlDialog_->raise();
	urlDialog_->activateWindow();
}

void VideoEditorWindow::addSubtitleClips(const QVector<TlClip> &clips, const QString &summary)
{
	if (deferUntilLoaded([this, clips, summary]() { addSubtitleClips(clips, summary); }))
		return;
	if (!timelineView_ || clips.isEmpty())
		return;
	// One "Subtitles" lane, reused across runs, at the top of the picture
	// stack so the captions draw over everything. Made on a copy and swapped
	// in, so the lane and every caption are ONE undo step.
	TimelineModel m = timelineView_->model();
	int lane = -1;
	for (int i = 0; i < m.tracks.size(); ++i)
		if (m.tracks[i].kind == TlTrack::Kind::Video && m.tracks[i].name == QStringLiteral("Subtitles"))
			lane = i;
	if (lane < 0) {
		TlTrack t;
		t.kind = TlTrack::Kind::Video;
		t.name = QStringLiteral("Subtitles");
		t.color = QColor(0xe0, 0xb3, 0x4a);
		m.tracks.insert(0, t);
		lane = 0;
	}
	for (const TlClip &c : clips)
		m.tracks[lane].clips.append(c);
	timelineView_->setModel(m);
	timelineView_->selectClip(lane, m.tracks[lane].clips.size() - clips.size());
	syncPreviewTransformTarget();
	syncClipInspector();
	showTimelineFrame(timelinePlayheadMs());
	commitSnapshot();
	Q_UNUSED(summary); // the dialog shows it; the lane shows the result
}

void VideoEditorWindow::openTextToSpeech()
{
	if (!timelineView_)
		return;
	if (!fullEdit())
		setEditMode(EditMode::Full);
	if (!ttsDialog_) {
		ttsDialog_ = new TtsDialog(this);
		connect(ttsDialog_, &TtsDialog::speechReady, this, &VideoEditorWindow::addSpeechClips);
	}
	updateTtsFitTarget();
	ttsDialog_->show();
	ttsDialog_->raise();
	ttsDialog_->activateWindow();
}

void VideoEditorWindow::updateTtsFitTarget()
{
	if (!ttsDialog_ || !timelineView_)
		return;
	const TlSpan span = timelineView_->selectedPairs().isEmpty() ? TlSpan() : timelineView_->selectionSpan();
	ttsDialog_->setFitTarget(span.fromMs, span.toMs);
}

namespace {
// A narration's parts in `m`: its audio clip (the one carrying TlClip::tts),
// its captions in time order, and the captions' words in the audio's own
// (source) time -- what sentenceAround() and spliceSentence() work in.
struct NarrationParts {
	int audioTrack = -1, audioClip = -1;
	QVector<QPair<int, int>> captions;
	QVector<NarrationCaption> caps;
};

NarrationParts narrationParts(const TimelineModel &m, int link)
{
	NarrationParts n;
	if (link == 0)
		return n;
	for (const auto &p : m.linkedClips(link)) {
		const TlClip &c = m.tracks[p.first].clips[p.second];
		if (n.audioTrack < 0 && !c.tts.isEmpty()) {
			n.audioTrack = p.first;
			n.audioClip = p.second;
		} else if (c.type == TlClip::Type::Text) {
			n.captions.append(p);
		}
	}
	if (n.audioTrack < 0)
		return n;
	std::sort(n.captions.begin(), n.captions.end(), [&m](const QPair<int, int> &a, const QPair<int, int> &b) {
		return m.tracks[a.first].clips[a.second].outStartMs < m.tracks[b.first].clips[b.second].outStartMs;
	});
	const TlClip &a = m.tracks[n.audioTrack].clips[n.audioClip];
	const double sp = a.speed > 0.01 ? a.speed : 1.0;
	const auto src = [&](qint64 out) { return a.srcStartMs + qint64(std::llround(double(out - a.outStartMs) * sp)); };
	for (const auto &p : n.captions) {
		const TlClip &c = m.tracks[p.first].clips[p.second];
		NarrationCaption nc;
		nc.text = tlPlainText(c.text);
		for (const ClipWordTime &w : c.words) {
			ClipWordTime x = w;
			x.startMs = src(c.outStartMs + w.startMs);
			x.endMs = src(c.outStartMs + w.endMs);
			nc.words.append(x);
		}
		if (nc.words.isEmpty()) { // typed by hand: its own span
			ClipWordTime x;
			x.text = nc.text;
			x.startMs = src(c.outStartMs);
			x.endMs = src(c.outEndMs());
			nc.words.append(x);
		}
		n.caps.append(nc);
	}
	return n;
}
} // namespace

QString VideoEditorWindow::narrationSentenceAt(int track, int clip, qint64 atOutMs, int *link, QString *why) const
{
	const auto say = [why](const QString &w) {
		if (why)
			*why = w;
		return QString();
	};
	if (!timelineView_)
		return say(QStringLiteral("No timeline."));
	const TimelineModel &m = timelineView_->model();
	if (track < 0 || track >= m.tracks.size() || clip < 0 || clip >= m.tracks[track].clips.size())
		return say(QStringLiteral("That clip is gone."));
	const TlClip &c = m.tracks[track].clips[clip];
	const NarrationParts n = narrationParts(m, c.link);
	if (n.audioTrack < 0 || n.caps.isEmpty())
		return say(QStringLiteral("This clip is not part of a text-to-speech narration with captions."));
	int k = -1;
	for (int i = 0; i < n.captions.size(); ++i)
		if (n.captions[i] == QPair<int, int>(track, clip))
			k = i;
	if (k < 0) // the audio: the caption at the time clicked
		for (int i = 0; i < n.captions.size(); ++i)
			if (m.tracks[n.captions[i].first].clips[n.captions[i].second].outStartMs <= atOutMs)
				k = i;
	k = std::max(k, 0);
	const TlClip &a = m.tracks[n.audioTrack].clips[n.audioClip];
	const EditorSource *s = const_cast<VideoEditorWindow *>(this)->sourceById(a.sourceId);
	const qint64 len = s && s->durationMs > 0 ? s->durationMs : a.srcEndMs;
	if (link)
		*link = c.link;
	return sentenceAround(n.caps, k, len).text;
}

void VideoEditorWindow::respeakSentence(int track, int clip, qint64 atOutMs)
{
	int link = 0;
	QString why;
	const QString sentence = narrationSentenceAt(track, clip, atOutMs, &link, &why);
	if (sentence.isEmpty()) {
		QMessageBox::information(this, QStringLiteral("Re-speak"), why);
		return;
	}
	if (respeakEngine_ && respeakEngine_->running()) {
		QMessageBox::information(this, QStringLiteral("Re-speak"),
					 QStringLiteral("A sentence is being spoken again already. Wait for it to land."));
		return;
	}
	const TimelineModel &m = timelineView_->model();
	const NarrationParts n = narrationParts(m, link);
	bool specOk = false;
	const TtsSpec spec = TtsSpec::fromJson(m.tracks[n.audioTrack].clips[n.audioClip].tts, &specOk);
	const QString key = specOk ? TtsEngine::loadKey(spec.provider) : QString();
	if (!specOk || key.isEmpty()) {
		QMessageBox::information(this, QStringLiteral("Re-speak"),
					 QStringLiteral("There is no %1 API key saved on this computer. Open Add > Text to "
							"speech…, paste it, and try again.")
						 .arg(ttsProviderName(spec.provider)));
		return;
	}
	if (!respeakEngine_) {
		respeakEngine_ = new TtsEngine(this);
		connect(respeakEngine_, &TtsEngine::progress, this, [this](const QString &s) {
			if (infoLabel_)
				infoLabel_->setText(QStringLiteral("Re-speaking: %1").arg(s));
		});
		connect(respeakEngine_, &TtsEngine::failed, this, [this](const QString &w) {
			warnAndLog(this, QStringLiteral("Re-speak"), QStringLiteral("The sentence could not be spoken again: %1").arg(w));
		});
		connect(respeakEngine_, &TtsEngine::finished, this, &VideoEditorWindow::finishRespeak);
	}
	respeakLink_ = link;
	respeakText_ = sentence;
	TtsJob job;
	job.text = sentence;
	job.spec = spec; // its tempo too: the same pace as the rest of the narration
	job.apiKey = key;
	job.rules = TtsEngine::pronunciations();
	job.outPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
			      .filePath(QStringLiteral("harpia-respoken-%1.wav").arg(QDateTime::currentMSecsSinceEpoch()));
	if (infoLabel_)
		infoLabel_->setText(QStringLiteral("Re-speaking “%1”…").arg(sentence));
	respeakEngine_->start(job);
}

void VideoEditorWindow::finishRespeak(const TtsResult &result)
{
	if (loadingDialog_) {
		if (loadingProject_) {
			QFile::remove(result.wavPath);
		} else {
			const quint64 generation = projectLoadGeneration_;
			afterLoading_.append([this, generation, result]() {
				if (generation == projectLoadGeneration_)
					finishRespeak(result);
				else
					QFile::remove(result.wavPath);
			});
		}
		return;
	}
	QString why;
	if (!applyRespoken(respeakLink_, respeakText_, result.wavPath, result.words, &why))
		warnAndLog(this, QStringLiteral("Re-speak"), why);
	QFile::remove(result.wavPath);
}

bool VideoEditorWindow::applyRespoken(int link, const QString &sentence, const QString &takeWav,
				      const QVector<ClipWordTime> &takeWords, QString *why)
{
	const auto fail = [why](const QString &w) {
		if (why)
			*why = w;
		return false;
	};
	if (!timelineView_)
		return fail(QStringLiteral("No timeline."));
	TimelineModel m = timelineView_->model();
	const NarrationParts n = narrationParts(m, link);
	if (n.audioTrack < 0 || n.caps.isEmpty())
		return fail(QStringLiteral("The narration is no longer on the timeline."));
	const TlClip oldAudio = m.tracks[n.audioTrack].clips[n.audioClip];
	const EditorSource *src = sourceById(oldAudio.sourceId);
	if (!src)
		return fail(QStringLiteral("The narration's audio is gone."));
	const qint64 len = src->durationMs > 0 ? src->durationMs : oldAudio.srcEndMs;
	// The sentence as it was asked for: if the captions changed meanwhile,
	// this take no longer belongs anywhere.
	SentenceSpan span;
	for (int k = 0; k < n.caps.size(); ++k) {
		const SentenceSpan s = sentenceAround(n.caps, k, len);
		if (s.text == sentence) {
			span = s;
			break;
		}
		k = s.last;
	}
	if (!span.valid())
		return fail(QStringLiteral("The sentence changed on the timeline while it was being spoken again. Try again."));

	// The audio: the narration's own file (the session copy if that is gone),
	// the new take spliced in, saved beside it under a new name -- the old
	// file stays, so Ctrl+Z has it.
	const QString oldPath = !src->origPath.isEmpty() && QFileInfo::exists(src->origPath) ? src->origPath : src->path;
	tts_audio::Pcm old, take;
	if (!tts_audio::readWav16(oldPath, old) || !tts_audio::readWav16(takeWav, take))
		return fail(QStringLiteral("The narration's audio could not be read."));
	const Spliced spl = spliceSentence(old, span, take, takeWords);
	if (!spl.ok)
		return fail(QStringLiteral("The new take could not be put in place."));
	const QFileInfo oi(oldPath);
	QString base = oi.completeBaseName();
	base.remove(QRegularExpression(QStringLiteral(" \\(re-spoken \\d+\\)$")));
	QString newPath;
	for (int i = 1; newPath.isEmpty() || QFileInfo::exists(newPath); ++i)
		newPath = oi.dir().filePath(QStringLiteral("%1 (re-spoken %2).wav").arg(base).arg(i));
	if (!tts_audio::writeWav16(newPath, spl.pcm))
		return fail(QStringLiteral("Could not save the new narration beside the old one (%1).")
				    .arg(QDir::toNativeSeparators(oi.absolutePath())));
	const int newId = addAudioSource(newPath);
	if (newId < 0)
		return fail(QStringLiteral("The new narration could not be read back."));

	// The timeline, on a copy: one undo step.
	const double sp = oldAudio.speed > 0.01 ? oldAudio.speed : 1.0;
	const auto outOf = [&](qint64 srcMs) {
		return oldAudio.outStartMs + qint64(std::llround(double(srcMs - oldAudio.srcStartMs) / sp));
	};
	const qint64 sentenceEndOut = outOf(span.cutToMs);
	const qint64 deltaOut = qint64(std::llround(double(spl.deltaMs) / sp));
	const QPair<int, int> tmplAt = n.captions[span.first];
	const TlClip tmpl = m.tracks[tmplAt.first].clips[tmplAt.second];

	TlClip &audio = m.tracks[n.audioTrack].clips[n.audioClip];
	audio.sourceId = newId;
	if (audio.srcEndMs >= span.cutToMs)
		audio.srcEndMs += spl.deltaMs;
	audio.srcEndMs = std::min(audio.srcEndMs, spl.pcm.durationMs());
	if (const EditorSource *ns = sourceById(newId))
		audio.peaks = VoiceoverTrack::loadPeaks(ns->path, 600);

	// Out with the sentence's old captions (from the back, so the indices
	// still mean what they meant)...
	QVector<QPair<int, int>> gone;
	for (int i = span.first; i <= span.last; ++i)
		gone.append(n.captions[i]);
	std::sort(gone.begin(), gone.end(), [](const QPair<int, int> &a, const QPair<int, int> &b) { return a > b; });
	for (const auto &p : gone)
		m.tracks[p.first].clips.remove(p.second);
	// ...the rest of the narration moves by the difference...
	for (TlTrack &t : m.tracks)
		for (TlClip &c : t.clips)
			if (c.link == link && c.tts.isEmpty() && c.outStartMs >= sentenceEndOut)
				c.outStartMs += deltaOut;
	// ...and in with new ones, in the old ones' style and place.
	SubtitleLook look;
	look.style = tmpl.text;
	const QVector<TlClip> fresh =
		buildSubtitleClips(groupWords(spl.words, TtsEngine::captionRule()), m.tracks[n.audioTrack].clips[n.audioClip], look);
	for (TlClip c : fresh) {
		const QString words = c.text.text;
		c.text = tmpl.text;
		c.text.text = words;
		c.posX = tmpl.posX;
		c.posY = tmpl.posY;
		c.components = tmpl.components;
		c.tags = tmpl.tags;
		c.link = link;
		m.tracks[tmplAt.first].clips.append(c);
	}
	timelineView_->setModel(m);
	TlSelection sel;
	const auto all = m.linkedClips(link);
	if (!all.isEmpty()) {
		sel.track = all.first().first;
		sel.clip = all.first().second;
		for (int i = 1; i < all.size(); ++i)
			sel.extras.append(all[i]);
		std::sort(sel.extras.begin(), sel.extras.end());
	}
	timelineView_->setSelectionState(sel);
	syncPreviewTransformTarget();
	syncClipInspector();
	showTimelineFrame(timelinePlayheadMs());
	commitSnapshot();
	if (infoLabel_)
		infoLabel_->setText(QStringLiteral("Spoken again: “%1” (%2%3 s).")
					    .arg(sentence)
					    .arg(spl.deltaMs >= 0 ? QStringLiteral("+") : QString())
					    .arg(spl.deltaMs / 1000.0, 0, 'f', 1));
	return true;
}

void VideoEditorWindow::addSpeechClips(const QString &wavPath, qint64 durationMs, const QVector<ClipWordTime> &words,
				       const GroupRule &rule, const SubtitleLook &look, const QString &specJson, qint64 atMs)
{
	if (deferUntilLoaded([this, wavPath, durationMs, words, rule, look, specJson, atMs]() {
		addSpeechClips(wavPath, durationMs, words, rule, look, specJson, atMs);
	}))
		return;
	if (!timelineView_)
		return;
	if (!fullEdit())
		setEditMode(EditMode::Full);
	// A source like any imported audio file: the project keeps the narration's
	// path (in Movies/Harpia/Speech), the session plays its decoded copy.
	const int src = addAudioSource(wavPath);
	if (src < 0)
		return; // addAudioSource said why
	const EditorSource *s = sourceById(src);
	const qint64 dur = s && s->durationMs > 0 ? s->durationMs : durationMs;
	if (dur <= 0)
		return;
	TlClip audio;
	audio.type = TlClip::Type::Video; // "media clip"; the lane makes it audio
	audio.sourceId = src;
	audio.srcStartMs = 0;
	audio.srcEndMs = dur;
	audio.outStartMs = atMs >= 0 ? atMs : timelinePlayheadMs();
	audio.peaks = VoiceoverTrack::loadPeaks(s ? s->path : wavPath, 600);
	audio.tts = specJson; // how it was made: "Re-speak this sentence" needs it
	// The captions, timed against that clip exactly as subtitles from speech
	// are against theirs.
	const QVector<TlClip> captions = buildSubtitleClips(groupWords(words, rule), audio, look);

	// Placed on a copy and swapped in: the lanes, the narration and every
	// caption are ONE undo step, linked as one group.
	TimelineModel m = timelineView_->model();
	const SpeechPlacement placed = placeSpeech(m, audio, captions);
	timelineView_->setModel(m);
	TlSelection sel;
	sel.track = placed.audioTrack;
	sel.clip = placed.audioClip;
	for (const auto &p : placed.all())
		if (p != QPair<int, int>(placed.audioTrack, placed.audioClip))
			sel.extras.append(p);
	std::sort(sel.extras.begin(), sel.extras.end());
	timelineView_->setSelectionState(sel);
	syncPreviewTransformTarget();
	syncClipInspector();
	showTimelineFrame(timelinePlayheadMs());
	commitSnapshot();
	if (infoLabel_)
		infoLabel_->setText(QStringLiteral("Narration added: %1, %2 captions, linked.")
					    .arg(timeTextTenths(dur))
					    .arg(captions.size()));
}

void VideoEditorWindow::addTextClip()
{
	if (!timelineView_)
		return;
	if (!fullEdit())
		setEditMode(EditMode::Full);
	TlClip c;
	c.type = TlClip::Type::Text;
	c.srcStartMs = 0;
	c.srcEndMs = 4000; // a 4s caption by default
	c.outStartMs = timelinePlayheadMs();
	c.text.text = QStringLiteral("Your text");
	// Never over a clip that is already there: a free lane, or a new one.
	timelineView_->addClipOnFreeLane(TlTrack::Kind::Video, c);
	syncPreviewTransformTarget();
	syncClipInspector();
	showTimelineFrame(timelinePlayheadMs());
}

qint64 VideoEditorWindow::timelinePlayheadMs() const
{
	if (!timelineView_)
		return 0;
	return std::max<qint64>(0, timelineView_->playhead());
}

// The time an edit to the selected clip should be seen at.
//
// Edits land at the playhead, but the playhead need not be inside the clip
// being edited — it starts at 0, and hovering previews a time without moving
// it. Rendering the raw playhead then showed a moment the clip does not cover,
// i.e. a black frame, while the edit itself was clamped into the clip. Clamping
// both to the same instant means you always see the clip you are changing.
qint64 VideoEditorWindow::timelineEditMs() const
{
	const qint64 ph = timelinePlayheadMs();
	const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!sel)
		return ph;
	return std::clamp<qint64>(ph, sel->outStartMs, std::max(sel->outStartMs, sel->outEndMs() - 1));
}

void VideoEditorWindow::addZoomAtPreviewPoint(double canvasXNorm, double canvasYNorm)
{
	if (!fullEdit() || !timelineView_)
		return;
	const TlClip *sel = timelineView_->selectedClipPtr();
	if (!sel)
		return;
	const QSize natural = clipNaturalSize(*sel);
	if (natural.isEmpty())
		return;
	if (playing_)
		stopPlayback(); // the zoom is anchored to a still playhead, as the drags are

	const qint64 at = timelineEditMs();
	const QSize canvasSize = timelineCanvasSize();
	// The click is a point on the CANVAS; the generator wants one in the clip.
	// They differ the moment the clip is already zoomed, which is exactly when
	// someone reaches for this a second time.
	const QPointF inClip = clipPointFromCanvas(sel->transformAt(at), canvasSize, natural,
						   QPointF(canvasXNorm, canvasYNorm));

	TlClip c = *sel;
	if (!addZoomAt(c, at, inClip, canvasSize, natural, zoomSettings_)) {
		infoLabel_->setText(
			QStringLiteral("This clip is too short for a zoom that can arrive and leave."));
		return;
	}
	// One call: updateSelectedClip emits clipsChanged, which re-renders the
	// preview, re-reads the Inspector, and schedules the undo snapshot. Doing
	// any of that again here would just be a second, later copy of it.
	timelineView_->updateSelectedClip(c);
	refreshKeyframeEditor(); // the keys are new; the editor lists them
	// After the update, because the clipsChanged handler rewrites this label.
	infoLabel_->setText(
		QStringLiteral("Zoom added at %1 — four keyframes, so you can drag or delete it.")
			.arg(timeTextCentis(at)));
}

QSize VideoEditorWindow::clipNaturalSize(const TlClip &c)
{
	// The clip's own pixel size, which is what every canvas-geometry question
	// is measured against. Shared rather than re-derived: the transform grips
	// and "Zoom here" have to agree about where the picture is, and two copies
	// of this would eventually not.
	const QSize canvasSize = timelineCanvasSize();
	if (c.type == TlClip::Type::Text)
		return TimelineCompositor::textNaturalSize(c.text, canvasSize);
	if (const QSize s = stillSize(c.sourceId); !s.isEmpty())
		return (!c.crop.isNull() && c.crop.width() > 1) ? c.crop.size() : s;
	if (EditorSource *s = sourceById(c.sourceId))
		return (!c.crop.isNull() && c.crop.width() > 1) ? c.crop.size()
							       : QSize(s->width, s->height);
	return QSize();
}

void VideoEditorWindow::syncPreviewTransformTarget(bool refreshPath)
{
	if (!canvas_ || !timelineView_)
		return;
	const TlClip *c = fullEdit() ? timelineView_->selectedClipPtr() : nullptr;
	canvas_->setTransformMode(c != nullptr);
	if (!c) {
		canvas_->setTransformBox(QRectF(), 0.0);
		canvas_->setMaskEdit(PreviewCanvas::MaskEdit{});
		if (refreshPath)
			refreshMotionPath();
		return;
	}
	// Outline the clip where it currently sits on the canvas.
	const QSize canvasSize = timelineCanvasSize();
	const qint64 ph = timelinePlayheadMs();
	const TlTransform tf = c->transformAt(ph);
	const QSize natural = clipNaturalSize(*c);
	// The rotation goes across separately: clipRectOnCanvas returns the box
	// BEFORE it is turned, and handing the canvas a bounding box instead would
	// put the grips off the corners of anything tilted.
	canvas_->setTransformBox(natural.isEmpty() ? QRectF()
						   : TimelineCompositor::clipRectOnCanvas(
							     tf, canvasSize, natural),
				 tf.rotation);

	// A Mask component on this clip becomes draggable on the picture. The FIRST
	// enabled one: with two masks there is no single shape to put grips on, and
	// silently editing one of them would be worse than editing the obvious one.
	PreviewCanvas::MaskEdit me;
	const ComponentType *maskType =
		ComponentRegistry::instance().find(QStringLiteral("harpia.mask"));
	if (maskType && !natural.isEmpty()) {
		for (const ComponentInstance &ci : c->components) {
			if (ci.typeId != QStringLiteral("harpia.mask") || !ci.enabled)
				continue;
			// Through propAt, so a keyframed mask puts its grips where the
			// shape actually IS at the playhead rather than at its resting
			// pose -- the same trap the spotlight masks already avoid.
			const qint64 tMs = ph - c->outStartMs;
			const auto f = [&](const char *k, double d) {
				const QVariant v = propAt(*maskType, ci, QString::fromLatin1(k), tMs);
				return v.isValid() ? v.toDouble() : d;
			};
			me.on = true;
			me.shape = spotShapeFromInt(int(std::lround(f("shape", 1.0))));
			me.cx = f("centreX", 0.5);
			me.cy = f("centreY", 0.5);
			me.w = f("width", 0.5);
			me.h = f("height", 0.5);
			me.rotation = f("rotation", 0.0);
			me.corner = f("corner", 0.15);
			break;
		}
	}
	canvas_->setMaskEdit(me);
	if (refreshPath)
		refreshMotionPath();
}

void VideoEditorWindow::refreshMotionPath()
{
	if (!canvas_)
		return;
	const TlClip *c = (fullEdit() && timelineView_) ? timelineView_->selectedClipPtr() : nullptr;
	PreviewCanvas::PathDraw pd;
	if (c) {
		const MotionPath mp = buildMotionPath(*c, timelineFps());
		if (mp.drawable()) {
			pd.on = true;
			const qint64 at = timelineEditMs() - c->outStartMs;
			for (int i = 0; i < mp.keys.size(); ++i) {
				pd.keys.append(mp.keys[i].pos);
				pd.keyIds.append(mp.keys[i].keyIndex);
				if (std::llabs(mp.keys[i].tMs - at) <= 40)
					pd.current = i;
			}
			pd.frames = mp.frames;
			pd.line = mp.line;
			if (pd.current >= 0) {
				const PathHandles h = pathHandlesFor(*c, pd.keyIds[pd.current]);
				pd.showIn = h.in;
				pd.showOut = h.out;
				pd.inHandle = h.inPos;
				pd.outHandle = h.outPos;
			}
		}
	}
	// A picked key stays picked (for Delete) only while the playhead is on it:
	// scrubbing away, selecting something else or removing it lets go.
	if (pathKeyPicked_ >= 0 && (!pd.on || pd.current < 0 || pd.keyIds[pd.current] != pathKeyPicked_))
		pathKeyPicked_ = -1;
	canvas_->setMotionPath(pd);
}

void VideoEditorWindow::deleteMotionPathKey(int keyId)
{
	const TlClip *c = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!c || keyId < 0 || keyId >= c->keys.size())
		return;
	if (playing_)
		stopPlayback();
	// Only the position part of the key: a key that also pins scale or
	// rotation keeps doing that. clearKeyLane drops the key once it pins
	// nothing at all.
	editSelectedClip([keyId](TlClip &cl) {
		if (keyId < 0 || keyId >= cl.keys.size())
			return;
		cl.clearKeyLane(keyId, TlLanePos);
		if (cl.keys.size() == 1) { // a single key is just a static pose
			cl.setBaseTransform(cl.keys.front().tf);
			cl.keys.clear();
		}
	});
	pathKeyPicked_ = -1;
	commitSnapshot();
}

void VideoEditorWindow::motionPathKeyMenu(int keyId, const QPoint &globalPos)
{
	const TlClip *c = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!c || keyId < 0 || keyId >= c->keys.size())
		return;
	const qint64 at = c->outStartMs + c->keys[keyId].tMs;
	const TlEase cur = c->keys[keyId].channel(TlLanePos).ease;
	QMenu menu(this);
	menu.addSection(QStringLiteral("Position key at %1").arg(timeTextCentis(c->keys[keyId].tMs)));
	QAction *go = menu.addAction(QStringLiteral("Go to this keyframe"));
	QMenu *easeMenu = menu.addMenu(QStringLiteral("Easing to the next key"));
	easeMenu->setToolTip(QStringLiteral("How the clip speeds up and slows down between this key and the next. "
					    "It changes the speed, never the path; the dots show the speed."));
	QVector<QAction *> easeActs;
	for (int i = 0; i < kTlEaseCount; ++i) {
		const TlEase e = TlEase(i);
		if (e == TlEase::Bezier && cur != TlEase::Bezier)
			continue; // custom curves are made in the keyframe editor
		QAction *a = easeMenu->addAction(QString::fromLatin1(tlEaseName(e)));
		a->setCheckable(true);
		a->setChecked(e == cur);
		a->setData(i);
		easeActs.append(a);
	}
	// The path's shape, which the ease never changes.
	const TlKeyframe &key = c->keys[keyId];
	QAction *curved = menu.addAction(QStringLiteral("Curved path to the next key"));
	curved->setCheckable(true);
	curved->setChecked(key.curvedPath);
	curved->setToolTip(QStringLiteral("Travel to the next position key along a curve instead of a straight "
					  "line. Drag the round handles on the preview to bend it (Alt-drag moves one "
					  "handle alone)."));
	QAction *resetHandles = menu.addAction(QStringLiteral("Smooth handles (reset)"));
	resetHandles->setEnabled(key.handlesManual);
	resetHandles->setToolTip(QStringLiteral("Put this key's handles back to the automatic smooth curve"));
	QAction *curveAll = menu.addAction(QStringLiteral("Curve the whole path"));
	QAction *straightAll = menu.addAction(QStringLiteral("Straighten the whole path"));
	menu.addSeparator();
	QAction *del = menu.addAction(QStringLiteral("Delete keyframe"));
	del->setShortcut(QKeySequence(Qt::Key_Delete));
	QAction *chosen = menu.exec(globalPos);
	if (!chosen)
		return;
	if (chosen == go) {
		if (playing_)
			stopPlayback();
		pathKeyPicked_ = keyId;
		onTimelineScrub(at);
		syncClipInspector();
	} else if (chosen == del) {
		deleteMotionPathKey(keyId);
	} else if (easeActs.contains(chosen)) {
		const TlEase e = TlEase(chosen->data().toInt());
		editSelectedClip([keyId, e](TlClip &cl) {
			if (keyId >= 0 && keyId < cl.keys.size())
				cl.keys[keyId].channel(TlLanePos).ease = e;
		});
		commitSnapshot();
	} else if (chosen == curved) {
		const bool on = curved->isChecked();
		editSelectedClip([keyId, on](TlClip &cl) {
			if (keyId >= 0 && keyId < cl.keys.size())
				cl.keys[keyId].curvedPath = on;
		});
		commitSnapshot();
	} else if (chosen == resetHandles) {
		editSelectedClip([keyId](TlClip &cl) {
			if (keyId >= 0 && keyId < cl.keys.size()) {
				cl.keys[keyId].handlesManual = false;
				cl.keys[keyId].handlesBroken = false;
			}
		});
		commitSnapshot();
	} else if (chosen == curveAll || chosen == straightAll) {
		setSelectedClipPathCurved(chosen == curveAll);
	}
}

// Every position key of the selected clip curved (smooth, automatic handles
// unless they were dragged) or straight again. One undo step.
void VideoEditorWindow::setSelectedClipPathCurved(bool curved)
{
	editSelectedClip([curved](TlClip &cl) {
		for (TlKeyframe &k : cl.keys)
			if (k.channel(TlLanePos).on)
				k.curvedPath = curved;
	});
	commitSnapshot();
	refreshMotionPath();
}

// Snap the selected clip's rect to the canvas. `how` is the Align menu's
// row: 0 centre, 1/2 centre on one axis, 3-6 an edge, 7 fit, 8 fill, 9-12
// resets. Everything is worked out from where the rect IS at the playhead
// (clipRectOnCanvas), so it holds for a scaled or keyed clip too.
void VideoEditorWindow::alignSelectedClip(int how)
{
	if (!timelineView_)
		return;
	const TlClip *sel = timelineView_->selectedClipPtr();
	if (!sel)
		return;
	if (playing_)
		stopPlayback();
	const QSize canvas = timelineCanvasSize();
	if (canvas.width() <= 0 || canvas.height() <= 0)
		return;
	const QSize natural = clipNaturalSize(*sel);
	TlTransform tf = sel->transformAt(timelinePlayheadMs());
	const double cw = canvas.width(), ch = canvas.height();
	auto rectFor = [&](const TlTransform &t) {
		return TimelineCompositor::clipRectOnCanvas(t, canvas, natural);
	};
	switch (how) {
	case 0: tf.posX = 0.5; tf.posY = 0.5; break;
	case 1: tf.posX = 0.5; break;
	case 2: tf.posY = 0.5; break;
	case 3: tf.posX = (rectFor(tf).width() / 2.0) / cw; break;
	case 4: tf.posX = (cw - rectFor(tf).width() / 2.0) / cw; break;
	case 5: tf.posY = (rectFor(tf).height() / 2.0) / ch; break;
	case 6: tf.posY = (ch - rectFor(tf).height() / 2.0) / ch; break;
	case 7: // scale 1 IS "fits inside the canvas" (clipRectOnCanvas fits the
		// source by its longer side); centred so the fit is visible.
		tf.scale = 1.0; tf.posX = 0.5; tf.posY = 0.5; tf.rotation = 0.0; break;
	case 8: { // the shorter side reaches the canvas: no bars, edges cropped
		if (natural.width() > 0 && natural.height() > 0) {
			const double fit = std::min(cw / natural.width(), ch / natural.height());
			const double fill = std::max(cw / natural.width(), ch / natural.height());
			tf.scale = std::clamp(fill / fit, 0.05, 20.0);
		}
		tf.posX = 0.5; tf.posY = 0.5; tf.rotation = 0.0;
		break;
	}
	case 9: tf.posX = 0.5; tf.posY = 0.5; break;
	case 10: tf.scale = 1.0; break;
	case 11: tf.rotation = 0.0; break;
	case 12: tf.posX = 0.5; tf.posY = 0.5; tf.scale = 1.0; tf.rotation = 0.0; break;
	default: return;
	}
	applySelectedClipTransform(tf);
	commitSnapshot(); // a menu pick is a finished action, not a drag in progress
}

void VideoEditorWindow::applySelectedClipTransform(const TlTransform &tf)
{
	if (!timelineView_)
		return;
	const TlClip *sel = timelineView_->selectedClipPtr();
	if (!sel)
		return;
	// Keyframed clips (or auto-key) record the pose at `at` so the zoom
	// animates; otherwise the clip's static pose moves.
	const auto pose = [this](TlClip &c, qint64 at, const TlTransform &t) {
		if (autoKeyframe_ || !c.keys.isEmpty()) {
			if (c.keys.isEmpty()) {
				// Seed the animation with the current static pose at the clip start so
				// the first recorded key doesn't snap the whole clip.
				TlKeyframe seed;
				seed.tMs = 0;
				seed.tf = c.baseTransform();
				c.keys.append(seed);
			}
			c.setKeyframeAt(at, t);
		} else {
			c.setBaseTransform(t);
		}
	};
	TlClip c = *sel;
	pose(c, timelineEditMs(), tf);
	const QVector<QPair<int, int>> selection = timelineView_->selectedPairs();
	if (selection.size() < 2) {
		timelineView_->updateSelectedClip(c);
	} else {
		// All selected clips move, scale and turn together. Each
		// takes the same CHANGE the clicked one did -- the same offset, the
		// same factor, the same turn -- so the group keeps its layout rather
		// than piling onto one spot.
		const TlTransform was = sel->transformAt(timelinePlayheadMs());
		const double dx = tf.posX - was.posX, dy = tf.posY - was.posY;
		const double k = tf.scale / std::max(0.0001, was.scale);
		const double turn = tf.rotation - was.rotation;
		const QPair<int, int> clicked(timelineView_->selectedTrack(), timelineView_->selectedClip());
		const qint64 ph = timelinePlayheadMs();
		timelineView_->editClipsAt(selection, [&](const QPair<int, int> &at, TlClip &o) {
			if (at == clicked) {
				o = c;
				return;
			}
			// Its own pose where it is: at the playhead when that is over
			// it, else at its nearest end.
			const qint64 when = std::clamp<qint64>(ph, o.outStartMs, std::max(o.outStartMs, o.outEndMs() - 1));
			TlTransform t = o.transformAt(when);
			t.posX += dx;
			t.posY += dy;
			t.scale = std::clamp(t.scale * k, 0.05, 20.0);
			t.rotation += turn;
			pose(o, when, t);
		});
	}
	syncPreviewTransformTarget();
	const qint64 at = timelineEditMs();
	timelineView_->setPlayhead(at);
	showTimelineFrame(at);
	// Dragging in the preview is an edit like any other, so the Inspector's
	// position/zoom/rotation must track the mouse rather than going stale until
	// the next reselect.
	syncClipInspector();
	scheduleSnapshot();
}

namespace {
// How close to the middle a drag has to get before it is pulled onto it, as a
// fraction of the canvas. About 1% -- roughly 19 canvas px at 1080p, and a
// handful of screen pixels at any preview size, which is close enough to be
// deliberate and far enough to be reachable.
constexpr double kCentreSnapNorm = 0.01;
} // namespace

void VideoEditorWindow::onPreviewTransformDrag(double dxNorm, double dyNorm)
{
	const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!sel)
		return;
	// Framing a clip while the playhead runs would apply each delta against a
	// different interpolated pose (and scatter keyframes across the timeline), so
	// the clip appears not to follow the mouse. Pause first, like scrubbing does.
	if (playing_)
		stopPlayback();
	TlTransform tf = sel->transformAt(timelinePlayheadMs());
	tf.posX += dxNorm;
	tf.posY += dyNorm;
	// Snap does for the picture what it already does for clip edges on the
	// timeline: dragging something to EXACTLY centred by hand is a game of
	// one-pixel corrections you lose, and 0.4997 reads as centred while not
	// being it. Per axis, so sliding down the middle keeps its horizontal
	// centring instead of needing both held at once.
	bool snappedX = false, snappedY = false;
	if (timelineView_ && timelineView_->snapEnabled())
		tf = snapPoseToCentre(tf, kCentreSnapNorm, &snappedX, &snappedY);
	if (canvas_)
		canvas_->setCentreGuides(snappedX, snappedY);
	applySelectedClipTransform(tf);
}

void VideoEditorWindow::onPreviewTransformZoom(double factor, double cursorXNorm, double cursorYNorm)
{
	const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
	if (!sel)
		return;
	if (playing_) // see onPreviewTransformDrag: reframe against a still playhead
		stopPlayback();
	TlTransform tf = sel->transformAt(timelinePlayheadMs());
	const double newScale = std::clamp(tf.scale * factor, 0.05, 20.0);
	const double k = newScale / std::max(0.0001, tf.scale); // actual applied factor
	// Hold the point under the cursor fixed: c' = cursor - k * (cursor - c).
	tf.posX = cursorXNorm - k * (cursorXNorm - tf.posX);
	tf.posY = cursorYNorm - k * (cursorYNorm - tf.posY);
	tf.scale = newScale;
	applySelectedClipTransform(tf);
}

// Dropped anywhere in the window EXCEPT the lanes -- the timeline handles its
// own drops, and gets them first because it is the widget under the pointer.
// Here there is no lane to aim at, so a file joins the media pool and waits to
// be used, which is what dropping on the background has always meant.
void VideoEditorWindow::dragEnterEvent(QDragEnterEvent *e)
{
	if (!e->mimeData()->hasUrls())
		return;
	for (const QUrl &u : e->mimeData()->urls())
		if (isMediaFile(u.toLocalFile())) {
			e->acceptProposedAction();
			return;
		}
}

void VideoEditorWindow::dropEvent(QDropEvent *e)
{
	int firstVideo = -1;
	bool addedAny = false;
	const QList<QUrl> urls = e->mimeData()->urls();
	LoadingScope loading(*this, QStringLiteral("Adding media"));
	QScopedValueRollback<int> totalLoading(loadingTotal_, int(urls.size()));
	QScopedValueRollback<int> completedLoading(loadingCompleted_, 0);
	for (int fileIndex = 0; fileIndex < urls.size(); ++fileIndex) {
		loadingCompleted_ = fileIndex;
		const QUrl &u = urls[fileIndex];
		const QString f = u.toLocalFile();
		if (f.isEmpty())
			continue;
		// Images used to be ignored here, so dropping a PNG on the editor did
		// nothing at all and gave no reason why.
		if (isVideoFile(f)) {
			const int id = addSource(f);
			if (id < 0)
				continue;
			addedAny = true;
			if (firstVideo < 0)
				firstVideo = id;
		} else if (isImageFile(f)) {
			addedAny |= addImageSource(f) >= 0;
		} else if (isAudioFile(f)) {
			// A music file dropped on the editor rather than on a lane: added
			// to the pool, ready for "Add audio", instead of ignored.
			addedAny |= addAudioSource(f) >= 0;
		}
	}
	if (!addedAny)
		return;
	// Only a video can be the ACTIVE source -- that is what the preview scrubs
	// and what Trim and Multi-Cut cut from. A still has nothing to scrub.
	if (firstVideo >= 0)
		setActiveSource(firstVideo);
	e->acceptProposedAction();
}

void VideoEditorWindow::showSourcesPanel()
{
	if (!sourcesPanel_)
		return;
	if (!sourcesPlaced_) {
		QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		const QByteArray geom = st.value(QStringLiteral("editor/sourcesGeom")).toByteArray();
		if (!geom.isEmpty())
			sourcesPanel_->restoreGeometry(geom);
		else
			sourcesPanel_->move(geometry().left() + 16, geometry().top() + 76);
		// A geometry saved by an older build is the old narrow one; open it
		// at the new width once rather than carrying the cramp forward.
		if (sourcesPanel_->width() < 480)
			sourcesPanel_->resize(560, std::max(sourcesPanel_->height(), 620));
		sourcesPlaced_ = true;
	}
	sourcesPanel_->show();
	sourcesPanel_->raise();
}

void VideoEditorWindow::showEvent(QShowEvent *e)
{
	QDialog::showEvent(e);
	if (projectIsEmpty())
		refreshRecentProjects();
	// The Sources panel stays closed until the user opens it with the toolbar
	// "Sources" button — it never opens on its own.

	// The editor opens maximised: shown at its restore size (centred over the
	// main window by QDialog), then maximised from the next turn of the event
	// loop, once the window is really on screen. A maximised state set before
	// that is the one Windows got wrong (see the constructor). Maximised, not
	// full screen: full screen was tried (v0.1.311) and on Windows an owned
	// dialog handed a screen-sized geometry came up LARGER than the screen,
	// its maximise button out of reach; F11 still goes full screen. Only on
	// the first show: a later show (after a modal child, say) must not yank a
	// window the user has resized back to maximised.
	if (!shownOnce_) {
		shownOnce_ = true;
		window_screen::restore(*this, QStringLiteral("editor"));
		QTimer::singleShot(0, this, [this]() {
			if (!isMaximized() && !isFullScreen())
				showMaximized();
		});
	}

	// First real geometry: now the split can be sized to the starting mode.
	QTimer::singleShot(0, this, [this]() { applyModeSplit(); });
}

void VideoEditorWindow::setFullScreen(bool on)
{
	if (on) {
		// showFullScreen, not a bare window-state flag plus a screen-sized
		// setGeometry: the latter is what produced a decorated window larger
		// than the screen on Windows. Activated, so Windows drops the taskbar
		// behind it.
		showFullScreen();
		raise();
		activateWindow();
	} else {
		// Out of full screen lands on MAXIMISED, not the 1040x680 restore size:
		// the point of leaving is to get the title bar and taskbar back, not
		// to shrink the editor to a quarter of the monitor.
		showMaximized();
	}
}

void VideoEditorWindow::keyPressEvent(QKeyEvent *e)
{
	// Escape in a full-screen window means "give me my desktop back", the way
	// it does in every player and browser. QDialog's default would call
	// reject() -- closing the editor, with a discard prompt if there are edits
	// -- which is a severe thing to hang on the key everyone presses to get
	// out of full screen. Once out, Escape closes as before.
	if (e->key() == Qt::Key_Escape && isFullScreen() && e->modifiers() == Qt::NoModifier) {
		setFullScreen(false);
		e->accept();
		return;
	}
	QDialog::keyPressEvent(e);
}

void VideoEditorWindow::resizeEvent(QResizeEvent *e)
{
	QDialog::resizeEvent(e);
	// Opening maximised means the real height can arrive AFTER showEvent, and
	// applyModeSplit() divides whatever height it happens to find -- sized
	// against the restore-down 680 it leaves the preview short on a 1440p
	// screen. Run it once more on the first resize, when the window is the size
	// it is actually going to be.
	//
	// Once. After that the split belongs to the user, and applyModeSplit only
	// ever takes room away from the bottom pane -- so re-running it on every
	// resize would quietly undo a splitter they had dragged the other way.
	if (!modeSplitSized_ && isVisible()) {
		modeSplitSized_ = true;
		applyModeSplit();
	}
}

// Across the BOTTOM of the preview, inset a little so it reads as floating on
// the picture rather than being part of its frame.
//
// It began at the top and that was the wrong end: what it describes is the
// timeline, and putting it at the bottom edge puts it directly above the
// timeline, so the red box and the lanes it stands for are next to each other
// and the eye travels a few pixels instead of the height of the preview. It
// stays a child of the preview -- floating, taking no layout space -- because
// it is only meant to be there while you are moving.
void VideoEditorWindow::layOutOverview()
{
	if (!overview_ || !canvas_)
		return;
	const int m = 12;
	const int w = std::max(120, canvas_->width() - 2 * m);
	const int y = std::max(m, canvas_->height() - TimelineOverview::kHeight - m);
	overview_->setGeometry(m, y, w, TimelineOverview::kHeight);
}

// Show the overview for a view that just moved. Only in the modes that have
// something to get lost in: Simple Trim is one clip with two handles.
void VideoEditorWindow::onTimelineViewChanged(qint64 totalMs, qint64 startMs, qint64 visibleMs,
					      qint64 playheadMs)
{
	if (!overview_ || mode() == EditMode::Trim)
		return;
	layOutOverview();
	overview_->showFor(totalMs, startMs, visibleMs, playheadMs);
}

bool VideoEditorWindow::eventFilter(QObject *watched, QEvent *e)
{
	if (watched == canvas_ && e->type() == QEvent::Resize) {
		layOutOverview();
		layOutEmptyPanel();
	}
	// Closing the floating panel via its title-bar X mirrors the toolbar toggle.
	if (watched == sourcesPanel_ && e->type() == QEvent::Close) {
		if (sourcesBtn_ && sourcesBtn_->isChecked()) {
			QSignalBlocker b(sourcesBtn_);
			sourcesBtn_->setChecked(false);
		}
		QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		st.setValue(QStringLiteral("editor/sourcesGeom"), sourcesPanel_->saveGeometry());
		st.setValue(QStringLiteral("editor/sourcesShown"), false);
	}
	// Up / Down in the console's line walk its history, like any shell. An
	// event filter rather than a subclass: the line is one widget with one
	// extra behaviour, and the window already filters events for its panels.
	if (consoleIn_ && watched == consoleIn_ && e->type() == QEvent::KeyPress) {
		auto *ke = static_cast<QKeyEvent *>(e);
		// With the completion list open it owns Up/Down/Tab/Enter/Esc, like
		// any IDE: the arrows pick, Tab or Enter take the pick, Esc closes.
		if (consolePopup_ && consolePopup_->isVisible()) {
			const int n = consolePopup_->count();
			if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
				int row = consolePopup_->currentRow();
				row += (ke->key() == Qt::Key_Up) ? -1 : 1;
				if (row < 0)
					row = n - 1;
				if (row >= n)
					row = 0;
				consolePopup_->setCurrentRow(row);
				return true;
			}
			if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
				acceptConsoleCompletion();
				return true;
			}
			if (ke->key() == Qt::Key_Escape) {
				hideConsoleCompletions();
				return true;
			}
		} else if (ke->key() == Qt::Key_Tab) {
			// Tab with the list closed opens it (or takes the only pick).
			refreshConsoleCompletions();
			if (consolePopup_ && consolePopup_->isVisible() && consolePopup_->count() == 1)
				acceptConsoleCompletion();
			return true;
		}
		if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
			if (consoleHistory_.isEmpty())
				return true;
			if (consoleHistIdx_ < 0)
				consoleDraft_ = consoleIn_->text();
			int idx = consoleHistIdx_ < 0 ? consoleHistory_.size() : consoleHistIdx_;
			idx += (ke->key() == Qt::Key_Up) ? -1 : 1;
			if (idx < 0)
				idx = 0;
			if (idx >= consoleHistory_.size()) {
				consoleHistIdx_ = -1;
				consoleIn_->setText(consoleDraft_);
			} else {
				consoleHistIdx_ = idx;
				consoleIn_->setText(consoleHistory_[idx]);
			}
			consoleIn_->end(false);
			hideConsoleCompletions(); // a recalled line is not being typed
			return true;
		}
	}
	if (consoleIn_ && watched == consoleIn_ && e->type() == QEvent::FocusOut)
		hideConsoleCompletions();
	return QDialog::eventFilter(watched, e);
}

// ---- The editing console -----------------------------------------------------

void VideoEditorWindow::refreshConsoleCompletions()
{
	if (!consoleIn_ || !consolePopup_ || !timelineView_)
		return;
	const QString before = consoleIn_->text().left(consoleIn_->cursorPosition());
	const QStringList items = EditConsole::completions(before, timelineView_->model(), &consolePartial_);
	if (items.isEmpty()) {
		hideConsoleCompletions();
		return;
	}
	consolePopup_->clear();
	consolePopup_->addItems(items);
	consolePopup_->setCurrentRow(0);
	const int rowH = std::max(18, consolePopup_->sizeHintForRow(0));
	const int rows = std::min(std::max(3, panelPopupRows_), int(items.size()));
	const int w = std::clamp(consoleIn_->width() / 2, 180, 360);
	consolePopup_->resize(w, rows * rowH + 4);
	// Under the caret, roughly: the line's left edge plus the width of what
	// is typed before the partial, so the list sits where the eye is.
	const QFontMetrics fm(consoleIn_->font());
	const int x = std::min(consoleIn_->width() - w,
			       fm.horizontalAdvance(before.left(before.size() - consolePartial_.size())) + 4);
	consolePopup_->move(consoleIn_->mapToGlobal(QPoint(std::max(0, x), consoleIn_->height() + 2)));
	if (!consolePopup_->isVisible())
		consolePopup_->show();
}

void VideoEditorWindow::acceptConsoleCompletion()
{
	if (!consoleIn_ || !consolePopup_ || !consolePopup_->isVisible())
		return;
	QListWidgetItem *it = consolePopup_->currentItem();
	if (!it) {
		hideConsoleCompletions();
		return;
	}
	const QString pick = it->text();
	const int cur = consoleIn_->cursorPosition();
	QString text = consoleIn_->text();
	const int from = cur - consolePartial_.size();
	text.replace(from, consolePartial_.size(), pick);
	consoleIn_->setText(text);
	consoleIn_->setCursorPosition(from + pick.size());
	hideConsoleCompletions();
	// Picking an object ("clip", "tracks") is usually the start of a path:
	// offer its members straight away after a typed dot.
}

void VideoEditorWindow::hideConsoleCompletions()
{
	if (consolePopup_ && consolePopup_->isVisible())
		consolePopup_->hide();
}

QString VideoEditorWindow::consoleTemplatesDir() const
{
	const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
	const QString dir = base + QStringLiteral("/harpia/templates");
	QDir().mkpath(dir);
	return dir;
}

QStringList VideoEditorWindow::consoleTemplateNames() const
{
	QStringList names;
	for (const QFileInfo &fi : QDir(consoleTemplatesDir()).entryInfoList({QStringLiteral("*.js")}, QDir::Files, QDir::Name))
		names << fi.completeBaseName();
	return names;
}

void VideoEditorWindow::consolePrint(const QString &text, ConsoleTone tone)
{
	// A console error is an editor error too: into the log with it.
	if (tone == ConsoleTone::Error)
		editorLog(EditorLogLevel::Error, QStringLiteral("Console"), text.simplified());
	if (!consoleOut_)
		return;
	// One colour per kind of line, so a glance tells what you typed from what
	// came back, and a red line from a green one. appendHtml so the colour is
	// per line; the text itself is escaped, never interpreted.
	auto esc = [](const QString &t) {
		QString h = t.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
		// HTML folds runs of spaces to one, which un-aligns the help table's
		// columns. Every space in a run stays a space.
		while (h.contains(QLatin1String("  ")))
			h.replace(QLatin1String("  "), QLatin1String("&nbsp;&nbsp;"));
		return h;
	};
	auto span = [&esc](const char *color, const QString &t) {
		return QStringLiteral("<span style=\"color:%1;\">%2</span>").arg(QLatin1String(color), esc(t));
	};
	QString html;
	switch (tone) {
	case ConsoleTone::Command:
		// The prompt dim, the code itself in the input blue.
		if (text.startsWith(QLatin1String("> ")))
			html = span("#6c7380", QStringLiteral("> ")) + span("#8ec5ff", text.mid(2));
		else
			html = span("#8ec5ff", text);
		break;
	case ConsoleTone::Comment:
		html = span("#8a919e", text);
		break;
	case ConsoleTone::Error:
		html = span("#ff6b6b", text);
		break;
	case ConsoleTone::Success:
		html = span("#7ee787", text);
		break;
	case ConsoleTone::Help: {
		// The help table: the name in amber, its explanation in the plain
		// colour, section headings ("On a clip ...:") in the comment grey.
		QStringList out;
		for (const QString &ln : text.split(QLatin1Char('\n'))) {
			if (ln.trimmed().isEmpty()) {
				out << QString();
			} else if (ln.endsWith(QLatin1Char(':'))) {
				out << span("#8a919e", ln);
			} else {
				// Every "name   meaning" pair on the line: a name is a run of
				// non-spaces followed by two or more spaces.
				static const QRegularExpression rx(QStringLiteral("(^|\\s{2,})(\\S[^ ]*(?: \\S[^ ]*)?)(?=\\s{2,}|$)"));
				QString h;
				int pos = 0;
				auto it = rx.globalMatch(ln);
				while (it.hasNext()) {
					const QRegularExpressionMatch m = it.next();
					h += esc(ln.mid(pos, m.capturedStart(2) - pos));
					h += span("#f2c46d", m.captured(2));
					pos = m.capturedEnd(2);
				}
				h += esc(ln.mid(pos));
				out << h;
			}
		}
		html = out.join(QStringLiteral("<br>"));
		break;
	}
	case ConsoleTone::Plain:
	default:
		html = span("#e8eaed", text);
		break;
	}
	consoleOut_->appendHtml(html);
	consoleOut_->verticalScrollBar()->setValue(consoleOut_->verticalScrollBar()->maximum());
}

void VideoEditorWindow::openConsole()
{
	if (!fullEdit() || !timelineView_ || !EditConsole::available())
		return;
	if (!consolePanel_) {
		// A window of its own, not a tool palette: it shows in the taskbar, can
		// be moved to another monitor and alt-tabbed to, and sits beside a
		// full-screen editor rather than being a strip glued to it.
		auto *p = new QWidget(this, Qt::Window | Qt::WindowTitleHint | Qt::WindowMinMaxButtonsHint |
						Qt::WindowCloseButtonHint);
		p->setWindowTitle(QStringLiteral("Harpia Console"));
		auto *lay = new QVBoxLayout(p);
		lay->setContentsMargins(8, 8, 8, 8);
		lay->setSpacing(6);

		consoleOut_ = new QPlainTextEdit(p);
		consoleOut_->setReadOnly(true);
		consoleOut_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
		consoleOut_->setMaximumBlockCount(2000);
		QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
		mono.setPixelSize(std::max(11, uiReadoutPx() - 1));
		consoleOut_->setFont(mono);
		consoleOut_->setStyleSheet(QStringLiteral("QPlainTextEdit{background:#15171a;color:#e8eaed;border:1px solid #2b2f36;}"));
		consoleOut_->setMinimumHeight(panelConsoleMinH_);
		if (panelConsoleFontPx_ > 0) {
			QFont f = consoleOut_->font();
			f.setPixelSize(panelConsoleFontPx_);
			consoleOut_->setFont(f);
		}
		lay->addWidget(consoleOut_, 1);

		auto *row = new QHBoxLayout;
		row->setSpacing(6);
		auto *prompt = new QLabel(QStringLiteral(">"), p);
		prompt->setFont(mono);
		row->addWidget(prompt);
		consoleIn_ = new QLineEdit(p);
		consoleIn_->setFont(mono);
		consoleIn_->setPlaceholderText(QStringLiteral("clip.position = [500, 300]      help() for the list"));
		consoleIn_->setClearButtonEnabled(true);
		consoleIn_->installEventFilter(this);
		connect(consoleIn_, &QLineEdit::returnPressed, this, [this]() {
			const QString line = consoleIn_->text();
			consoleIn_->clear();
			hideConsoleCompletions();
			runConsoleLine(line);
		});
		// The completion list: a frameless list floating under the line,
		// never focused (the line keeps the keyboard; see eventFilter).
		consolePopup_ = new QListWidget(p);
		consolePopup_->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
		consolePopup_->setFocusPolicy(Qt::NoFocus);
		consolePopup_->setAttribute(Qt::WA_ShowWithoutActivating);
		consolePopup_->setFont(mono);
		consolePopup_->setStyleSheet(QStringLiteral(
			"QListWidget{background:#1d2026;color:#e8eaed;border:1px solid #3a3f48;}"
			"QListWidget::item{padding:2px 8px;}"
			"QListWidget::item:selected{background:#2f6fd6;color:#ffffff;}"));
		consolePopup_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		consolePopup_->hide();
		connect(consolePopup_, &QListWidget::itemClicked, this, [this](QListWidgetItem *) {
			acceptConsoleCompletion();
			consoleIn_->setFocus();
		});
		connect(consoleIn_, &QLineEdit::textEdited, this, [this](const QString &) {
			refreshConsoleCompletions();
		});
		connect(consoleIn_, &QLineEdit::cursorPositionChanged, this, [this](int, int) {
			if (consolePopup_ && consolePopup_->isVisible())
				refreshConsoleCompletions();
		});
		row->addWidget(consoleIn_, 1);
		lay->addLayout(row);

		auto *btns = new QHBoxLayout;
		btns->setSpacing(6);
		auto *runBtn = new QPushButton(QStringLiteral("Run"), p);
		runBtn->setToolTip(QStringLiteral("Run the line (Enter does too). One undo step."));
		connect(runBtn, &QPushButton::clicked, this, [this]() {
			const QString line = consoleIn_->text();
			consoleIn_->clear();
			runConsoleLine(line);
		});
		btns->addWidget(runBtn);
		auto *tplBtn = new QPushButton(QStringLiteral("Templates ▾"), p);
		tplBtn->setToolTip(QStringLiteral(
			"Saved *.js files. Pick one to run it; \"Save last line as template…\" keeps the "
			"line you just ran under a name, for run(\"name\") on any project."));
		connect(tplBtn, &QPushButton::clicked, this, [this, tplBtn]() {
			QMenu menu(this);
			const QStringList names = consoleTemplateNames();
			QHash<QAction *, QString> runActs;
			for (const QString &n : names)
				runActs.insert(menu.addAction(QStringLiteral("Run \"%1\"").arg(n)), n);
			if (names.isEmpty()) {
				QAction *none = menu.addAction(QStringLiteral("(no templates yet)"));
				none->setEnabled(false);
			}
			menu.addSeparator();
			QAction *save = menu.addAction(QStringLiteral("Save last line as template…"));
			save->setEnabled(!consoleHistory_.isEmpty());
			QAction *folder = menu.addAction(QStringLiteral("Open templates folder"));
			QAction *chosen = menu.exec(tplBtn->mapToGlobal(QPoint(0, tplBtn->height())));
			if (!chosen)
				return;
			if (const auto it = runActs.constFind(chosen); it != runActs.constEnd()) {
				runConsoleLine(QStringLiteral("run(\"%1\")").arg(it.value()));
			} else if (chosen == save) {
				saveConsoleTemplate();
			} else if (chosen == folder) {
				QDesktopServices::openUrl(QUrl::fromLocalFile(consoleTemplatesDir()));
			}
		});
		btns->addWidget(tplBtn);
		auto *helpBtn = new QPushButton(QStringLiteral("Help"), p);
		connect(helpBtn, &QPushButton::clicked, this, [this]() { runConsoleLine(QStringLiteral("help()")); });
		btns->addWidget(helpBtn);
		btns->addStretch(1);
		auto *clearBtn = new QPushButton(QStringLiteral("Clear"), p);
		connect(clearBtn, &QPushButton::clicked, this, [this]() { consoleOut_->clear(); });
		btns->addWidget(clearBtn);
		lay->addLayout(btns);

		p->resize(640, 420);
		consolePanel_ = p;
		consolePrint(QStringLiteral("// Editing console. Click a clip to see its name; help() lists what you can set."),
			     ConsoleTone::Comment);
		consolePrint(QStringLiteral("// Units: pixels for position, ms for time. Every line is one undo step (Ctrl+Z)."),
			     ConsoleTone::Comment);
		consolePrint(QStringLiteral("// Colours: blue = what you typed, white = what came back, amber = names in help(), "
					    "green = applied, red = error."),
			     ConsoleTone::Comment);
	}
	if (!consolePanel_->isVisible() && consoleBtn_)
		consolePanel_->move(consoleBtn_->mapToGlobal(QPoint(0, consoleBtn_->height() + 6)));
	consolePanel_->show();
	consolePanel_->raise();
	consolePanel_->activateWindow();
	consoleIn_->setFocus();
}

void VideoEditorWindow::runConsoleLine(const QString &lineIn)
{
	const QString line = lineIn.trimmed();
	if (line.isEmpty() || !timelineView_)
		return;
	if (consoleHistory_.isEmpty() || consoleHistory_.last() != line)
		consoleHistory_.append(line);
	consoleHistIdx_ = -1;
	consolePrint(QStringLiteral("> ") + line, ConsoleTone::Command);

	EditConsole::Input in;
	in.model = timelineView_->model();
	in.selection = timelineView_->selectedPairs();
	in.canvas = timelineCanvasSize();
	in.playheadMs = timelinePlayheadMs();
	in.sourceName = [this](int id) {
		for (const EditorSource &s : sources_)
			if (s.id == id)
				return s.name;
		return QString();
	};
	in.loadTemplate = [this](const QString &name) {
		// A name, never a path: the templates folder is the only place.
		const QString safe = QFileInfo(name).fileName();
		QFile f(consoleTemplatesDir() + QLatin1Char('/') + safe +
			(safe.endsWith(QLatin1String(".js")) ? QString() : QStringLiteral(".js")));
		if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
			return QString();
		return QString::fromUtf8(f.readAll());
	};

	const EditConsole::Result r = EditConsole::run(line, in);
	if (!r.output.isEmpty())
		consolePrint(r.output, line.startsWith(QLatin1String("help")) ? ConsoleTone::Help
									      : ConsoleTone::Plain);
	if (!r.ok) {
		consolePrint(r.error, ConsoleTone::Error);
		return; // nothing changed, by construction
	}
	if (!r.changed) {
		consolePrint(QStringLiteral("// ok, nothing changed"), ConsoleTone::Comment);
		return;
	}
	consolePrint(QStringLiteral("\u2713 applied \u2014 one undo step (Ctrl+Z)"), ConsoleTone::Success);
	// One undo step: the edited copy replaces the model, the selection is put
	// back (setModel clears it), and the snapshot is closed now rather than
	// waiting out the coalescing timer -- a second line must not merge into
	// the first's undo entry.
	const QVector<QPair<int, int>> sel = in.selection;
	timelineView_->setModel(r.model);
	for (int i = 0; i < sel.size(); ++i) {
		if (i == 0)
			timelineView_->selectClip(sel[i].first, sel[i].second);
		else
			timelineView_->addToSelection(sel[i].first, sel[i].second);
	}
	syncPreviewTransformTarget();
	showTimelineFrame(timelinePlayheadMs());
	syncComponentPanel();
	updateInspector();
	commitSnapshot();
}

void VideoEditorWindow::saveConsoleTemplate()
{
	if (consoleHistory_.isEmpty())
		return;
	bool ok = false;
	const QString name = QInputDialog::getText(this, QStringLiteral("Save template"),
						   QStringLiteral("Name (used as run(\"name\")):"),
						   QLineEdit::Normal, QString(), &ok)
				     .trimmed();
	if (!ok || name.isEmpty())
		return;
	const QString safe = QFileInfo(name).fileName();
	QFile f(consoleTemplatesDir() + QLatin1Char('/') + safe + QStringLiteral(".js"));
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		consolePrint(QStringLiteral("could not write %1").arg(f.fileName()), ConsoleTone::Error);
		return;
	}
	f.write(QStringLiteral("// Harpia template \"%1\"\n%2\n").arg(safe, consoleHistory_.last()).toUtf8());
	consolePrint(QStringLiteral("\u2713 saved as template \"%1\": run(\"%1\")").arg(safe), ConsoleTone::Success);
}

VideoEditorWindow::EditMode VideoEditorWindow::mode() const
{
	return stack_ ? EditMode(stack_->currentIndex()) : EditMode::Trim;
}

bool VideoEditorWindow::multiCut() const
{
	return mode() == EditMode::MultiCut;
}

bool VideoEditorWindow::fullEdit() const
{
	return mode() == EditMode::Full;
}

bool VideoEditorWindow::hasUnsavedEdits() const
{
	if (!valid_)
		return false;
	if (!tracks_->segments().isEmpty())
		return true; // Multi-Cut edit in progress
	// The trim range only means something against a video: an editor opened
	// blank, or with its last source removed, has none -- and closing one of
	// those used to read through a null (or freed) decoder and crash.
	if (seeker_ && (timeline_->start() != 0 || timeline_->end() != seeker_->durationMs()))
		return true; // trim range changed
	if (speed_ != 1.0)
		return true;
	if (cropToggle_->isChecked())
		return true;
	if (voTrack_ && !voTrack_->isEmpty())
		return true; // recorded narration would be lost
	if (timelineView_ && !timelineView_->model().isEmpty())
		return true; // Full-editing timeline in progress
	if (projCanvas_.isValid() || projFps_ >= 1.0)
		return true; // a pinned output format is a decision worth keeping
	return false;
}

QString VideoEditorWindow::autosaveProjectPath() const
{
	if (projectPath_.isEmpty())
		return {};
	const QFileInfo fi(projectPath_);
	return fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() +
	       QStringLiteral("_autosave.harpiaproj");
}

// The Unsaved Changes prompt. Three ways out, as in every editor: save and
// then close, close and lose the changes, or go back to work.
//
// Hand-built rather than a QMessageBox: QMessageBox lays its buttons out by
// role, in an order that differs per platform, and it would not keep
// "Don't Save | Cancel | Save Project". Styling one button inside one also
// made "Don't Save" read as disabled rather than destructive.
//
// Save Project is the default, so Enter cannot destroy anything; Esc is Cancel.
// A failed save keeps the dialog up with the reason rather than closing and
// silently losing the project, which is the whole point of the prompt.
bool VideoEditorWindow::confirmDiscardOnClose()
{
	if (!hasUnsavedEdits())
		return true; // nothing to lose: no prompt at all

	stopPlayback();

	// What the user needs in order to decide: which project, how much work is
	// at stake, and whether anything was already written down.
	QStringList facts;
	facts << QStringLiteral("<span style='color:#e8eaed;'><b>%1</b></span>")
			 .arg(projectPath_.isEmpty() ? QStringLiteral("Untitled project")
						     : QFileInfo(projectPath_).fileName());
	if (projectPath_.isEmpty()) {
		facts << QStringLiteral("Not saved yet.");
	} else {
		const QDateTime saved = QFileInfo(projectPath_).lastModified();
		facts << (saved.isValid() ? QStringLiteral("Last saved %1.").arg(relativeAge(saved))
					  : QStringLiteral("Last saved time unknown."));
	}
	const QString autoPath = autosaveProjectPath();
	if (!autoPath.isEmpty() && QFileInfo::exists(autoPath))
		facts << QStringLiteral("An autosave from %1 is on disk, so recent work may be "
					"recoverable from <i>%2</i>.")
				 .arg(relativeAge(QFileInfo(autoPath).lastModified()),
				      QFileInfo(autoPath).fileName());

	QDialog dlg(this);
	dlg.setWindowTitle(QStringLiteral("Unsaved Changes"));
	dlg.setModal(true);
	// Wide enough that the headline is one line: a wrapped warning reads as an
	// afterthought.
	dlg.setMinimumWidth(470);

	auto *outer = new QVBoxLayout(&dlg);
	outer->setContentsMargins(20, 18, 20, 16);
	outer->setSpacing(16);

	auto *top = new QHBoxLayout;
	top->setSpacing(16);
	auto *icon = new QLabel(&dlg);
	// A warning, not an error: nothing has gone wrong, a choice is being asked.
	icon->setPixmap(dlg.style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(40, 40));
	icon->setAlignment(Qt::AlignTop);
	top->addWidget(icon, 0, Qt::AlignTop);

	auto *col = new QVBoxLayout;
	col->setSpacing(8);
	auto *head = new QLabel(QStringLiteral("You have unsaved changes to this project."), &dlg);
	head->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed; font-size:%1px;").arg(uiHeadingPx()));
	head->setWordWrap(true);
	col->addWidget(head);
	auto *body = new QLabel(
		QStringLiteral("If you close the project now, all changes made since the last "
			       "save will be permanently lost."),
		&dlg);
	body->setWordWrap(true);
	col->addWidget(body);
	auto *info = new QLabel(facts.join(QStringLiteral("<br>")), &dlg);
	info->setTextFormat(Qt::RichText);
	info->setWordWrap(true);
	info->setStyleSheet(QStringLiteral("color:#9a9fa8; padding:8px 10px; background:#25272b; "
					   "border-radius:5px;"));
	col->addWidget(info);
	auto *errLabel = new QLabel(&dlg);
	errLabel->setWordWrap(true);
	errLabel->setStyleSheet(QStringLiteral("color:#e5484d;"));
	errLabel->setVisible(false);
	col->addWidget(errLabel);
	top->addLayout(col, 1);
	outer->addLayout(top);

	// Left-to-right exactly as asked, and laid out by hand so it stays that way
	// on every platform.
	auto *row = new QHBoxLayout;
	row->setSpacing(8);
	auto *dontBtn = new QPushButton(QStringLiteral("Don't Save"), &dlg);
	dontBtn->setStyleSheet(QStringLiteral(
		"QPushButton { color:#ff6b6f; border:1px solid #6b2f31; border-radius:4px; "
		"padding:6px 16px; background:#2b2426; } "
		"QPushButton:hover { background:#3a2c2e; }"));
	row->addWidget(dontBtn);
	row->addStretch(1);
	auto *cancelBtn = new QPushButton(QStringLiteral("Cancel"), &dlg);
	cancelBtn->setStyleSheet(QStringLiteral("padding:6px 16px;"));
	row->addWidget(cancelBtn);
	auto *saveBtn = new QPushButton(QStringLiteral("Save Project"), &dlg);
	saveBtn->setDefault(true);
	saveBtn->setAutoDefault(true);
	saveBtn->setStyleSheet(QStringLiteral(
		"QPushButton { font-weight:bold; color:#ffffff; border:1px solid #1a7fb5; "
		"border-radius:4px; padding:6px 18px; background:#1c88c4; } "
		"QPushButton:hover { background:#2596d6; }"));
	row->addWidget(saveBtn);
	outer->addLayout(row);

	// 0 = cancel (also what Esc and the title-bar X give), 1 = discard, 2 = save.
	connect(cancelBtn, &QPushButton::clicked, &dlg, [&dlg]() { dlg.done(0); });
	connect(dontBtn, &QPushButton::clicked, &dlg, [&dlg]() { dlg.done(1); });
	connect(saveBtn, &QPushButton::clicked, &dlg, [&dlg]() { dlg.done(2); });
	saveBtn->setFocus();

	// Looped, so a failed save leaves the dialog open with the reason on it.
	for (;;) {
		const int r = dlg.exec();
		if (r == 1)
			return true; // close now, changes discarded
		if (r != 2)
			return false; // Cancel / Esc / X: back to the editor, nothing lost
		// Save Project: close only once it has actually worked.
		if (projectPath_.isEmpty()) {
			onSaveProjectAs();
			if (projectPath_.isEmpty())
				continue; // the file dialog was cancelled: ask again
			return true;  // Save As has already written the file
		}
		const QString err = saveProjectTo(projectPath_, /*quiet=*/true);
		if (err.isEmpty()) {
			refreshProjectInspector();
			return true;
		}
		errLabel->setText(QStringLiteral("Could not save: %1").arg(err));
		errLabel->setVisible(true);
	}
}

void VideoEditorWindow::reject()
{
	// Exports save to a NEW file, so "unsaved" means any edit that would be
	// lost by closing now. A successful export closes via accept() instead.
	if (!confirmDiscardOnClose())
		return; // keep editing
	QDialog::reject();
}

QString VideoEditorWindow::textPresetsPath() const
{
	const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
	QDir().mkpath(base + QStringLiteral("/harpia"));
	return base + QStringLiteral("/harpia/text-presets.json");
}

// name -> style. A plain readable file, so a preset can be shared by copying it.
QMap<QString, TlText> VideoEditorWindow::loadTextPresets() const
{
	QMap<QString, TlText> out;
	QFile f(textPresetsPath());
	if (!f.open(QIODevice::ReadOnly))
		return out;
	const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
	for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
		TlText t;
		applyTextStyleFromJson(it.value().toObject(), t);
		out.insert(it.key(), t);
	}
	return out;
}

void VideoEditorWindow::saveTextPresets(const QMap<QString, TlText> &presets) const
{
	QJsonObject root;
	for (auto it = presets.constBegin(); it != presets.constEnd(); ++it)
		root.insert(it.key(), textStyleToJson(it.value()));
	QFile f(textPresetsPath());
	if (f.open(QIODevice::WriteOnly))
		f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void VideoEditorWindow::refreshTextPresets()
{
	if (!textPresetCombo_)
		return;
	// Read afresh so edits from another window or an external editor are seen.
	// Only names affect these rows; applying a style still reads its current
	// values from disk. Ordinary Inspector refreshes need not rebuild the list.
	const auto saved = loadTextPresets();
	const QStringList names = saved.keys();
	if (textPresetListReady_ && displayedTextPresetNames_ == names)
		return;
	displayedTextPresetNames_ = names;
	textPresetListReady_ = true;
	const QString keep = textPresetCombo_->currentData().toString();
	const QSignalBlocker b(textPresetCombo_);
	textPresetCombo_->clear();
	textPresetCombo_->addItem(QStringLiteral("Style preset…"));
	QStringList builtinNames;
	for (const auto &preset : text_style_presets::builtins()) {
		textPresetCombo_->addItem(preset.name, QStringLiteral("builtin/") + preset.id);
		textPresetCombo_->setItemData(textPresetCombo_->count() - 1, preset.description, Qt::ToolTipRole);
		builtinNames << preset.name;
	}
	if (!saved.isEmpty())
		textPresetCombo_->insertSeparator(textPresetCombo_->count());
	for (auto it = saved.constBegin(); it != saved.constEnd(); ++it) {
		const QString label = builtinNames.contains(it.key()) ? it.key() + QStringLiteral(" (Saved)") : it.key();
		textPresetCombo_->addItem(label, QStringLiteral("saved/") + it.key());
		textPresetCombo_->setItemData(textPresetCombo_->count() - 1, QStringLiteral("Your saved text style: %1").arg(it.key()), Qt::ToolTipRole);
	}
	const int i = keep.isEmpty() ? 0 : textPresetCombo_->findData(keep);
	textPresetCombo_->setCurrentIndex(i >= 0 ? i : 0);
	textPresetDelete_->setEnabled(textPresetCombo_->currentData().toString().startsWith(QStringLiteral("saved/")));
}

void VideoEditorWindow::applyTextPreset(const QString &key)
{
	TlText style;
	if (key.startsWith(QStringLiteral("builtin/"))) {
		const auto *preset = text_style_presets::find(key.mid(8));
		if (!preset)
			return;
		style = preset->style;
	} else if (key.startsWith(QStringLiteral("saved/"))) {
		const auto presets = loadTextPresets();
		const auto it = presets.constFind(key.mid(6));
		if (it == presets.constEnd())
			return;
		style = it.value();
	} else {
		return;
	}
	// Style only: the clip keeps its own words, which is the whole point of a
	// preset — apply the look you settled on to whatever this caption says.
	// Each menu choice is one complete undo action, including rapid choices.
	commitSnapshot();
	editTextStyle([&style](TlClip &c) {
		text_style_presets::applyStyle(style, c.text);
	});
	commitSnapshot();
	syncClipInspector();
}

void VideoEditorWindow::saveTextPresetFromSelection()
{
	const TlClip *sel = inspectedClip();
	if (!sel || sel->type != TlClip::Type::Text)
		return;
	auto presets = loadTextPresets();
	bool ok = false;
	const QString name =
		QInputDialog::getText(this, QStringLiteral("Save text style"),
				      QStringLiteral("Name for this style:"), QLineEdit::Normal,
				      QString(), &ok)
			.trimmed();
	if (!ok || name.isEmpty())
		return;
	if (presets.contains(name) &&
	    QMessageBox::question(this, QStringLiteral("Replace style"),
				  QStringLiteral("\"%1\" already exists. Replace it?").arg(name)) !=
		    QMessageBox::Yes)
		return;
	presets.insert(name, sel->text); // t.text rides along but is ignored on apply
	saveTextPresets(presets);
	refreshTextPresets();
	if (textPresetCombo_) {
		const QSignalBlocker b(textPresetCombo_);
		textPresetCombo_->setCurrentIndex(textPresetCombo_->findData(QStringLiteral("saved/") + name));
		textPresetDelete_->setEnabled(true);
	}
}

void VideoEditorWindow::deleteSelectedTextPreset()
{
	if (!textPresetCombo_ || textPresetCombo_->currentIndex() <= 0)
		return;
	const QString key = textPresetCombo_->currentData().toString();
	if (!key.startsWith(QStringLiteral("saved/")))
		return;
	const QString name = key.mid(6);
	if (QMessageBox::question(this, QStringLiteral("Delete style"),
				  QStringLiteral("Delete the text style \"%1\"?").arg(name)) !=
	    QMessageBox::Yes)
		return;
	auto presets = loadTextPresets();
	presets.remove(name);
	saveTextPresets(presets);
	refreshTextPresets();
}

QString VideoEditorWindow::variationPresetsPath() const
{
	return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
	       QStringLiteral("/harpia/variation-presets.json");
}

void VideoEditorWindow::refreshVariationPresets()
{
	if (!variationPresetCombo_)
		return;
	const QString keep = variationPresetCombo_->currentIndex() > 0 ? variationPresetCombo_->currentText() : QString();
	const QSignalBlocker b(variationPresetCombo_);
	variationPresetCombo_->clear();
	variationPresetCombo_->addItem(QStringLiteral("Saved lists…"));
	const auto presets = variation_presets::load(variationPresetsPath());
	for (auto it = presets.constBegin(); it != presets.constEnd(); ++it) {
		variationPresetCombo_->addItem(it.key());
		variationPresetCombo_->setItemData(variationPresetCombo_->count() - 1,
						   it.value().join(QLatin1Char('\n')), Qt::ToolTipRole);
	}
	const int i = keep.isEmpty() ? 0 : variationPresetCombo_->findText(keep);
	variationPresetCombo_->setCurrentIndex(i >= 0 ? i : 0);
}

void VideoEditorWindow::applyVariationPreset(const QString &name)
{
	const auto presets = variation_presets::load(variationPresetsPath());
	const auto it = presets.constFind(name);
	if (it == presets.constEnd())
		return;
	// The alternatives only: the caption's own text is version 1 and stays.
	// One edit, so Ctrl+Z puts the previous list back.
	const QStringList lines = it.value();
	editSelectedClip([&lines](TlClip &c) { c.text.variations = lines; });
	previewVariant_ = 0;
	syncClipInspector();
	requestPreview(-1, timelinePlayheadMs());
}

void VideoEditorWindow::saveVariationPresetFromSelection()
{
	const TlClip *sel = inspectedClip();
	if (!sel || sel->type != TlClip::Type::Text)
		return;
	const QStringList lines = variation_presets::cleaned(sel->text.variations);
	if (lines.isEmpty()) {
		QMessageBox::information(this, QStringLiteral("Save variations"),
					 QStringLiteral("Add some texts first (+ Add text)."));
		return;
	}
	auto presets = variation_presets::load(variationPresetsPath());
	bool ok = false;
	const QString suggested = variationPresetCombo_ && variationPresetCombo_->currentIndex() > 0
					  ? variationPresetCombo_->currentText()
					  : QString();
	const QString name = QInputDialog::getText(this, QStringLiteral("Save variations"),
						   QStringLiteral("Name for this list (%1 lines):").arg(lines.size()),
						   QLineEdit::Normal, suggested, &ok)
				     .trimmed();
	if (!ok || name.isEmpty())
		return;
	if (presets.contains(name) &&
	    QMessageBox::question(this, QStringLiteral("Replace list"),
				  QStringLiteral("\"%1\" already exists. Replace it?").arg(name)) != QMessageBox::Yes)
		return;
	presets.insert(name, lines);
	if (!variation_presets::save(variationPresetsPath(), presets)) {
		QMessageBox::warning(this, QStringLiteral("Save variations"),
				     QStringLiteral("Could not write %1").arg(variationPresetsPath()));
		return;
	}
	refreshVariationPresets();
	if (variationPresetCombo_) {
		const QSignalBlocker b(variationPresetCombo_);
		variationPresetCombo_->setCurrentIndex(variationPresetCombo_->findText(name));
	}
}

void VideoEditorWindow::deleteSelectedVariationPreset()
{
	if (!variationPresetCombo_ || variationPresetCombo_->currentIndex() <= 0)
		return;
	const QString name = variationPresetCombo_->currentText();
	if (QMessageBox::question(this, QStringLiteral("Delete list"),
				  QStringLiteral("Delete the saved variations \"%1\"?").arg(name)) != QMessageBox::Yes)
		return;
	auto presets = variation_presets::load(variationPresetsPath());
	presets.remove(name);
	variation_presets::save(variationPresetsPath(), presets);
	refreshVariationPresets();
}

void VideoEditorWindow::copySelectedClips(bool cut)
{
	if (!timelineView_)
		return;
	// A selected track header means the whole track, clips and settings. The two
	// selections are exclusive in the view, so this is never a guess about which
	// the user meant -- whichever one is lit is the one Ctrl+C takes.
	const int hdr = timelineView_->selectedHeaderTrack();
	if (hdr >= 0) {
		TlTrack t;
		if (!timelineView_->copySelectedTrack(&t))
			return;
		trackClipboard_ = t;
		haveTrackClipboard_ = true;
		trackCopiedFrom_ = hdr;
		trackCopySeq_ = ++seqCounter_;
		if (cut)
			timelineView_->deleteTrack(hdr);
		return;
	}
	const auto sel = timelineView_->copySelection();
	if (sel.isEmpty())
		return;
	clipboard_ = sel;
	copySeq_ = ++seqCounter_; // for Ctrl+V's newest-wins rule
	if (cut)
		timelineView_->deleteSelected();
}

void VideoEditorWindow::pasteClips()
{
	if (!timelineView_ || clipboard_.isEmpty())
		return;
	// Land at the playhead, the one place the user is definitely looking.
	timelineView_->pasteAt(clipboard_, timelinePlayheadMs());
}

// Somewhere to put a picture that arrived without a file behind it.
//
// This is not an implementation detail that could have gone either way. A
// source is identified by its PATH: the project writes sources[].path and
// relinks by it on open, and the exporter re-reads that path on a worker thread
// rather than touching the editor's in-memory cache. An image held only in RAM
// would therefore preview perfectly, save a project that reopens broken, and
// export as an empty frame -- three symptoms, one cause, none of them visible
// until after the paste looked like it worked.
//
// The app's own config folder rather than a temp dir, because "still there
// tomorrow" is the whole requirement.
QString VideoEditorWindow::pastedFilePath(const QString &ext, QString *why)
{
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
			    QStringLiteral("/pasted");
	if (!QDir().mkpath(dir)) {
		if (why)
			*why = QStringLiteral("Could not create the folder for pasted files:\n%1")
				       .arg(QDir::toNativeSeparators(dir));
		return QString();
	}
	// Timestamped so the folder stays browsable, plus a counter because two
	// pastes inside one second is normal and losing the first would be silent.
	const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"));
	QString path;
	for (int n = 0; n < 10000; ++n) {
		path = n == 0 ? QStringLiteral("%1/pasted-%2.%3").arg(dir, stamp, ext)
			      : QStringLiteral("%1/pasted-%2-%3.%4").arg(dir, stamp).arg(n).arg(ext);
		if (!QFileInfo::exists(path))
			break;
	}
	return path;
}

QString VideoEditorWindow::writePastedImage(const QImage &img, QString *why)
{
	const auto fail = [why](const QString &msg) {
		if (why)
			*why = msg;
		return QString();
	};
	if (img.isNull())
		return fail(QStringLiteral("The clipboard image was empty."));
	const QString path = pastedFilePath(QStringLiteral("png"), why);
	if (path.isEmpty())
		return QString();
	// PNG, not the clipboard's original format: lossless, alpha-preserving, and
	// readable by the plain QImage(path) the export worker uses.
	if (!img.save(path, "PNG"))
		return fail(QStringLiteral("Could not write the pasted image to:\n%1")
				    .arg(QDir::toNativeSeparators(path)));
	return path;
}

QString VideoEditorWindow::sourcePathForTest(int sourceId) const
{
	for (const EditorSource &s : sources_)
		if (s.id == sourceId)
			return s.path;
	return {};
}

QString VideoEditorWindow::sourceFileForTest(int sourceId) const
{
	for (const EditorSource &s : sources_)
		if (s.id == sourceId)
			return s.origPath.isEmpty() ? s.path : s.origPath;
	return {};
}

QString VideoEditorWindow::infoTextForTest() const
{
	return infoLabel_ ? infoLabel_->text() : QString();
}

bool VideoEditorWindow::systemClipboardHasPaste() const
{
	// The menu entry greys from the same reading the paste acts on, so the
	// two cannot disagree about what is there.
	return clipboardHasWave() ||
	       readPaste(QApplication::clipboard()->mimeData()).kind != PasteContent::Kind::Nothing;
}

bool VideoEditorWindow::pasteSystemClipboard(qint64 atMs)
{
	if (!timelineView_)
		return false;
	if (!fullEdit())
		setEditMode(EditMode::Full);
	const PasteContent c = readPaste(QApplication::clipboard()->mimeData(), clipboardWave());
	switch (c.kind) {
	case PasteContent::Kind::Nothing:
		// Not silence: a paste that does nothing at all reads as broken.
		if (infoLabel_)
			infoLabel_->setText(QStringLiteral("There is nothing on the clipboard to paste."));
		return false;
	case PasteContent::Kind::Files:
		// Used where they lie: a file already has a path, which is all the
		// project and the exporter need.
		return pasteMediaFiles(c.files, atMs);
	case PasteContent::Kind::Image: {
		QString why;
		const QString path = writePastedImage(c.image, &why);
		if (path.isEmpty()) {
			warnAndLog(this, QStringLiteral("Paste"), why);
			return false;
		}
		return pasteMediaFiles({path}, atMs);
	}
	case PasteContent::Kind::Audio: {
		// Sound copied in an audio app has no file either; it is given one
		// in the same folder as pasted pictures, for the same reason.
		QString why;
		const QString path = pastedFilePath(c.audioExt, &why);
		QFile f(path);
		if (path.isEmpty() || !f.open(QIODevice::WriteOnly) || f.write(c.audio) != c.audio.size()) {
			warnAndLog(this, QStringLiteral("Paste"),
				   why.isEmpty() ? QStringLiteral("Could not write the pasted sound to:\n%1")
							   .arg(QDir::toNativeSeparators(path))
						 : why);
			return false;
		}
		f.close();
		return pasteMediaFiles({path}, atMs);
	}
	case PasteContent::Kind::Link:
		pasteLink(c.link, atMs);
		return true;
	case PasteContent::Kind::Text:
		pasteText(c.text, atMs);
		return true;
	}
	return false;
}

void VideoEditorWindow::pasteText(const QString &text, qint64 atMs)
{
	if (!timelineView_)
		return;
	TlClip c;
	c.type = TlClip::Type::Text;
	c.srcStartMs = 0;
	c.srcEndMs = 4000; // as long as Add > Text's
	c.outStartMs = std::max<qint64>(0, atMs);
	// A page of text pasted by accident is still the user's text; past this
	// it stops being a caption and starts being a hang in the text layout.
	c.text.text = text.left(5000);
	timelineView_->placePasted({{TlTrack::Kind::Video, {c}}});
	syncPreviewTransformTarget();
	syncClipInspector();
	showTimelineFrame(timelinePlayheadMs());
}

void VideoEditorWindow::pasteLink(const QUrl &url, qint64 atMs)
{
	if (!linkDownload_) {
		linkDownload_ = new LinkDownload(this);
		linkProgress_ = new QProgressDialog(this);
		linkProgress_->setWindowTitle(QStringLiteral("Paste"));
		linkProgress_->setAutoReset(false); // reset() -- and hiding -- is done()'s
		linkProgress_->setMinimumDuration(400); // a small picture never shows it
		linkProgress_->reset(); // or it shows itself, empty, a moment from now
		connect(linkProgress_, &QProgressDialog::canceled, linkDownload_, &LinkDownload::cancel);
		connect(linkDownload_, &LinkDownload::progress, this, [this](qint64 got, qint64 total) {
			if (total > 0) {
				linkProgress_->setMaximum(1000);
				linkProgress_->setValue(int(got * 1000 / total));
			} else {
				linkProgress_->setMaximum(0); // busy, size unknown
				linkProgress_->setValue(0);
			}
		});
	}
	if (linkDownload_->running()) {
		if (infoLabel_)
			infoLabel_->setText(QStringLiteral("Still downloading the last link you pasted."));
		return;
	}
	// One connection per paste, so each download lands at the playhead it was
	// pasted at, not wherever the playhead has moved to since.
	auto *ctx = new QObject(this);
	const auto done = [this, ctx]() {
		linkProgress_->reset();
		ctx->deleteLater();
	};
	connect(linkDownload_, &LinkDownload::fileReady, ctx, [this, atMs, done](const QString &path) {
		done();
		if (pasteMediaFiles({path}, atMs) && infoLabel_)
			infoLabel_->setText(QStringLiteral("Pasted %1.").arg(QFileInfo(path).fileName()));
	});
	connect(linkDownload_, &LinkDownload::failed, ctx, [this, done](const QString &why) {
		done();
		warnAndLog(this, QStringLiteral("Paste"), QStringLiteral("The link could not be downloaded.\n\n%1").arg(why));
	});
	// Cancelled: nothing more will arrive for this paste, and a later one must
	// not find these connections still listening.
	connect(linkProgress_, &QProgressDialog::canceled, ctx, done);
	connect(linkDownload_, &LinkDownload::notMedia, ctx, [this, done](const QUrl &page, const QString &) {
		done();
		// A page, not the file itself: a video page is what yt-dlp is for, and
		// its dialog picks the quality and lands the video when it is done.
		if (YtDlp::available()) {
			openUrlDownload();
			if (urlDialog_)
				urlDialog_->setUrl(page.toString());
			return;
		}
		warnAndLog(this, QStringLiteral("Paste"),
			   QStringLiteral("That link is a web page, not a picture, sound or video file.\n\n"
					  "To download videos from pages, install yt-dlp and point Harpia at it "
					  "(Settings \u2192 Downloads)."));
	});
	linkProgress_->setLabelText(QStringLiteral("Downloading %1\u2026").arg(url.host()));
	linkProgress_->setMaximum(0);
	linkProgress_->setValue(0); // starts the clock on showing it
	linkDownload_->start(url, QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
					  QStringLiteral("/pasted"));
}

// One key, two plausible meanings, newest wins.
//
// Ctrl+V has meant "paste the clips I copied inside Harpia" since the timeline
// existed, and it now also has to mean "paste the screenshot I just took". A
// mode switch or a second shortcut would be honest but nobody would find it, so
// the dispatch follows what was copied last -- which is what the hand already
// expects from every other editor.
void VideoEditorWindow::pasteFromClipboard()
{
	// A copied track wins when it is the most recent copy, by the same
	// newest-wins rule that already settles clips against the system clipboard.
	// Landing ABOVE the track it came from makes Ctrl+C, Ctrl+V read as
	// "duplicate this", and a second Ctrl+V stacks another copy above that one
	// rather than piling them all in the same place.
	const bool haveTrack = timelineView_ && fullEdit() && haveTrackClipboard_;
	if (haveTrack && trackCopySeq_ > copySeq_ && trackCopySeq_ > systemCopySeq_) {
		const int at = timelineView_->pasteTrack(trackClipboard_, trackCopiedFrom_);
		if (at >= 0) {
			// The pasted lane is the one a further paste duplicates.
			trackCopiedFrom_ = at;
			updateInfoLabel();
			showTimelineFrame(timelinePlayheadMs());
		}
		return;
	}
	const bool haveClips = timelineView_ && fullEdit() && !clipboard_.isEmpty();
	bool haveMedia = systemClipboardHasPaste();
	// Words copied inside Harpia's own boxes -- a clip name, a line of the
	// Inspector -- never take the place of clips you copied: that copy was
	// a side effect of editing, not a thing to put on the timeline. Words
	// from another app follow the newest-wins rule like a picture does.
	if (haveMedia && haveClips && systemCopyHere_ &&
	    readPaste(QApplication::clipboard()->mimeData()).kind == PasteContent::Kind::Text)
		haveMedia = false;
	if (!haveClips && !haveMedia) {
		// Not silence: Ctrl+V doing nothing at all reads as a broken shortcut.
		pasteSystemClipboard(timelinePlayheadMs()); // says there is nothing
		return;
	}
	if (haveMedia && (!haveClips || systemCopySeq_ > copySeq_)) {
		pasteSystemClipboard(timelinePlayheadMs());
		return;
	}
	pasteClips();
}

void VideoEditorWindow::applyModeSplit()
{
	if (editorLayout_ != EditorLayout::Standard)
		return; // the preview is a column of its own: see sizeVerticalLayout
	// The three modes need very different amounts of room below the preview:
	// Simple Trim is one short bar, Full editing a whole track stack. Without
	// this the split stayed wherever the tallest mode left it, and Simple Trim
	// showed a band of dead space under its timeline while the preview was
	// squeezed. Only ever gives space BACK to the preview.
	if (!vsplit_ || !bottomPane_ || !stack_ || !stack_->currentWidget())
		return;
	const QList<int> sizes = vsplit_->sizes();
	if (sizes.size() != 2)
		return;
	const int total = sizes[0] + sizes[1];
	if (total <= 0)
		return;
	// A height the user chose for this mode wins: put it back, within reason
	// (the window may be smaller than when it was set).
	{
		const QVariant saved = QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"))
					       .value(QStringLiteral("editor/splitBottom/%1").arg(int(mode())));
		if (saved.isValid()) {
			const int floor = std::max(80, bottomPane_->minimumSizeHint().height());
			const int want = std::clamp(saved.toInt(), floor, std::max(floor, total - 160));
			vsplit_->setSizes({total - want, want});
			return;
		}
	}
	// The stack reports the TALLEST page, so ask this page directly and add the
	// rows around it (mode bar, audio foldout, buttons) measured, not guessed.
	const int chrome = bottomPane_->sizeHint().height() - stack_->sizeHint().height();
	const int wanted = std::clamp(chrome + stack_->currentWidget()->sizeHint().height(), 120,
				      std::max(120, total / 2));
	if (sizes[1] > wanted)
		vsplit_->setSizes({total - wanted, wanted});
}

bool VideoEditorWindow::insTabShowing(int tab) const
{
	return insTabs_ && inspector_ && inspector_->isVisible() && insTabs_->currentIndex() == tab;
}

void VideoEditorWindow::refreshShownInspectorTab()
{
	if (clipTabDirty_ && insTabShowing(kInsTabClip)) {
		clipTabDirty_ = false;
		syncClipInspector();
	}
	if (soundsTabDirty_ && insTabShowing(kInsTabSounds))
		rebuildSoundsTab();
	if (tagsDirty_ && insTabShowing(kInsTabProject))
		rebuildTagsSection();
}

void VideoEditorWindow::showInspector(bool on)
{
	if (!inspector_)
		return;
	inspector_->setVisible(on);
	if (on)
		refreshShownInspectorTab();
	if (!on || !hsplit_)
		return;
	// A QSplitter hands a freshly-shown pane whatever its stretch factor implies,
	// which here was its bare minimum — narrow enough to clip the panel's own
	// controls. Widen it to something the content fits in, but never more than a
	// third of the window, and leave it alone once it is already wide enough (so
	// a width the user chose is kept).
	//
	// Deferred: the splitter re-lays out when the pane is shown, and doing this
	// inline just gets overwritten by that pass.
	QTimer::singleShot(0, this, [this]() {
		if (!hsplit_ || !inspector_ || !inspector_->isVisible())
			return;
		int total = 0;
		for (int w : hsplit_->sizes())
			total += w;
		const int want = std::clamp(inspectorParams_.openWidth, inspector_->minimumWidth(),
					    std::max(inspector_->minimumWidth(), total / 3));
		if (inspectorWidth() < want)
			setInspectorWidth(want);
	});
}

QWidget *VideoEditorWindow::editingAreaPane() const
{
	return editorLayout_ == EditorLayout::Vertical ? bottomPane_ : static_cast<QWidget *>(vsplit_);
}

int VideoEditorWindow::inspectorWidth() const
{
	return hsplit_ ? hsplit_->sizes().value(hsplit_->indexOf(inspector_)) : 0;
}

void VideoEditorWindow::setInspectorWidth(int want)
{
	if (!hsplit_)
		return;
	QList<int> sizes = hsplit_->sizes();
	const int ins = hsplit_->indexOf(inspector_), area = hsplit_->indexOf(editingAreaPane());
	if (ins < 0 || area < 0 || ins >= sizes.size() || area >= sizes.size())
		return;
	const int delta = want - sizes[ins];
	sizes[ins] = want;
	sizes[area] = std::max(0, sizes[area] - delta);
	hsplit_->setSizes(sizes);
}

VideoEditorWindow::EditorLayout VideoEditorWindow::savedEditorLayout() const
{
	QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	// v0.1.438 kept one per mode; Full editing's (where vertical work is
	// done) carries over as the choice for all of them.
	const QVariant fullsOwn = st.value(QStringLiteral("editor/layout/%1").arg(int(EditMode::Full)), 0);
	const int v = st.value(QStringLiteral("editor/layout"), fullsOwn).toInt();
	return v == int(EditorLayout::Vertical) ? EditorLayout::Vertical : EditorLayout::Standard;
}

void VideoEditorWindow::adaptEditingArea()
{
	if (!stack_ || !bottomLayout_ || !bottomFill_)
		return;
	const bool tall = editorLayout_ == EditorLayout::Vertical;
	timeline_->setTallBar(tall);
	tracks_->setTallTracks(tall);
	// The stack would otherwise stretch Trim's one bar over the whole column
	// (and a stack is as tall as its TALLEST page, Full editing's timeline):
	// pin it to the current page, and let the spacer under the audio section
	// take what is left.
	const bool pack = tall && !fullEdit();
	QWidget *page = stack_->currentWidget();
	if (pack && page) {
		const int h = std::max({page->sizeHint().height(), page->minimumSizeHint().height(), page->minimumHeight()});
		stack_->setFixedHeight(h);
	} else {
		stack_->setMinimumHeight(0);
		stack_->setMaximumHeight(QWIDGETSIZE_MAX);
	}
	bottomFill_->changeSize(0, 0, QSizePolicy::Minimum, pack ? QSizePolicy::Expanding : QSizePolicy::Fixed);
	bottomLayout_->invalidate();
}

void VideoEditorWindow::setEditorLayout(EditorLayout l)
{
	if (layoutCombo_) {
		const QSignalBlocker block(layoutCombo_);
		layoutCombo_->setCurrentIndex(std::max(0, layoutCombo_->findData(int(l))));
	}
	if (!hsplit_ || !vsplit_ || !topPane_ || !topLayout_ || !bottomPane_ || !barHost_ || !rootLayout_ ||
	    l == editorLayout_)
		return;
	editorLayout_ = l;
	setUpdatesEnabled(false);
	if (l == EditorLayout::Vertical) {
		// The toolbar goes across the top of the window: a preview column is
		// too narrow to hold it, and it belongs to all three columns anyway.
		topLayout_->removeWidget(barHost_);
		rootLayout_->insertWidget(0, barHost_);
		// [preview/editing split, Inspector] -> [(empty, hidden) split,
		// editing area, Inspector, preview].
		hsplit_->insertWidget(1, bottomPane_);
		hsplit_->addWidget(topPane_);
		vsplit_->hide();
		hsplit_->setStretchFactor(hsplit_->indexOf(bottomPane_), 5);
		hsplit_->setStretchFactor(hsplit_->indexOf(topPane_), 2);
		// A tall column is narrow: let the preview go below the minimum width
		// a wide preview keeps (it still keeps its minimum height).
		canvas_->setMinimumWidth(std::min(canvas_->layoutParams().minW, 180));
	} else {
		vsplit_->insertWidget(0, topPane_);
		vsplit_->insertWidget(1, bottomPane_);
		rootLayout_->removeWidget(barHost_);
		topLayout_->addWidget(barHost_);
		vsplit_->show();
		canvas_->setLayoutParams(canvas_->layoutParams()); // its own minimum size again
	}
	adaptEditingArea();
	setUpdatesEnabled(true);
	// Sized once the splitters have laid the new arrangement out.
	QTimer::singleShot(0, this, [this]() {
		if (editorLayout_ == EditorLayout::Vertical)
			sizeVerticalLayout();
		else
			applyModeSplit();
	});
}

void VideoEditorWindow::sizeVerticalLayout()
{
	if (editorLayout_ != EditorLayout::Vertical || !hsplit_)
		return;
	QList<int> sizes = hsplit_->sizes();
	const int edit = hsplit_->indexOf(bottomPane_), ins = hsplit_->indexOf(inspector_),
		  pre = hsplit_->indexOf(topPane_);
	if (edit < 0 || ins < 0 || pre < 0 || pre >= sizes.size())
		return;
	int total = 0;
	for (int w : sizes)
		total += w;
	if (total <= 0)
		return;
	const int insW = inspector_->isVisible() ? sizes[ins] : 0;
	// Where it was last dragged to in this mode; else as wide as the picture
	// needs at the column's full height, so a 9:16 video fills it top to bottom.
	int want = QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"))
			   .value(QStringLiteral("editor/verticalPreviewW"), 0)
			   .toInt();
	if (want <= 0) {
		QSize pic = fullEdit() ? timelineCanvasSize() : multiCut() ? multiCutCanvasSize() : QSize();
		if (!pic.isValid() || pic.isEmpty()) {
			const EditorSource *s = activeSource();
			pic = s ? QSize(s->width, s->height) : QSize(9, 16);
		}
		if (pic.isEmpty())
			pic = QSize(9, 16);
		want = int(std::lround(double(topPane_->height()) * pic.width() / pic.height()));
	}
	const int floor = std::max(canvas_->minimumWidth(), 120);
	want = std::clamp(want, floor, std::max(floor, (total - insW) / 2));
	sizes[pre] = want;
	sizes[edit] = std::max(0, total - insW - want);
	sizes[ins] = insW;
	for (int i = 0; i < sizes.size(); ++i)
		if (i != pre && i != edit && i != ins)
			sizes[i] = 0; // the empty, hidden preview/editing split
	hsplit_->setSizes(sizes);
}

void VideoEditorWindow::sendToFullEditing()
{
	if (!timelineView_)
		return;

	// Append, never overwrite: sending twice piles clips up, which is visible
	// and undoable, whereas sending ON TOP of Full-editing work is neither.
	const qint64 at = timelineView_->model().contentDurationMs();
	QVector<TlClip> clips;
	if (multiCut()) {
		clips = clipsFromSegments(tracks_->segments(), at);
	} else {
		// Trim: one clip, carrying its in/out, speed and crop. The active
		// source is what Trim was showing.
		const EditorSource *src = activeSource();
		clips = clipsFromTrim(src ? src->id : 0, timeline_->start(), timeline_->end(), speed_,
				      canvas_->cropEnabled(), canvas_->cropRectVideo(), at);
	}

	if (clips.isEmpty()) {
		QMessageBox::information(
			this, QStringLiteral("Send to Full Editing"),
			multiCut() ? QStringLiteral("There are no cuts to send yet.")
				   : QStringLiteral("The trim range is empty — set an in and out point "
						    "first."));
		return;
	}

	for (const TlClip &c : clips)
		timelineView_->addClip(TlTrack::Kind::Video, c);

	// One undo entry for the whole send, not one per clip: it was one action.
	commitSnapshot();
	setEditMode(EditMode::Full);

	const qint64 ms = sentDurationMs(clips);
	if (infoLabel_)
		infoLabel_->setText(QStringLiteral("Sent %1 clip%2 (%3 s) to Full Editing%4")
					    .arg(clips.size())
					    .arg(clips.size() == 1 ? "" : "s")
					    .arg(ms / 1000.0, 0, 'f', 1)
					    .arg(at > 0 ? QStringLiteral(", after the existing clips")
							: QString()));
}

void VideoEditorWindow::setEditMode(EditMode m)
{
	stopPlayback();
	if (previewTimer_)
		previewTimer_->stop();
	pendingMs_ = -1;
	setHoverInspect(-1);
	// The strip describes the timeline you were on; leaving it makes it a lie.
	if (overview_)
		overview_->hideNow();
	const bool wasFull = fullEdit();
	trimModeBtn_->setChecked(m == EditMode::Trim);
	cutModeBtn_->setChecked(m == EditMode::MultiCut);
	fullModeBtn_->setChecked(m == EditMode::Full);
	stack_->setCurrentIndex(int(m));
	if (multiCutSourceRow_)
		multiCutSourceRow_->setVisible(m == EditMode::MultiCut);
	if (m == EditMode::MultiCut && multiCutSourceCombo_ && multiCutSourceCombo_->currentIndex() < 0 &&
	    multiCutSourceCombo_->count() > 0)
		setActiveSource(multiCutSourceCombo_->itemData(0).toInt());
	// The layout is the same in every mode, but in Vertical the editing area
	// fits the mode just entered.
	adaptEditingArea();
	if (shortcuts_)
		shortcuts_->refreshEnabled(); // mode-gated commands (Up / Down in Multi-Cut)
	// Nothing to send FROM Full editing -- it is the destination.
	if (sendToFullBtn_)
		sendToFullBtn_->setVisible(m != EditMode::Full);

	// The speed control and the separate Voiceover foldout only apply to Trim /
	// Multi-Cut; Full editing puts audio on its own timeline tracks. Only touch
	// the audio foldout when crossing into/out of Full, so Trim<->Multi-Cut keeps
	// the user's collapse state.
	const bool full = (m == EditMode::Full);
	if (full && !wasFull) {
		if (audioBody_)
			audioExpandedBeforeFull_ = audioBody_->isVisible();
		if (audioHeader_)
			audioHeader_->setVisible(false);
		if (audioBody_)
			audioBody_->setVisible(false);
	} else if (!full && wasFull) {
		if (audioHeader_)
			audioHeader_->setVisible(true);
		if (audioBody_)
			audioBody_->setVisible(audioExpandedBeforeFull_);
	}
	speedSlider_->setEnabled(valid_ && !full);
	speedSpin_->setEnabled(valid_ && !full);
	// Hidden rather than disabled in Full editing: a dead slider reads as broken,
	// and per-clip speed lives in the Inspector there.
	for (QWidget *w : {static_cast<QWidget *>(speedCaption_), static_cast<QWidget *>(speedSlider_),
			   static_cast<QWidget *>(speedSpin_), static_cast<QWidget *>(speedLabel_)})
		if (w)
			w->setVisible(!full);

	if (m == EditMode::MultiCut) {
		playBtn_->setToolTip(QStringLiteral("Loop-play the assembled output"));
		onSegmentSelected(tracks_->selectedIndex()); // rebind the speed slider
	} else if (m == EditMode::Full) {
		playBtn_->setToolTip(QStringLiteral("Play the timeline"));
		speedLabel_->setText(QString());
		// First time in, seed the timeline with the active source so there's
		// something to edit (like the other modes start on the whole clip).
		if (!timelineSeeded_ && timelineView_ && timelineView_->model().isEmpty()) {
			EditorSource *s = activeSource();
			if (s && s->durationMs > 0) {
				TlClip c;
				c.sourceId = s->id;
				c.srcStartMs = 0;
				c.srcEndMs = s->durationMs;
				c.outStartMs = 0;
				timelineView_->addClip(TlTrack::Kind::Video, c);
			}
			timelineSeeded_ = true;
		}
	} else {
		playBtn_->setToolTip(
			QStringLiteral("Loop-play the trimmed section at the current speed"));
		syncSpeedControls(speed_);
		speedLabel_->setText(QString()); // no per-cut count in Simple Trim
	}

	// Crop is a Trim/Multi-Cut tool; Full editing uses per-clip zoom/position
	// instead (driven from the preview with the mouse).
	if (resetCropBtn_)
		resetCropBtn_->setVisible(!full); // followed the Crop toggle, which already hid
	if (cropToggle_) {
		cropToggle_->setVisible(!full);
		if (full && cropToggle_->isChecked()) {
			// Off while in Full editing, but REMEMBERED: it used to be
			// dropped here, so a project reopened in Full editing and saved
			// lost its Trim crop for good.
			trimCropWanted_ = true;
			QSignalBlocker b(cropToggle_);
			cropToggle_->setChecked(false);
			canvas_->setCropEnabled(false);
		} else if (!full && trimCropWanted_) {
			trimCropWanted_ = false;
			QSignalBlocker b(cropToggle_);
			cropToggle_->setChecked(true);
			canvas_->setCropEnabled(true);
		}
	}
	if (addMenuBtn_)
		addMenuBtn_->setVisible(full); // the Add menu: text, subtitles, audio, image, effect clip
	if (addLibraryAct_)
		addLibraryAct_->setVisible(full);
	for (QAction *a : fullOnlyActs_)
		a->setEnabled(full); // greyed, not hidden: the menus keep their shape
	const bool canShuffle = full || m == EditMode::MultiCut;
	if (randomizeAct_)
		randomizeAct_->setEnabled(canShuffle);
	if (snapBtn_)
		snapBtn_->setVisible(full);
	if (fitBtn_)
		fitBtn_->setVisible(full);
	if (randomBtn_)
		randomBtn_->setVisible(canShuffle);
	if (!canShuffle && randomPanel_)
		randomPanel_->hide(); // its target just went away
	if (consoleBtn_)
		consoleBtn_->setVisible(full && EditConsole::available());
	if (!full && consolePanel_)
		consolePanel_->hide();
	if (muteBtn_)
		muteBtn_->setVisible(full); // only Full editing has a timeline to mix
	if (!full && audioPreview_)
		audioPreview_->stop();
	if (full)
		schedulePremix(); // edits made in another mode left the mix behind
	syncPreviewTransformTarget();
	const QSize previewSize = previewCanvasSize();
	canvas_->setVideoSize(previewSize.width(), previewSize.height());
	showPlayheadFrame(); // the picture must describe the mode just entered

	// Deferred: during construction, and immediately after a mode switch, the
	// splitter has not laid out yet and its sizes() mean nothing.
	QTimer::singleShot(0, this, [this]() { applyModeSplit(); });

	updateInfoLabel();
	updateInspector();
	updateVoiceoverAxis();
}

void VideoEditorWindow::syncSpeedControls(double value)
{
	QSignalBlocker bs(speedSlider_);
	QSignalBlocker bp(speedSpin_);
	speedSlider_->setValue(speedToSlider(value));
	speedSpin_->setValue(value);
}

void VideoEditorWindow::updateInfoLabel()
{
	if (!valid_)
		return;
	QString text = baseInfo_;
	if (multiCut()) {
		text += QStringLiteral("   ·   %1 cut%2 → %3s")
				.arg(tracks_->segments().size())
				.arg(tracks_->segments().size() == 1 ? QString() : QStringLiteral("s"))
				.arg(tracks_->totalOutputMs() / 1000.0, 0, 'f', 1);
	}
	infoLabel_->setText(text);
}

void VideoEditorWindow::revealInspector()
{
	// Open the side panel (the Inspector/Effects buttons mirror each other) and
	// show the current selection's properties.
	if (inspectorBtn_ && !inspectorBtn_->isChecked())
		inspectorBtn_->setChecked(true);
	else if (inspector_)
		inspector_->setVisible(true);
	updateInspector();
	refreshProjectInspector();
}

void VideoEditorWindow::updateInspector()
{
	syncClipInspector(); // Full-editing per-clip controls (hidden in other modes)
	// The Spotlight panel's subject depends on what is selected, so it has to
	// re-point on every selection change, not only when it is itself edited.
	syncSpotlightInspector();
	if (!inspector_)
		return;
	auto setRange = [this](qint64 inMs, qint64 outMs, double sp, qint64 outLen) {
		inspInMs_->setText(previewTimeText(inMs));
		inspOutMs_->setText(previewTimeText(outMs));
		inspSrcLen_->setText(QStringLiteral("%1s").arg((outMs - inMs) / 1000.0, 0, 'f', 2));
		inspSpeed_->setText(QStringLiteral("%1×").arg(sp, 0, 'f', 2));
		inspOutLen_->setText(QStringLiteral("%1s").arg(outLen / 1000.0, 0, 'f', 2));
	};
	if (!valid_) {
		inspTitle_->setText(QStringLiteral("No clip loaded"));
		inspInMs_->setText(QStringLiteral("—"));
		inspOutMs_->setText(QStringLiteral("—"));
		inspSrcLen_->setText(QStringLiteral("—"));
		inspSpeed_->setText(QStringLiteral("—"));
		inspOutLen_->setText(QStringLiteral("—"));
		inspHint_->clear();
		return;
	}
	if (multiCut()) {
		const int idx = tracks_->selectedIndex();
		const auto &segs = tracks_->segments();
		if (idx >= 0 && idx < segs.size()) {
			const CutSegment &s = segs[idx];
			const int n = tracks_->selectedIndices().size();
			inspTitle_->setText(n > 1
				? QStringLiteral("Cut %1 of %2 selected  ·  %3 total")
					  .arg(idx + 1).arg(segs.size()).arg(n)
				: QStringLiteral("Cut %1 of %2").arg(idx + 1).arg(segs.size()));
			setRange(s.srcStartMs, s.srcEndMs, s.speed, s.outDurationMs());
			inspHint_->setText(n > 1
				? QStringLiteral("The speed control applies to all %1 selected cuts.").arg(n)
				: QString());
		} else {
			inspTitle_->setText(QStringLiteral("No cut selected"));
			inspInMs_->setText(QStringLiteral("—"));
			inspOutMs_->setText(QStringLiteral("—"));
			inspSrcLen_->setText(QStringLiteral("—"));
			inspSpeed_->setText(QStringLiteral("—"));
			inspOutLen_->setText(QStringLiteral("—"));
			inspHint_->setText(QStringLiteral(
				"Click a cut on the Output track to see and edit its properties."));
		}
	} else if (fullEdit()) {
		// Full editing: the SELECTED CLIP's own range. This branch did not
		// exist -- full editing fell through to the Trim timeline's start and
		// end below, which belong to a different mode's widget and describe a
		// different thing, so these five rows were reporting numbers that had
		// nothing to do with the clip on screen.
		const TlClip *c = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
		if (!c) {
			inspTitle_->clear();
			inspHint_->clear();
		} else if (c->freeDuration()) {
			// A caption, a still or an effect has no source timeline: its
			// length is whatever it was dragged to. Dashes rather than zeroes,
			// which would read as "starts at 0, ends at 0".
			inspTitle_->setText(clipLabel(*c));
			inspInMs_->setText(QStringLiteral("—"));
			inspOutMs_->setText(QStringLiteral("—"));
			inspSrcLen_->setText(QStringLiteral("—"));
			inspSpeed_->setText(QStringLiteral("—"));
			inspOutLen_->setText(
				QStringLiteral("%1s").arg(c->outDurationMs() / 1000.0, 0, 'f', 2));
			inspHint_->setText(
				QStringLiteral("No source file — its length is whatever you drag it to."));
		} else {
			inspTitle_->setText(clipLabel(*c));
			setRange(c->srcStartMs, c->srcEndMs, c->speed, c->outDurationMs());
			inspHint_->clear();
		}
		const bool have = c != nullptr;
		if (srcForm_)
			srcForm_->setVisible(have);
		if (srcEmpty_)
			srcEmpty_->setVisible(!have);
		return;
	} else {
		// Simple Trim: the single kept range at the global speed.
		const qint64 in = timeline_->start();
		const qint64 out = timeline_->end();
		const double sp = speed_ > 0.01 ? speed_ : 1.0;
		const qint64 outLen = std::max<qint64>(1, qint64((out - in) / sp));
		inspTitle_->setText(QStringLiteral("Trim range"));
		setRange(in, out, speed_, outLen);
		inspHint_->clear();
	}
	// Multi-Cut with nothing selected is the other empty case; Simple Trim
	// always has a range, because the range IS the edit.
	{
		const bool have = !multiCut() || (tracks_ && tracks_->selectedIndex() >= 0);
		if (srcForm_)
			srcForm_->setVisible(have);
		if (srcEmpty_)
			srcEmpty_->setVisible(!have);
	}
}

void VideoEditorWindow::onSegmentsChanged()
{
	stopPlayback();
	playSeg_ = -1;
	updateInfoLabel();
	updateInspector();
	updateVoiceoverAxis();
}

void VideoEditorWindow::onSegmentSelected(int index)
{
	if (!multiCut())
		return;
	if (index >= 0 && index < tracks_->segments().size()) {
		// Sync the Source track to the selected cut's video so you see (and can
		// cut more from) the source that clip came from.
		const int sid = tracks_->segments()[index].sourceId;
		if (sid != activeSourceId_ && sourceById(sid))
			setActiveSource(sid);
		speedSlider_->setEnabled(true);
		speedSpin_->setEnabled(true);
		const double sp = tracks_->segments()[index].speed;
		syncSpeedControls(sp);
		const int n = tracks_->selectedIndices().size();
		speedLabel_->setText(n > 1 ? QStringLiteral("(%1 cuts)").arg(n) : QString());
	} else {
		// No clip selected — nothing to edit.
		speedSlider_->setEnabled(false);
		speedSpin_->setEnabled(false);
		speedLabel_->setText(QStringLiteral("—"));
	}
	updateInspector();
}

VideoEditorWindow::~VideoEditorWindow()
{
	delete mouseHistoryFilter_;
	if (isVisible())
		window_screen::save(*this, QStringLiteral("editor"));
	--openCount_;
	stopPlayback();
	if (voRecorder_ && voRecorder_->isRecording())
		voRecorder_->stop();
	if (sceneThread_.joinable()) {
		sceneCancel_.store(true);
		sceneThread_.join();
	}
	// The mixing worker captures `this` for its completion callback, so it must
	// be finished before the window goes away.
	if (audioMixThread_.joinable()) {
		audioMixCancel_.store(true);
		audioMixThread_.join();
	}
	joinExport();
	// Voiceover takes are session-only — clear the temp dir on close.
	if (!voTempDir_.isEmpty())
		QDir(voTempDir_).removeRecursively();
}

qint64 VideoEditorWindow::outputDurationMs() const
{
	if (!valid_)
		return 0;
	if (fullEdit())
		return timelineView_->durationMs();
	if (multiCut())
		return tracks_->totalOutputMs();
	const double sp = speed_ > 0.01 ? speed_ : 1.0;
	return std::max<qint64>(1, qint64((timeline_->end() - timeline_->start()) / sp));
}

qint64 VideoEditorWindow::currentOutputMs() const
{
	if (!playing_)
		return 0;
	if (fullEdit())
		return std::clamp<qint64>(playAnchorMs_ + playClock_.elapsed(), 0,
					  std::max<qint64>(0, timelineView_->durationMs()));
	if (multiCut())
		return std::clamp<qint64>(playAnchorMs_ + playClock_.elapsed(), 0,
					  std::max<qint64>(0, tracks_->totalOutputMs()));
	const qint64 src = playAnchorMs_ + qint64(playClock_.elapsed() * speed_);
	const double sp = speed_ > 0.01 ? speed_ : 1.0;
	return std::max<qint64>(0, qint64((src - timeline_->start()) / sp));
}

void VideoEditorWindow::updateVoiceoverAxis()
{
	if (voTrack_)
		voTrack_->setOutputDuration(outputDurationMs());
}

QString VideoEditorWindow::voiceoverTempDir()
{
	if (voTempDir_.isEmpty()) {
		const QString base =
			QDir::tempPath() + QStringLiteral("/harpia_voiceover_") +
			QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss"));
		QDir().mkpath(base);
		voTempDir_ = base;
	}
	return voTempDir_;
}

void VideoEditorWindow::onVoiceoverRecordClicked()
{
	// A running countdown: cancel it.
	if (voCountdownTimer_->isActive()) {
		voCountdownTimer_->stop();
		voStatus_->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
		voStatus_->setText(QStringLiteral("Cancelled."));
		voRecordBtn_->setText(QStringLiteral("Record"));
		voRecordBtn_->setIcon(uiIcon(Glyph::Record, 12, QColor(0xe5, 0x48, 0x4d)));
		return;
	}
	if (voRecording_) {
		finishVoiceover();
		return;
	}
	if (!valid_)
		return;
	saveVoiceoverSettings();
	const int secs = voCountdownSec_ ? voCountdownSec_->value() : 0;
	if (secs > 0) {
		voCountdownLeft_ = secs;
		voStatus_->setStyleSheet(QStringLiteral("color:#f2c94c; font-size:18px; font-weight:bold;"));
		voStatus_->setText(QStringLiteral("Starting in %1…").arg(voCountdownLeft_));
		voRecordBtn_->setText(QStringLiteral("Cancel"));
		voRecordBtn_->setIcon(QIcon());
		voCountdownTimer_->start();
	} else {
		startVoiceoverCapture();
	}
}

int VideoEditorWindow::liveTakeTrack(const TimelineModel &m) const
{
	if (voLiveTrackName_.isEmpty())
		return -1;
	for (int i = 0; i < m.tracks.size(); ++i)
		if (m.tracks[i].kind == TlTrack::Kind::Audio && m.tracks[i].name == voLiveTrackName_)
			return i;
	return -1;
}

void VideoEditorWindow::beginLiveTake()
{
	voLivePeaks_.clear();
	voLiveMax_ = 0.f;
	voLiveBucketEnd_ = 0;
	voLiveTick_.invalidate();
	if (fullEdit() && timelineView_) {
		// A new audio track for this take, at the bottom of the audio lanes
		// (above the Sounds lane). Made quietly: the finished take commits the
		// track and the clip together, so there is no half-made undo step.
		TimelineModel m = timelineView_->model();
		QString name = QStringLiteral("Voiceover");
		for (int n = 2;; ++n) {
			bool taken = false;
			for (const TlTrack &t : m.tracks)
				if (t.name == name)
					taken = true;
			if (!taken)
				break;
			name = QStringLiteral("Voiceover %1").arg(n);
		}
		TlTrack t;
		t.kind = TlTrack::Kind::Audio;
		t.name = name;
		t.color = QColor(0xe5, 0x48, 0x4d);
		TlClip live;
		live.type = TlClip::Type::Video; // a media clip; the lane makes it audio
		live.sourceId = 0;               // no file yet: this is the live preview
		live.srcEndMs = 1;
		live.outStartMs = voClipStartMs_;
		live.transition.enabled = false;
		t.clips.append(live);
		const int at = m.soundsLane() >= 0 ? m.soundsLane() : m.tracks.size();
		m.tracks.insert(at, t);
		voLiveTrackName_ = name;
		timelineView_->setModelQuiet(m);
		return;
	}
	if (voTrack_) {
		voClipsBefore_ = voTrack_->clips();
		updateLiveTake();
	}
}

void VideoEditorWindow::updateLiveTake()
{
	const qint64 dur = std::max<qint64>(1, voRecorder_ ? voRecorder_->capturedMs() : 1);
	if (fullEdit() && timelineView_) {
		TimelineModel m = timelineView_->model();
		const int ti = liveTakeTrack(m);
		if (ti < 0)
			return;
		for (TlClip &c : m.tracks[ti].clips)
			if (c.sourceId == 0) {
				c.srcEndMs = dur;
				c.fadeInMs = c.fadeOutMs = 0;
				c.peaks = voLivePeaks_;
			}
		timelineView_->setModelQuiet(m);
		return;
	}
	if (voTrack_) {
		QVector<VoiceoverClip> list = voClipsBefore_;
		VoiceoverClip live;
		live.outStartMs = voClipStartMs_;
		live.durationMs = dur;
		live.srcTotalMs = dur;
		live.peaks = voLivePeaks_;
		list.append(live);
		voTrack_->setClips(list);
	}
}

qint64 VideoEditorWindow::voiceoverStartMs() const
{
	if (voStartAt_ && voStartAt_->currentIndex() == 1)
		return 0; // "Start of the timeline"
	// The playhead where it sits NOW, not where playback is: recording stops
	// playback first, and the old code read the playing position after that,
	// which is 0 -- so every take landed at the very start.
	if (fullEdit() && timelineView_) {
		const qint64 total = timelineView_->durationMs();
		const qint64 ph = std::max<qint64>(0, timelineView_->playhead());
		return (total > 0 && ph >= total) ? total : ph;
	}
	if (multiCut() && tracks_)
		return std::max<qint64>(0, tracks_->playhead());
	return 0;
}

void VideoEditorWindow::startVoiceoverCapture()
{
	stopPlayback();
	voRecorder_->setDeviceId(voDevice_->currentData().toString());
	voRecorder_->setChannelCount(voChannels_ && voChannels_->currentIndex() == 1 ? 2 : 1);
	voRecorder_->setSampleRate(voRate_ ? voRate_->currentData().toInt() : 48000);
	const qint64 startMs = voiceoverStartMs();
	if (!voRecorder_->start(voiceoverTempDir())) {
		voStatus_->clear();
		voRecordBtn_->setText(QStringLiteral("Record"));
		voRecordBtn_->setIcon(uiIcon(Glyph::Record, 12, QColor(0xe5, 0x48, 0x4d)));
		return;
	}
	voRecording_ = true;
	voClipStartMs_ = startMs;
	beginLiveTake();
	if (voOpenBtn_) {
		voOpenBtn_->setText(QStringLiteral("Recording…"));
		voOpenBtn_->setStyleSheet(QStringLiteral("QPushButton{background:#7a1f22;color:#ffffff;}"));
	}
	voRecordBtn_->setText(QStringLiteral("Stop"));
	voRecordBtn_->setIcon(uiIcon(Glyph::Stop, 12));
	voStatus_->setStyleSheet(QStringLiteral("color:#e5484d; font-weight:bold;"));
	voStatus_->setText(QStringLiteral("● Recording"));
	if (voTrack_ && !fullEdit())
		voTrack_->setPlayhead(voClipStartMs_);
	// Talk-along: play from where the take starts, so what you see is what
	// the narration will sit under.
	voPlayedAlong_ = false;
	voMutedPreview_ = false;
	if (voTalkAlong_->isChecked()) {
		if (fullEdit() && timelineView_)
			timelineView_->setPlayhead(startMs);
		else if (multiCut() && tracks_)
			tracks_->setPlayhead(startMs);
		// The timeline's own sound would be picked up by the microphone
		// unless you wear headphones, so it is off unless asked for. And
		// recording all of Windows' sound where Harpia cannot be left out of
		// it (Windows 10), it would go straight into the take: always off.
		const bool hearsItself = voRecorder_->isSystemSource() && !voRecorder_->systemExcludesSelf();
		if (audioPreview_ && (hearsItself || !(voTimelineSound_ && voTimelineSound_->isChecked())) &&
		    !audioPreview_->isMuted()) {
			audioPreview_->setMuted(true);
			voMutedPreview_ = true;
		}
		startPlayback();
		voPlayedAlong_ = playing_;
	}
}

void VideoEditorWindow::finishVoiceover()
{
	if (!voRecording_)
		return;
	voRecording_ = false;
	if (voOpenBtn_) {
		voOpenBtn_->setText(QStringLiteral("Voiceover"));
		voOpenBtn_->setStyleSheet(QString());
	}
	QString path = voRecorder_->stop();
	qint64 durMs = voRecorder_->capturedMs();
	const bool playedAlong = voPlayedAlong_;
	stopPlayback();
	if (voMutedPreview_ && audioPreview_)
		audioPreview_->setMuted(false);
	voMutedPreview_ = false;
	voPlayedAlong_ = false;
	// A grab from Windows: off with the wait before the video started and
	// after it ended. The sound then starts where the take is placed -- unless
	// the timeline played along, where it keeps its place in time instead.
	const bool sysTake = voRecorder_->isSystemSource();
	bool heardNothing = false;
	if (sysTake && !path.isEmpty() && durMs > 0 && voTrimSilence_ && voTrimSilence_->isChecked()) {
		const WavTrim t = trimWavSilence(path);
		if (t.ok && t.silent) {
			QFile::remove(path);
			path.clear();
			durMs = 0;
			heardNothing = true;
		} else if (t.ok) {
			durMs = t.durationMs;
			if (playedAlong)
				voClipStartMs_ += t.leadMs;
		}
	}
	voMeter_->reset();
	voStatus_->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	voRecordBtn_->setText(QStringLiteral("Record"));
	voRecordBtn_->setIcon(uiIcon(Glyph::Record, 12, QColor(0xe5, 0x48, 0x4d)));
	if (path.isEmpty() || durMs <= 0) {
		voStatus_->setText(heardNothing ? QStringLiteral("Nothing was heard. Was the video playing, and not muted?")
				   : sysTake    ? QStringLiteral("Nothing was captured from Windows audio.")
						: QStringLiteral("Nothing was captured. Check the microphone above."));
		if (fullEdit() && timelineView_) {
			TimelineModel m = timelineView_->model();
			const int ti = liveTakeTrack(m);
			if (ti >= 0) {
				m.tracks.remove(ti); // it held only the live clip
				timelineView_->setModelQuiet(m);
			}
			voLiveTrackName_.clear();
		} else if (voTrack_) {
			voTrack_->setClips(voClipsBefore_);
		}
		return;
	}
	voStatus_->setText(QStringLiteral("Take added: %1 at %2.")
				   .arg(timeTextTenths(durMs), timeTextTenths(voClipStartMs_)));
	// In Full editing the take goes on the track made for it when recording
	// started, replacing the live clip: the track and the take arrive as one
	// undo step.
	if (fullEdit() && timelineView_) {
		TimelineModel m = timelineView_->model();
		const int ti = liveTakeTrack(m);
		const int id = addAudioSource(path);
		if (ti >= 0) {
			TlTrack &t = m.tracks[ti];
			for (int i = t.clips.size() - 1; i >= 0; --i)
				if (t.clips[i].sourceId == 0)
					t.clips.remove(i);
			if (id >= 0) {
				TlClip c;
				c.sourceId = id;
				c.srcStartMs = 0;
				c.srcEndMs = durMs;
				c.outStartMs = voClipStartMs_;
				c.peaks = VoiceoverTrack::loadPeaks(path, 600);
				t.clips.append(c);
			} else if (t.clips.isEmpty()) {
				m.tracks.remove(ti);
			}
			timelineView_->setModelAndCommit(m);
		} else if (id >= 0) {
			// The live track was deleted mid-take: a free lane, as before.
			TlClip c;
			c.sourceId = id;
			c.srcEndMs = durMs;
			c.outStartMs = voClipStartMs_;
			c.peaks = VoiceoverTrack::loadPeaks(path, 600);
			timelineView_->addClipOnFreeLane(TlTrack::Kind::Audio, c);
		}
		voLiveTrackName_.clear();
		updateInfoLabel();
		return;
	}
	voTrack_->setClips(voClipsBefore_); // the live clip goes; the real one follows
	VoiceoverClip clip;
	clip.path = path;
	clip.outStartMs = voClipStartMs_;
	clip.durationMs = durMs;
	clip.srcTotalMs = durMs;
	voTrack_->addClip(clip);
}

void VideoEditorWindow::refreshVoiceoverDevices()
{
	if (!voDevice_)
		return;
	QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	const QString want = voDevice_->count() ? voDevice_->currentData().toString()
						: st.value(QStringLiteral("editor/vo/device")).toString();
	QSignalBlocker b(voDevice_);
	voDevice_->clear();
	int def = 0;
	for (const AudioInputDevice &d : AudioRecorder::inputDevices()) {
		voDevice_->addItem(d.isDefault ? QStringLiteral("%1  (default)").arg(d.name) : d.name, d.id);
		if (d.isDefault)
			def = voDevice_->count() - 1;
	}
	const bool mics = voDevice_->count() > 0;
	if (!mics) {
		voDevice_->addItem(QStringLiteral("No microphone found"), QString());
		if (auto *model = qobject_cast<QStandardItemModel *>(voDevice_->model()))
			model->item(0)->setEnabled(!system_audio::available());
	}
	// The computer's own sound, after the microphones: everything it plays,
	// and -- where Windows can single one out -- each app with sound open.
	if (system_audio::available()) {
		voDevice_->insertSeparator(voDevice_->count());
		const int everything = voDevice_->count();
		voDevice_->addItem(QStringLiteral("Windows audio (everything)"), system_audio::sourceId());
		if (!mics)
			def = everything;
		if (system_audio::perAppAvailable()) {
			QStringList listed;
			for (const SystemAudioApp &a : system_audio::apps()) {
				voDevice_->addItem(a.playing ? QStringLiteral("Only %1  (playing)").arg(a.name)
							     : QStringLiteral("Only %1").arg(a.name),
						   system_audio::sourceId(a.exe));
				listed << a.exe.toLower();
			}
			// The app chosen last time, even with nothing playing in it now.
			QString wantExe;
			if (system_audio::isSourceId(want, &wantExe) && !wantExe.isEmpty() &&
			    !listed.contains(wantExe.toLower()))
				voDevice_->addItem(QStringLiteral("Only %1  (not playing now)").arg(wantExe), want);
		}
	}
	voDevice_->setEnabled(mics || system_audio::available());
	const int i = voDevice_->findData(want);
	voDevice_->setCurrentIndex(i >= 0 && !want.isEmpty() ? i : def);
	syncVoiceoverSource();
}

void VideoEditorWindow::saveVoiceoverSettings()
{
	QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	if (voDevice_)
		st.setValue(QStringLiteral("editor/vo/device"), voDevice_->currentData().toString());
	if (voCountdownSec_)
		st.setValue(QStringLiteral("editor/vo/countdown"), voCountdownSec_->value());
	if (voStartAt_)
		st.setValue(QStringLiteral("editor/vo/startAt"), voStartAt_->currentIndex());
	if (voChannels_)
		st.setValue(QStringLiteral("editor/vo/channels"), voChannels_->currentIndex());
	if (voRate_)
		st.setValue(QStringLiteral("editor/vo/rate"), voRate_->currentData().toInt());
	if (voTalkAlong_)
		st.setValue(QStringLiteral("editor/vo/playAlong"), voTalkAlong_->isChecked());
	if (voTimelineSound_)
		st.setValue(QStringLiteral("editor/vo/timelineSound"), voTimelineSound_->isChecked());
	if (voStopAtEnd_)
		st.setValue(QStringLiteral("editor/vo/stopAtEnd"), voStopAtEnd_->isChecked());
	if (voTrimSilence_)
		st.setValue(QStringLiteral("editor/vo/trimSystem"), voTrimSilence_->isChecked());
}

void VideoEditorWindow::syncVoiceoverSource()
{
	if (!voDevice_)
		return;
	const bool sys = system_audio::isSourceId(voDevice_->currentData().toString());
	// Windows audio is taken as Windows plays it: stereo, at its own rate.
	const QString asPlayed = QStringLiteral("Windows audio is recorded in stereo, at the rate Windows plays it.");
	if (voChannels_) {
		voChannels_->setEnabled(!sys);
		voChannels_->setToolTip(sys ? asPlayed : QStringLiteral("Mono suits a single voice and a single microphone."));
	}
	if (voRate_) {
		voRate_->setEnabled(!sys);
		voRate_->setToolTip(sys ? asPlayed : QString());
	}
	if (voTrimSilence_)
		voTrimSilence_->setVisible(sys);
}

void VideoEditorWindow::buildVoiceoverPanel()
{
	voPanel_ = new QDialog(this, Qt::Tool);
	voPanel_->setWindowTitle(QStringLiteral("Voiceover"));
	voPanel_->setModal(false);
	auto *v = new QVBoxLayout(voPanel_);
	v->setContentsMargins(14, 12, 14, 12);
	v->setSpacing(8);
	QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));

	auto *form = new QFormLayout;
	form->setHorizontalSpacing(10);
	form->setVerticalSpacing(6);

	auto *devRow = new QHBoxLayout;
	voDevice_ = new QComboBox(voPanel_);
	voDevice_->setMinimumWidth(220);
	voDevice_->setToolTip(system_audio::available()
				      ? QStringLiteral("A microphone, or the sound Windows is playing: play a video and "
						       "grab its sound onto the timeline.")
				      : QStringLiteral("The microphone to record from."));
	devRow->addWidget(voDevice_, 1);
	auto *refresh = new QPushButton(QStringLiteral("↻"), voPanel_);
	refresh->setFixedWidth(28);
	refresh->setToolTip(system_audio::available()
				    ? QStringLiteral("Look again for microphones, and for apps playing sound.")
				    : QStringLiteral("Look for microphones again (after plugging one in)."));
	connect(refresh, &QPushButton::clicked, this, &VideoEditorWindow::refreshVoiceoverDevices);
	devRow->addWidget(refresh);
	form->addRow(QStringLiteral("Record from"), devRow);
	refreshVoiceoverDevices();

	voCountdownSec_ = new QSpinBox(voPanel_);
	voCountdownSec_->setRange(0, 10);
	voCountdownSec_->setSuffix(QStringLiteral(" s"));
	voCountdownSec_->setSpecialValueText(QStringLiteral("None: start at once"));
	voCountdownSec_->setValue(st.value(QStringLiteral("editor/vo/countdown"), 3).toInt());
	voCountdownSec_->setToolTip(QStringLiteral("Seconds to count down before capture starts. 0 starts at once."));
	form->addRow(QStringLiteral("Countdown"), voCountdownSec_);

	voStartAt_ = new QComboBox(voPanel_);
	voStartAt_->addItem(QStringLiteral("At the playhead"));
	voStartAt_->addItem(QStringLiteral("At the start of the timeline"));
	voStartAt_->setCurrentIndex(std::clamp(st.value(QStringLiteral("editor/vo/startAt"), 0).toInt(), 0, 1));
	voStartAt_->setToolTip(QStringLiteral("Where the take is placed, and where play-along starts."));
	form->addRow(QStringLiteral("Place take"), voStartAt_);

	voChannels_ = new QComboBox(voPanel_);
	voChannels_->addItem(QStringLiteral("Mono (voice)"));
	voChannels_->addItem(QStringLiteral("Stereo"));
	voChannels_->setCurrentIndex(std::clamp(st.value(QStringLiteral("editor/vo/channels"), 0).toInt(), 0, 1));
	voChannels_->setToolTip(QStringLiteral("Mono suits a single voice and a single microphone."));
	form->addRow(QStringLiteral("Channels"), voChannels_);

	voRate_ = new QComboBox(voPanel_);
	voRate_->addItem(QStringLiteral("48 kHz (video standard)"), 48000);
	voRate_->addItem(QStringLiteral("44.1 kHz"), 44100);
	{
		const int i = voRate_->findData(st.value(QStringLiteral("editor/vo/rate"), 48000).toInt());
		voRate_->setCurrentIndex(i < 0 ? 0 : i);
	}
	form->addRow(QStringLiteral("Sample rate"), voRate_);
	v->addLayout(form);

	voTrimSilence_ = new QCheckBox(QStringLiteral("Trim the silence at both ends"), voPanel_);
	voTrimSilence_->setChecked(st.value(QStringLiteral("editor/vo/trimSystem"), true).toBool());
	voTrimSilence_->setToolTip(QStringLiteral(
		"Cuts the wait while you switch to the video and press play, and after it ends, so the "
		"sound starts right where the take is placed."));
	v->addWidget(voTrimSilence_);

	voTalkAlong_ = new QCheckBox(QStringLiteral("Play the video while recording"), voPanel_);
	voTalkAlong_->setChecked(st.value(QStringLiteral("editor/vo/playAlong"), true).toBool());
	voTalkAlong_->setToolTip(QStringLiteral("Narrate to what you see: playback runs from where the take starts."));
	v->addWidget(voTalkAlong_);
	voTimelineSound_ = new QCheckBox(QStringLiteral("Hear the timeline's sound too (use headphones)"), voPanel_);
	voTimelineSound_->setChecked(st.value(QStringLiteral("editor/vo/timelineSound"), false).toBool());
	voTimelineSound_->setToolTip(QStringLiteral(
		"Off keeps the timeline silent while you record, so the microphone does not pick up "
		"music or the original audio from your speakers."));
	v->addWidget(voTimelineSound_);
	voStopAtEnd_ = new QCheckBox(QStringLiteral("Stop recording when playback reaches the end"), voPanel_);
	voStopAtEnd_->setChecked(st.value(QStringLiteral("editor/vo/stopAtEnd"), true).toBool());
	v->addWidget(voStopAtEnd_);
	auto syncDeps = [this]() {
		voTimelineSound_->setEnabled(voTalkAlong_->isChecked());
		voStopAtEnd_->setEnabled(voTalkAlong_->isChecked());
	};
	connect(voTalkAlong_, &QCheckBox::toggled, this, syncDeps);
	syncDeps();

	voMeter_ = new LevelMeter(voPanel_);
	voMeter_->setMinimumHeight(14);
	v->addWidget(voMeter_);
	voStatus_ = new QLabel(QStringLiteral("Ready."), voPanel_);
	voStatus_->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	voStatus_->setWordWrap(true);
	v->addWidget(voStatus_);

	auto *btns = new QHBoxLayout;
	voRecordBtn_ = new QPushButton(QStringLiteral("Record"), voPanel_);
	voRecordBtn_->setIcon(uiIcon(Glyph::Record, 12, QColor(0xe5, 0x48, 0x4d)));
	voRecordBtn_->setMinimumHeight(32);
	voRecordBtn_->setDefault(true);
	voRecordBtn_->setToolTip(QStringLiteral("Start recording (after the countdown). Press again to stop."));
	btns->addWidget(voRecordBtn_, 1);
	auto *close = new QPushButton(QStringLiteral("Close"), voPanel_);
	connect(close, &QPushButton::clicked, voPanel_, &QDialog::close);
	btns->addWidget(close);
	v->addLayout(btns);

	for (QComboBox *cb : {voDevice_, voStartAt_, voChannels_, voRate_})
		connect(cb, &QComboBox::currentIndexChanged, this, [this](int) { saveVoiceoverSettings(); });
	for (QCheckBox *cb : {voTalkAlong_, voTimelineSound_, voStopAtEnd_, voTrimSilence_})
		connect(cb, &QCheckBox::toggled, this, [this](bool) { saveVoiceoverSettings(); });
	connect(voDevice_, &QComboBox::currentIndexChanged, this, [this](int) { syncVoiceoverSource(); });
	syncVoiceoverSource();
	connect(voCountdownSec_, &QSpinBox::valueChanged, this, [this](int) { saveVoiceoverSettings(); });
	// Closing the panel mid-take would leave the microphone running with no
	// way to stop it: finish the take instead.
	connect(voPanel_, &QDialog::finished, this, [this](int) {
		if (voCountdownTimer_ && voCountdownTimer_->isActive())
			onVoiceoverRecordClicked(); // cancels
		if (voRecording_)
			finishVoiceover();
	});
}

void VideoEditorWindow::openVoiceoverPanel()
{
	if (!voPanel_)
		return;
	if (!voRecording_)
		refreshVoiceoverDevices();
	voPanel_->show();
	voPanel_->raise();
	voPanel_->activateWindow();
	if (voRecordBtn_)
		voRecordBtn_->setFocus();
}

void VideoEditorWindow::onImportAudioClicked()
{
	if (!valid_ || voRecording_)
		return;
	const QString in = QFileDialog::getOpenFileName(
		this, QStringLiteral("Import audio"), QString(),
		audioOpenFilter());
	if (in.isEmpty())
		return;
	// Decode to our uniform PCM WAV so waveform/trim/mix all work the same way.
	const QString base = voiceoverTempDir() + QStringLiteral("/import_") +
			     QFileInfo(in).completeBaseName() + QStringLiteral(".wav");
	if (!VoiceoverMixer::decodeToWav(in, base)) {
		warnAndLog(this, QStringLiteral("Import audio"),
				     QStringLiteral("Could not read that audio file."));
		return;
	}
	VoiceoverClip clip;
	clip.path = base;
	clip.outStartMs = currentOutputMs(); // 0 when idle — drag it where you want
	clip.srcTotalMs = VoiceoverTrack::wavDurationMs(base);
	clip.durationMs = clip.srcTotalMs;
	voTrack_->addClip(clip);
}

void VideoEditorWindow::onAutoCut()
{
	if (!valid_ || sceneThread_.joinable())
		return;

	// Modal: pick the scene-change threshold before detecting.
	QDialog dlg(this);
	dlg.setWindowTitle(QStringLiteral("Auto-cut on scene changes"));
	auto *v = new QVBoxLayout(&dlg);
	v->addWidget(infoHint(QStringLiteral("Detect visual scene changes and add a cut at each one.\n"
			       "Lower threshold = more cuts, higher = fewer."), &dlg), 0, Qt::AlignLeft);
	auto *row = new QHBoxLayout;
	row->addWidget(new QLabel(QStringLiteral("Threshold"), &dlg));
	auto *sl = new QSlider(Qt::Horizontal, &dlg);
	sl->setRange(0, 100);
	sl->setValue(40); // 0.40 — FFmpeg's typical default
	row->addWidget(sl, 1);
	auto *val = new QLabel(QStringLiteral("0.40"), &dlg);
	val->setMinimumWidth(40);
	row->addWidget(val);
	connect(sl, &QSlider::valueChanged, val,
		[val](int x) { val->setText(QString::number(x / 100.0, 'f', 2)); });
	v->addLayout(row);
	auto *btns = new QHBoxLayout;
	btns->addStretch(1);
	auto *startBtn = new QPushButton(QStringLiteral("Start"), &dlg);
	startBtn->setDefault(true);
	auto *cancelBtn = new QPushButton(QStringLiteral("Cancel"), &dlg);
	btns->addWidget(startBtn);
	btns->addWidget(cancelBtn);
	v->addLayout(btns);
	connect(startBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
	connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
	if (dlg.exec() != QDialog::Accepted)
		return;
	const double threshold = sl->value() / 100.0;

	stopPlayback();
	sceneCancel_.store(false);
	sceneProgress_ = new QProgressDialog(QStringLiteral("Detecting scene changes…"),
					     QStringLiteral("Cancel"), 0, 100, this);
	sceneProgress_->setWindowModality(Qt::WindowModal);
	sceneProgress_->setAutoClose(false);
	sceneProgress_->setAutoReset(false);
	sceneProgress_->setMinimumDuration(0);
	connect(sceneProgress_, &QProgressDialog::canceled, this, [this]() { sceneCancel_.store(true); });
	sceneProgress_->setValue(0);
	sceneProgress_->show();

	const QString path = inPath_;
	sceneThread_ = std::thread([this, path, threshold]() {
		QString err;
		const QVector<qint64> cuts = SceneDetector::detect(
			path, threshold, &sceneCancel_,
			[this](int pct) {
				QMetaObject::invokeMethod(
					this, [this, pct]() {
						if (sceneProgress_)
							sceneProgress_->setValue(pct);
					},
					Qt::QueuedConnection);
			},
			&err);
		QMetaObject::invokeMethod(
			this, [this, cuts, err]() { onSceneDetected(cuts, err); }, Qt::QueuedConnection);
	});
}

void VideoEditorWindow::onSceneDetected(const QVector<qint64> &cutMs, const QString &err)
{
	if (sceneThread_.joinable())
		sceneThread_.join();
	if (sceneProgress_) {
		sceneProgress_->reset();
		sceneProgress_->deleteLater();
		sceneProgress_ = nullptr;
	}
	if (sceneCancel_.load())
		return;
	if (!err.isEmpty()) {
		warnAndLog(this, QStringLiteral("Auto-cut"), err);
		return;
	}

	// Build cut spans from the boundaries: 0, each scene change, end.
	if (!seeker_)
		return; // the source went while it was being scanned
	const qint64 dur = seeker_->durationMs();
	QVector<qint64> bounds;
	bounds.push_back(0);
	for (qint64 t : cutMs)
		if (t > 0 && t < dur)
			bounds.push_back(t);
	bounds.push_back(dur);
	std::sort(bounds.begin(), bounds.end());

	QVector<CutSegment> segs;
	for (int i = 0; i + 1 < bounds.size(); ++i) {
		const qint64 a = bounds[i], b = bounds[i + 1];
		if (b - a >= 150) { // skip sub-150ms slivers (the cut minimum)
			CutSegment s;
			s.srcStartMs = a;
			s.srcEndMs = b;
			s.speed = 1.0;
			s.sourceId = activeSourceId_; // auto-cut works on the active source
			segs.push_back(s);
		}
	}
	if (segs.size() <= 1) {
		QMessageBox::information(
			this, QStringLiteral("Auto-cut"),
			QStringLiteral("No scene changes were detected — try a lower threshold."));
		return;
	}
	if (!multiCut())
		setEditMode(EditMode::MultiCut); // show the output track
	tracks_->addSegments(segs);
	QMessageBox::information(this, QStringLiteral("Auto-cut"),
				QStringLiteral("Added %1 cuts from scene changes.").arg(segs.size()));
}

void VideoEditorWindow::onSaveProject()
{
	if (!valid_)
		return;
	// Re-save over the known file; fall back to Save As the first time.
	if (projectPath_.isEmpty()) {
		onSaveProjectAs();
		return;
	}
	const QString err = saveProjectTo(projectPath_, /*quiet=*/false);
	if (!err.isEmpty())
		warnAndLog(this, QStringLiteral("Save project"), err);
	refreshProjectInspector();
}

void VideoEditorWindow::onSaveProjectAs()
{
	if (!valid_)
		return;
	// A project that has never been saved goes to the projects folder under a
	// name taken from its media, not next to whichever file it happened to
	// start from -- see ProjectPaths.hpp. Already-saved projects stay where
	// they are, which is where the user put them.
	QString suggested = projectPath_;
	if (suggested.isEmpty()) {
		const QString dir = ensureProjectsFolder();
		suggested = dir.isEmpty()
				    // No projects folder could be made: the old behaviour, so
				    // Save As still opens somewhere sensible.
				    ? QFileInfo(inPath_).absolutePath() + QLatin1Char('/') +
					      suggestedProjectStem(inPath_) +
					      QStringLiteral(".harpiaproj")
				    : uniqueProjectPath(dir, suggestedProjectStem(inPath_));
	}
	QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save project as"), suggested,
						    QStringLiteral("Harpia project (*.harpiaproj)"));
	if (path.isEmpty())
		return;
	if (!path.endsWith(QStringLiteral(".harpiaproj"), Qt::CaseInsensitive))
		path += QStringLiteral(".harpiaproj");
	const QString err = saveProjectTo(path, /*quiet=*/false);
	if (!err.isEmpty())
		warnAndLog(this, QStringLiteral("Save project"), err);
	refreshProjectInspector();
}

// Writes the project to `path`. Returns "" on success, else a message. `quiet`
// suppresses the success dialog and leaves projectPath_ alone (used by autosave,
// which writes to a sidecar file).
QString VideoEditorWindow::saveProjectTo(const QString &path, bool quiet)
{
	const EditorSnapshot s = snapshot();
	const QString projDir = QFileInfo(path).absolutePath();
	const QString assetsRel = QFileInfo(path).completeBaseName() + QStringLiteral("_assets");
	const QString assetsDir = projDir + QLatin1Char('/') + assetsRel;

	QJsonObject root;
	root[QStringLiteral("harpiaProject")] = 2; // v2: multiple sources
	// Project metadata (shown in the Inspector's Project section).
	if (!projectCreated_.isValid())
		projectCreated_ = QDateTime::currentDateTime();
	root[QStringLiteral("created")] = projectCreated_.toString(Qt::ISODate);
	root[QStringLiteral("saved")] = QDateTime::currentDateTime().toString(Qt::ISODate);
	if (!projectAuthor_.isEmpty())
		root[QStringLiteral("author")] = projectAuthor_;
	// Output format. Only written when PINNED: an absent key means "auto", so a
	// project saved before this existed still follows its first clip, and one
	// left on Auto keeps doing so on another machine.
	if (projCanvas_.isValid() && !projCanvas_.isEmpty()) {
		root[QStringLiteral("canvasW")] = projCanvas_.width();
		root[QStringLiteral("canvasH")] = projCanvas_.height();
	}
	if (projFps_ >= 1.0)
		root[QStringLiteral("fps")] = projFps_;
	// All sources (index by stable id; segments reference these ids).
	QJsonArray srcArr;
	for (const EditorSource &es : sources_) {
		QJsonObject so;
		so[QStringLiteral("id")] = es.id;
		// The original for an audio source, whose `path` is a session decode in
		// a temp folder: saving that meant saving a file that would not exist
		// the next time the project was opened.
		so[QStringLiteral("path")] = QDir::toNativeSeparators(
			es.origPath.isEmpty() ? es.path : es.origPath);
		so[QStringLiteral("name")] = es.name;
		if (!es.librarySlot.isEmpty())
			so[QStringLiteral("librarySlot")] = es.librarySlot;
		so[QStringLiteral("durationMs")] = double(es.durationMs);
		so[QStringLiteral("width")] = es.width;
		so[QStringLiteral("height")] = es.height;
		srcArr.append(so);
	}
	root[QStringLiteral("sources")] = srcArr;
	// Kept for the "different file" hint when a v2 project is opened elsewhere.
	root[QStringLiteral("sourceName")] = QFileInfo(inPath_).fileName();
	root[QStringLiteral("trimStart")] = double(s.trimStart);
	root[QStringLiteral("trimEnd")] = double(s.trimEnd);
	root[QStringLiteral("speed")] = s.speed;
	QJsonObject crop;
	crop[QStringLiteral("enabled")] = s.cropEnabled;
	crop[QStringLiteral("x")] = s.cropRect.x();
	crop[QStringLiteral("y")] = s.cropRect.y();
	crop[QStringLiteral("w")] = s.cropRect.width();
	crop[QStringLiteral("h")] = s.cropRect.height();
	root[QStringLiteral("crop")] = crop;

	QJsonArray segArr;
	for (const CutSegment &c : s.segments) {
		QJsonObject o;
		o[QStringLiteral("srcStart")] = double(c.srcStartMs);
		o[QStringLiteral("srcEnd")] = double(c.srcEndMs);
		o[QStringLiteral("speed")] = c.speed;
		o[QStringLiteral("source")] = c.sourceId;
		segArr.append(o);
	}
	root[QStringLiteral("segments")] = segArr;

	// Copy each voiceover WAV into the project's assets folder so it's portable.
	QJsonArray voArr;
	if (!s.voiceClips.isEmpty())
		QDir().mkpath(assetsDir);
	bool copyOk = true;
	for (int i = 0; i < s.voiceClips.size(); ++i) {
		const VoiceoverClip &v = s.voiceClips[i];
		const QString dstName = QStringLiteral("vo_%1.wav").arg(i, 3, 10, QLatin1Char('0'));
		const QString dst = assetsDir + QLatin1Char('/') + dstName;
		QFile::remove(dst);
		if (!QFile::copy(v.path, dst))
			copyOk = false;
		QJsonObject o;
		o[QStringLiteral("file")] = assetsRel + QLatin1Char('/') + dstName;
		o[QStringLiteral("outStart")] = double(v.outStartMs);
		o[QStringLiteral("duration")] = double(v.durationMs);
		o[QStringLiteral("srcStart")] = double(v.srcStartMs);
		o[QStringLiteral("srcTotal")] = double(v.srcTotalMs);
		o[QStringLiteral("volume")] = v.volume;
		o[QStringLiteral("fadeIn")] = v.fadeInMs;
		o[QStringLiteral("fadeOut")] = v.fadeOutMs;
		voArr.append(o);
	}
	root[QStringLiteral("voiceovers")] = voArr;
	// How the takes are mixed with the video's own sound. Not part of a take,
	// so it lived only on the sliders -- and a project reopened with the
	// original back at 100% exported differently from the one that was saved.
	{
		QJsonObject mix;
		mix[QStringLiteral("originalVolume")] = voOrigVol_ ? voOrigVol_->value() : 100;
		mix[QStringLiteral("duck")] = voDuck_ && voDuck_->isChecked();
		root[QStringLiteral("voiceoverMix")] = mix;
	}
	// The mode it was being edited in, and which source was in front. Both used
	// to be guessed on open -- a timeline means Full editing, cuts mean
	// Multi-Cut -- which reopened a Trim project that also had cuts in
	// Multi-Cut, and applied the trim range to the first source rather than
	// the one being trimmed.
	root[QStringLiteral("editMode")] = fullEdit()   ? QStringLiteral("full")
					   : multiCut() ? QStringLiteral("multicut")
							: QStringLiteral("trim");
	if (activeSourceId_ >= 0)
		root[QStringLiteral("activeSource")] = activeSourceId_;

	// "Full editing" multi-track timeline (project v3). Clip source ids are
	// remapped through the same `sources` array as the Multi-Cut segments.
	// Tags and sound rules count too: a project set up before any clip is on
	// it keeps its tag list.
	// Sound rules' sources are in `sources` like a clip's. See TimelineJson.hpp.
	timelineToProject(s.timeline, root);

	// No project-level "effects" array any more: a shader is a component, so it
	// is written with the clip that carries it. The reader still understands the
	// old key and migrates it -- see applyProjectJson.

	QFile f(path);
	if (!f.open(QIODevice::WriteOnly))
		return QStringLiteral("Could not write the project file.");
	f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	f.close();

	if (quiet) { // autosave sidecar — don't adopt it as the project file
		// Save when closing also uses quiet mode, but writes the actual
		// project rather than an autosave sidecar.
		if (!projectPath_.isEmpty() &&
		    QFileInfo(path).absoluteFilePath() == QFileInfo(projectPath_).absoluteFilePath())
			rememberRecentProject(path);
		lastAutosave_ = QDateTime::currentDateTime();
		return QString();
	}
	projectPath_ = path;
	rememberRecentProject(path);
	if (!copyOk)
		return QStringLiteral("Project saved, but some voiceover audio could not be copied.");
	QMessageBox::information(this, QStringLiteral("Save project"), QStringLiteral("Project saved."));
	return QString();
}

// The file dialog, the read and the parse. Applying what was parsed is
// applyProjectJson() below -- separated so opening a project can be driven
// without a dialog in front of it.
void VideoEditorWindow::onOpenProject()
{
	// No `valid_` guard. It used to return here, silently, and that made this
	// button do NOTHING -- no dialog, no message -- in the one state where it
	// matters most: an editor whose launch file failed to open is exactly the
	// editor you want to load a project into. Opening a project is how you get
	// out of an invalid state, so it cannot be gated on being in a valid one.
	//
	// Where projects are kept, not where the current media happens to live.
	const QString startIn = QDir(defaultProjectsFolder()).exists()
					? defaultProjectsFolder()
					: QFileInfo(inPath_).absolutePath();
	const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open project"),
							  startIn,
							  QStringLiteral("Harpia project (*.harpiaproj)"));
	if (path.isEmpty())
		return;
	openProjectWithDialogs(path);
}

void VideoEditorWindow::openProjectWithDialogs(const QString &path, bool showSuccess)
{
	LoadingScope loading(*this, QStringLiteral("Loading project"));
	loading.stage(QStringLiteral("Reading project…"), path);
	const ProjectRead project = runLoadingTask([path]() { return readProjectFile(path); });
	if (!project.error.isEmpty()) {
		loading.hide();
		warnAndLog(this, QStringLiteral("Open project"), project.error);
		return;
	}
	applyProjectJson(project.root, path, /*quiet=*/false, showSuccess);
}

// Open a project from a known path. Same validation as the menu item, without
// the dialog and without a message box on failure.
bool VideoEditorWindow::openProjectAt(const QString &path)
{
	LoadingScope loading(*this, QStringLiteral("Loading project"));
	loading.stage(QStringLiteral("Reading project…"), path);
	const ProjectRead project = runLoadingTask([path]() { return readProjectFile(path); });
	if (!project.error.isEmpty())
		return false;
	applyProjectJson(project.root, path, /*quiet=*/true);
	return true;
}

// Rebuild the whole editor from a parsed project document.
// Decode the waveform peaks for a set of files at once, on every core.
//
// Opening a project used to do this one file at a time on the GUI thread, so a
// project with eight audio sources paid eight full decodes back to back before
// the window would even paint. loadPeaks is a pure function of the path -- it
// opens its own file, holds no static state and touches nothing shared -- which
// makes this the safest kind of parallelism there is: N independent calls whose
// results are collected afterwards.
//
// blockingMapped, not a detached pool: the caller needs the answers before it
// can build the model, and running them concurrently while still waiting for
// all of them keeps the sequencing exactly as it was. The preparation thread
// handles one file itself while the GUI stays free to display loading feedback.
QHash<QString, QVector<float>> VideoEditorWindow::decodePeaks(const QStringList &paths)
{
	QHash<QString, QVector<float>> out;
	// Distinct paths only. The same file on two tracks used to be decoded twice
	// because the cache was per track rather than per project.
	QStringList todo;
	for (const QString &p : paths)
		if (!p.isEmpty() && !todo.contains(p))
			todo << p;
	if (todo.isEmpty())
		return out;
	if (todo.size() == 1) { // not worth a pool for one file
		out.insert(todo.first(), VoiceoverTrack::loadPeaks(todo.first(), 600));
		return out;
	}
	const QVector<QVector<float>> got = blockingMapped(
		QVector<QString>(todo.cbegin(), todo.cend()),
		[](const QString &p) { return VoiceoverTrack::loadPeaks(p, 600); });
	for (int i = 0; i < todo.size() && i < got.size(); ++i)
		out.insert(todo[i], got[i]);
	return out;
}

void VideoEditorWindow::applyProjectJson(const QJsonObject &root, const QString &path, bool quiet,
					 bool showSuccess)
{
	// Playback owns the preview, the per-track decoders and its own idea of
	// where it is; this is about to replace all three. Stopping first is the
	// difference between a load and a load racing a timer.
	if (playing_)
		stopPlayback();

	// Everything below runs with the editor half-rebuilt. The flag keeps the
	// per-file error boxes out of that window -- see addSource.
	LoadingScope loading(*this, QStringLiteral("Loading project"));
	QScopedValueRollback<bool> projectLoading(loadingProject_, true);
	QScopedValueRollback<int> totalLoading(loadingTotal_, 0);
	QScopedValueRollback<int> completedLoading(loadingCompleted_, 0);
	++projectLoadGeneration_; // old async audio imports belong to the previous timeline
	previewRenderWarning_->resetHistory();
	loadFailures_.clear();

	const int ver = root.value(QStringLiteral("harpiaProject")).toInt(1);

	// v2 projects carry their own list of sources — open (or relink) each and
	// map its saved id onto the editor's live source id. v1 projects (single
	// source) fall back to the primary source and the "different file" hint.
	QHash<int, int> srcMap; // project source id -> editor source id
	int defaultSrcId = sources_.empty() ? 0 : sources_.front().id;
	if (ver >= 2 && root.value(QStringLiteral("sources")).isArray()) {
		// Which sources anything actually USES. A project can list a source no
		// clip refers to -- delete the last clip that used a file and the entry
		// stays -- and stopping the whole load to hunt for a file nothing needs
		// is how a perfectly good project fails to open. The reported one did
		// exactly that: an imported voiceover, long since deleted from a temp
		// folder, on an audio track with no clips on it.
		QSet<int> used;
		for (const QJsonValue &tv : root.value(QStringLiteral("tracks")).toArray())
			for (const QJsonValue &cv : tv.toObject().value(QStringLiteral("clips")).toArray()) {
				const QJsonObject co = cv.toObject();
				const QString cty = co.value(QStringLiteral("type")).toString();
				// A caption carries a source id it never reads.
				if (cty != QLatin1String("text"))
					used.insert(co.value(QStringLiteral("source")).toInt());
			}
		for (const QJsonValue &sv : root.value(QStringLiteral("segments")).toArray())
			used.insert(sv.toObject().value(QStringLiteral("source")).toInt());
		for (const QJsonValue &rv : root.value(QStringLiteral("soundRules")).toArray())
			used.insert(rv.toObject().value(QStringLiteral("source")).toInt());
		if (root.contains(QStringLiteral("activeSource")))
			used.insert(root.value(QStringLiteral("activeSource")).toInt());

		bool first = true;
		const QJsonArray projectSources = root.value(QStringLiteral("sources")).toArray();
		loadingTotal_ = projectSources.size();
		for (int sourceIndex = 0; sourceIndex < projectSources.size(); ++sourceIndex) {
			loadingCompleted_ = sourceIndex;
			loading.stage(QStringLiteral("Loading project media…"), path);
			const QJsonValue jv = projectSources[sourceIndex];
			const QJsonObject so = jv.toObject();
			const int pid = so.value(QStringLiteral("id")).toInt();
			QString spath = so.value(QStringLiteral("path")).toString();
			const QString sname = so.value(QStringLiteral("name")).toString();
			// A library slot plays whatever file the slot has NOW, on this
			// machine: that is how swapping a slot reaches old projects.
			if (const QString slot = so.value(QStringLiteral("librarySlot")).toString(); !slot.isEmpty()) {
				const int sid = librarySoundSource(slot);
				if (sid >= 0) {
					srcMap.insert(pid, sid);
					continue;
				}
			}
			if (!used.contains(pid) && !QFileInfo::exists(spath))
				continue; // nothing needs it and it is not there: let it go
			const EditorSource *known = sourceByPath(spath);
			int eid = known ? known->id : -1;
			if (eid < 0) {
				// A quiet open never puts a dialog up: a missing file is
				// left missing rather than stopping an automated caller.
				if (!quiet && !QFileInfo::exists(spath)) {
					loading.hide();
					const QString picked = QFileDialog::getOpenFileName(
						this,
						QStringLiteral("Locate \"%1\"")
							.arg(sname.isEmpty() ? QFileInfo(spath).fileName()
									     : sname),
						QFileInfo(path).absolutePath(),
						// The filter follows what went missing. A video
						// filter on an image source made the file you
						// needed unselectable, which reads as the
						// relink being broken.
						isImageFile(spath) ? imageOpenFilter()
						: isAudioFile(spath)
							? audioOpenFilter()
							: QStringLiteral(
								  "Video files (*.mp4 *.mov *.mkv *.webm "
								  "*.avi *.m4v *.gif *.wmv *.flv "
								  "*.ts);;All files (*)"));
					if (!picked.isEmpty())
						spath = picked;
				}
				// A still comes back as a STILL. Every source used to be
				// reopened through addSource, which builds a video source
				// around a FrameSeeker -- so a reopened project had no
				// entry in stillImages_ for its image clips, and the one
				// place that serves a still's picture is that map. They
				// came back as sources with no picture behind them, and
				// now they also come back with their thumbnail.
				// isVideoFile FIRST, as everywhere else that asks: a GIF is
				// in both lists on purpose, and the video reader is what
				// people mean by one (MediaFiles.hpp says so at length).
				// By KIND -- see sourceKindFor. Audio is the one that was
				// missing: everything that was not an image went to the video
				// reader, which cannot open an audio-only file at all, so a
				// project with a voiceover or an imported track could never
				// load its audio back. Relinking failed the same way: you found
				// the file you were asked for and were told it could not be
				// opened.
				switch (sourceKindFor(spath)) {
				case SourceKind::Image: eid = addImageSource(spath); break;
				case SourceKind::Audio: eid = addAudioSource(spath); break;
				case SourceKind::Video: eid = addSource(spath); break;
				}
			}
			if (eid >= 0) {
				srcMap.insert(pid, eid);
				if (first) {
					defaultSrcId = eid;
					first = false;
				}
			}
		}
	} else {
		const QString projSourceName = root.value(QStringLiteral("sourceName")).toString();
		if (!quiet && !projSourceName.isEmpty() &&
		    projSourceName != QFileInfo(inPath_).fileName()) {
			loading.hide();
			const auto ret = QMessageBox::question(
				this, QStringLiteral("Open project"),
				QStringLiteral(
					"This project was made for \"%1\", but you're editing \"%2\".\n"
					"Apply the edits anyway?")
					.arg(projSourceName, QFileInfo(inPath_).fileName()));
			if (ret != QMessageBox::Yes)
				return;
		}
	}

	loadingTotal_ = 0;
	loading.stage(QStringLiteral("Preparing timeline…"), path);
	EditorSnapshot s;
	s.trimStart = qint64(root.value(QStringLiteral("trimStart")).toDouble(0));
	// A blank editor (opened with no file -- "Load project" on the empty
	// panel) has no seeker yet, and reading its duration here crashed the open
	// before any of the project was applied. The fallback only matters for a
	// project with no trimEnd at all.
	s.trimEnd = qint64(root.value(QStringLiteral("trimEnd"))
				   .toDouble(seeker_ ? double(seeker_->durationMs()) : 0.0));
	s.speed = root.value(QStringLiteral("speed")).toDouble(1.0);
	const QJsonObject crop = root.value(QStringLiteral("crop")).toObject();
	s.cropEnabled = crop.value(QStringLiteral("enabled")).toBool(false);
	s.cropRect = QRect(crop.value(QStringLiteral("x")).toInt(), crop.value(QStringLiteral("y")).toInt(),
			   crop.value(QStringLiteral("w")).toInt(), crop.value(QStringLiteral("h")).toInt());
	for (const QJsonValue &jv : root.value(QStringLiteral("segments")).toArray()) {
		const QJsonObject o = jv.toObject();
		CutSegment c;
		c.srcStartMs = qint64(o.value(QStringLiteral("srcStart")).toDouble());
		c.srcEndMs = qint64(o.value(QStringLiteral("srcEnd")).toDouble());
		c.speed = o.value(QStringLiteral("speed")).toDouble(1.0);
		// Remap the project's source id onto the live editor source (v1 has no
		// per-segment source → the primary).
		const int pid = o.value(QStringLiteral("source")).toInt(defaultSrcId);
		c.sourceId = srcMap.isEmpty() ? defaultSrcId : srcMap.value(pid, defaultSrcId);
		s.segments.push_back(c);
	}
	const QString projDir = QFileInfo(path).absolutePath();
	for (const QJsonValue &jv : root.value(QStringLiteral("voiceovers")).toArray()) {
		const QJsonObject o = jv.toObject();
		VoiceoverClip v;
		v.path = QDir(projDir).filePath(o.value(QStringLiteral("file")).toString());
		v.outStartMs = qint64(o.value(QStringLiteral("outStart")).toDouble());
		v.durationMs = qint64(o.value(QStringLiteral("duration")).toDouble());
		v.srcStartMs = qint64(o.value(QStringLiteral("srcStart")).toDouble());
		v.srcTotalMs = qint64(o.value(QStringLiteral("srcTotal")).toDouble());
		v.volume = o.value(QStringLiteral("volume")).toDouble(1.0);
		v.fadeInMs = o.value(QStringLiteral("fadeIn")).toInt(15);
		v.fadeOutMs = o.value(QStringLiteral("fadeOut")).toInt(15);
		s.voiceClips.push_back(v); // peaks filled in below, all files at once
	}

	// "Full editing" timeline (project v3). Absent in v1/v2 projects, which just
	// restore an empty timeline.
	s.timeline = timelineFromProject(root); // see TimelineJson.hpp
	// Source ids in the file index the project's own `sources` array; map them
	// onto whatever those files became in this session's media pool.
	for (TlTrack &t : s.timeline.tracks)
		for (TlClip &c : t.clips) {
			c.sourceId = srcMap.isEmpty() ? defaultSrcId : srcMap.value(c.sourceId, defaultSrcId);
			// A Random clip's other choices name files the same way.
			for (TlClipOption &o : c.clipOptions)
				o.sourceId = srcMap.isEmpty() ? defaultSrcId : srcMap.value(o.sourceId, defaultSrcId);
		}
	for (TlSoundRule &r : s.timeline.soundRules)
		r.sourceId = srcMap.isEmpty() ? r.sourceId : srcMap.value(r.sourceId, 0);

	// Waveforms are a derived cache, so they are not stored in the project — but
	// they DO have to be rebuilt, or an audio clip reopens as a blank bar. Every
	// file the project needs is decoded in ONE parallel pass here, rather than
	// one at a time as each track is read: the decodes are independent, and
	// serialising them was most of the time it took to open a project with
	// several audio sources.
	{
		QStringList want;
		for (const VoiceoverClip &v : s.voiceClips)
			want << v.path;
		for (const TlTrack &t : s.timeline.tracks) {
			if (t.kind != TlTrack::Kind::Audio)
				continue;
			for (const TlClip &c : t.clips) {
				if (c.type == TlClip::Type::Text || c.type == TlClip::Type::Image)
					continue;
				if (const EditorSource *es = sourceById(c.sourceId))
					want << es->path;
			}
		}
		loading.stage(QStringLiteral("Building audio waveforms…"), path);
		const QHash<QString, QVector<float>> peaks = runLoadingTask([want]() {
			return decodePeaks(want);
		});
		for (VoiceoverClip &v : s.voiceClips)
			v.peaks = peaks.value(v.path);
		for (TlTrack &t : s.timeline.tracks) {
			if (t.kind != TlTrack::Kind::Audio)
				continue;
			for (TlClip &c : t.clips) {
				if (c.type == TlClip::Type::Text || c.type == TlClip::Type::Image)
					continue;
				if (const EditorSource *es = sourceById(c.sourceId))
					c.peaks = peaks.value(es->path);
			}
		}
	}

	// Migration: Inverse Selection used to be one project-wide setting living
	// next to the tracks. It is a CLIP now, so a project that carries the old
	// field gets an effect track holding one spotlight clip spanning the whole
	// timeline -- which is what "whole project" meant, said in the timeline's own
	// vocabulary. From there it can be trimmed, moved, or joined by a second one,
	// none of which the old field could express.
	//
	// The track goes on TOP so it dims everything, matching where the old pass
	// ran (last, over the finished composite).
	if (root.contains(QStringLiteral("spotlight"))) {
		const SpotlightSpec spec =
			spotlightFromJson(root.value(QStringLiteral("spotlight")).toObject());
		if (spec != SpotlightSpec()) {
			TlClip c;
			c.type = TlClip::Type::Effect;
			c.fx.type = FxType::InverseSelection;
			c.fx.name = QStringLiteral("Inverse Selection");
			c.fx.enabled = true;
			c.fx.spot = spec;
			c.outStartMs = 0;
			// An effect clip's length is free, so the source range IS its
			// duration. A timeline with nothing on it still gets a usable clip
			// rather than a zero-width one that cannot be grabbed.
			c.srcStartMs = 0;
			c.srcEndMs = std::max<qint64>(1000, s.timeline.durationMs());

			TlTrack t;
			t.kind = TlTrack::Kind::Effect;
			t.name = QStringLiteral("Inverse Selection");
			t.clips.append(c);
			s.timeline.tracks.prepend(t);
		}
	}

	// Migration: the post-processing chain used to be a project-level list of
	// shaders applied to the finished picture. Each becomes a shader COMPONENT,
	// in the same order, on one effect clip spanning the whole timeline -- which
	// is exactly what "whole output, whole timeline" meant. From there it can be
	// trimmed to a stretch, moved down the stack, or keyframed, none of which the
	// project-level list could do.
	//
	// Older projects stored a single "shader" object instead of an "effects"
	// array; both read the same way.
	{
		auto readEffect = [](const QJsonObject &fo) {
			ShaderState st;
			st.name = fo.value(QStringLiteral("name")).toString();
			const QJsonObject params = fo.value(QStringLiteral("params")).toObject();
			for (auto it = params.constBegin(); it != params.constEnd(); ++it)
				st.params[it.key()] = it.value().toDouble();
			return st;
		};
		QVector<ShaderState> old;
		if (root.value(QStringLiteral("effects")).isArray()) {
			for (const QJsonValue &jv : root.value(QStringLiteral("effects")).toArray()) {
				const ShaderState st = readEffect(jv.toObject());
				if (!st.name.isEmpty())
					old.append(st);
			}
		} else if (root.value(QStringLiteral("shader")).isObject()) {
			const ShaderState st =
				readEffect(root.value(QStringLiteral("shader")).toObject());
			if (!st.name.isEmpty())
				old.append(st);
		}
		if (!old.isEmpty()) {
			TlClip c;
			c.type = TlClip::Type::Effect;
			c.fx.type = FxType::Brightness; // a carrier; the components do the work
			c.fx.enabled = false;
			c.fx.name = QStringLiteral("Shaders");
			c.outStartMs = 0;
			c.srcStartMs = 0;
			c.srcEndMs = std::max<qint64>(1000, s.timeline.durationMs());
			int n = 0;
			for (const ShaderState &st : old) {
				ComponentInstance ci;
				ci.typeId = shaderComponentId(st.name);
				// A shader whose file is gone registers no type. Skipped rather
				// than added as a component nothing can render; putting the
				// .frag back and reopening restores it.
				if (!ComponentRegistry::instance().find(ci.typeId))
					continue;
				ci.instanceId = QStringLiteral("shader%1").arg(n++);
				for (auto it = st.params.cbegin(); it != st.params.cend(); ++it)
					ci.props.insert(it.key(), it.value());
				c.components.append(ci);
			}
			if (!c.components.isEmpty()) {
				TlTrack t;
				t.kind = TlTrack::Kind::Effect;
				t.name = QStringLiteral("Shaders");
				t.clips.append(c);
				// On top, where the old chain ran: after everything else.
				s.timeline.tracks.prepend(t);
			}
		}
	}

	// Show the source that was in front when it was saved (older projects: the
	// first one it uses), then apply the edits -- the trim range belongs to it.
	int activeSrc = defaultSrcId;
	if (root.contains(QStringLiteral("activeSource"))) {
		const int pid = root.value(QStringLiteral("activeSource")).toInt();
		if (srcMap.contains(pid))
			activeSrc = srcMap.value(pid);
	}
	loading.stage(QStringLiteral("Restoring timeline…"), path);
	setActiveSource(activeSrc);
	restoreSnapshot(s);
	// Remember where this project lives + its metadata (Project inspector).
	projectPath_ = path;
	projectAuthor_ = root.value(QStringLiteral("author")).toString();
	projectCreated_ =
		QDateTime::fromString(root.value(QStringLiteral("created")).toString(), Qt::ISODate);
	// The project's own format beats the remembered one -- opening someone
	// else's 4K vertical edit must not quietly render it at your last setting.
	// Absent keys mean auto, which is also what every pre-v0.1.156 project says.
	{
		const int cw = root.value(QStringLiteral("canvasW")).toInt();
		const int chh = root.value(QStringLiteral("canvasH")).toInt();
		projCanvas_ = (cw >= 16 && chh >= 16) ? QSize(cw & ~1, chh & ~1) : QSize();
		const double f = root.value(QStringLiteral("fps")).toDouble();
		projFps_ = (f >= 1.0 && f <= 240.0) ? f : 0.0;
		// A project stores numbers, not which dropdown entry produced them, so
		// a saved size that matches a preset comes back showing that preset.
		projResCustom_ = projFpsCustom_ = false;
		if (timelineView_)
			timelineView_->setFrameRate(timelineFps());
		syncProjectFormatControls();
	}
	refreshProjectInspector();

	// The voiceover mix. Absent (older projects) means the defaults, not
	// whatever the last project left on the sliders.
	{
		const QJsonObject mix = root.value(QStringLiteral("voiceoverMix")).toObject();
		if (voOrigVol_)
			voOrigVol_->setValue(std::clamp(mix.value(QStringLiteral("originalVolume")).toInt(100), 0, 150));
		if (voDuck_)
			voDuck_->setChecked(mix.value(QStringLiteral("duck")).toBool(false));
	}

	// Open in the mode the project was saved in; older projects say nothing,
	// and are opened in the mode their contents suggest.
	if (!s.timeline.isEmpty())
		timelineSeeded_ = true; // don't seed over the loaded timeline
	const QString savedMode = root.value(QStringLiteral("editMode")).toString();
	EditMode want = mode();
	if (savedMode == QLatin1String("full"))
		want = EditMode::Full;
	else if (savedMode == QLatin1String("multicut"))
		want = EditMode::MultiCut;
	else if (savedMode == QLatin1String("trim"))
		want = EditMode::Trim;
	else if (!s.timeline.isEmpty())
		want = EditMode::Full;
	else if (!s.segments.isEmpty())
		want = EditMode::MultiCut;
	if (want != mode())
		setEditMode(want);
	captureSnapshot(); // make the load an undo step
	updateUndoRedoButtons();

	// A loaded project IS a working editor, whatever state this window was in
	// before -- including the one where its launch file would not open.
	valid_ = true;
	rememberRecentProject(path);
	// And the project may have brought in no SOURCES at all: a caption, a
	// still or an effect needs none. updateEmptyState was only ever reached
	// through addSource, so a text-only project loaded into a blank editor
	// left "Nothing loaded yet" sitting over the caption it was rendering and
	// every control switched off -- which reads exactly like Open Project
	// having done nothing.
	// The rules' clips were saved, but their sounds may have moved: rebuild
	// them from what is actually here.
	applySoundRules();
	rebuildSoundsTab();
	rebuildTagsSection();
	updateEmptyState();
	updateInspector();
	if (fullEdit())
		showTimelineFrame(timelinePlayheadMs());
	loading.hide();
	if (!quiet) {
		// One message, whatever went wrong, and only once the editor is whole
		// again. A project whose media has moved still OPENS -- its timeline,
		// its captions and its edits are all there -- so this says what is
		// missing rather than pretending the load failed.
		if (loadFailures_.isEmpty()) {
			if (showSuccess)
				QMessageBox::information(this, QStringLiteral("Open project"),
							 QStringLiteral("Project loaded."));
		} else {
			warnAndLog(
				this, QStringLiteral("Open project"),
				QStringLiteral("The project opened, but %1 of its files could not "
					       "be read:\n\n%2\n\nIts clips are still on the "
					       "timeline. Use Add video to relink them.")
					.arg(loadFailures_.size())
					.arg(loadFailures_.join(QStringLiteral("\n"))));
		}
	}
	loadFailures_.clear();
}

void VideoEditorWindow::joinExport()
{
	if (exportThread_.joinable()) {
		if (exporter_)
			exporter_->cancel();
		exportThread_.join();
	}
}

void VideoEditorWindow::onScrub(qint64 ms)
{
	// User is dragging a handle/playhead — stop playback and show that frame.
	if (playing_)
		stopPlayback();
	// Multi-Cut scrubs come from the Output/Source track and carry the segment's
	// source; Simple-Trim scrubs are always the active source.
	cursorTimeLabel_->setText(previewTimeText(ms));
	requestPreview(multiCut() ? tracks_->scrubSourceId() : activeSourceId_, ms);
}

void VideoEditorWindow::onHoverScrub(qint64 ms)
{
	// Hovering previews the frame under the cursor, but never fights an
	// active playback preview.
	if (!valid_ || playing_)
		return;
	cursorTimeLabel_->setText(previewTimeText(ms));
	requestPreview(multiCut() ? tracks_->scrubSourceId() : activeSourceId_, ms);
}

// The pointer stopped hovering: put the preview back where the marker is.
//
// Without this the picture simply stayed on the last frame the pointer happened
// to be over, which reads as the preview having lost its place -- the playhead
// says one time and the picture shows another, and nothing on screen explains
// why. Hovering is a look-ahead, so it has to be undone when the look ends.
//
// NOT refreshPreviewAtPlayhead: that re-renders wherever the preview last WAS,
// which is the right answer after a quality or canvas change and precisely the
// wrong one here, since hovering is exactly what moved it. This asks the marker.
//
// Through requestPreview rather than showFrame so it takes the same paced,
// newest-wins path a hover does. A hover render already in flight would
// otherwise land after this one and put the hovered frame back up.
void VideoEditorWindow::showPlayheadFrame()
{
	// The pointer has left the timeline (or something else asked for the
	// playhead's frame): the Inspector goes back to describing the playhead,
	// the same instant the picture is about to show. Before the early-out
	// below, so a panel left in its dimmed preview state is always released --
	// including while playing, where the hover preview is refused but the
	// dimming would otherwise stick.
	setHoverInspect(-1);
	// Never during playback, for the same reason hovering does not preview
	// then: the play loop owns the picture and would fight this for it.
	if (!valid_ || playing_)
		return;
	if (fullEdit()) {
		requestPreview(-1, timelinePlayheadMs());
		return;
	}
	if (multiCut()) {
		if (!tracks_)
			return;
		// The marker is in OUTPUT time; the preview wants the source frame that
		// output instant maps to, which is the same lookup playback and Reset
		// marker both do.
		const qint64 ph = std::max<qint64>(0, tracks_->playhead());
		qint64 srcMs = 0;
		const int seg = tracks_->sourceForOutput(ph, &srcMs);
		if (seg < 0 || seg >= tracks_->segments().size())
			return; // the marker is past the last cut: nothing to show
		cursorTimeLabel_->setText(previewTimeText(srcMs));
		requestPreview(tracks_->segments()[seg].sourceId, srcMs);
		return;
	}
	if (!timeline_)
		return;
	// Trim: the marker is source time already, clamped into the trimmed region
	// so leaving the strip cannot park the preview outside the kept part.
	const qint64 ph = std::clamp<qint64>(timeline_->playhead(), timeline_->start(),
					     std::max<qint64>(timeline_->start(), timeline_->end()));
	cursorTimeLabel_->setText(previewTimeText(ph));
	requestPreview(activeSourceId_, ph);
}

void VideoEditorWindow::onPlayPause()
{
	if (playing_)
		stopPlayback();
	else
		startPlayback();
}

void VideoEditorWindow::onResetMarker()
{
	if (!valid_)
		return;
	stopPlayback();
	previewTimer_->stop();
	pendingMs_ = -1;
	setHoverInspect(-1);
	if (fullEdit()) {
		timelineView_->setPlayhead(0);
		cursorTimeLabel_->setText(previewTimeText(0));
		syncPreviewTransformTarget();
		requestPreview(-1, 0);
	} else if (multiCut()) {
		tracks_->setPlayhead(0);
		if (voTrack_)
			voTrack_->setPlayhead(0);
		qint64 srcMs = 0;
		const int seg = tracks_->sourceForOutput(0, &srcMs);
		if (seg >= 0) {
			const int sid = tracks_->segments()[seg].sourceId;
			cursorTimeLabel_->setText(previewTimeText(srcMs));
			showFrame(sid, srcMs); // preview the first frame of the output
		} else {
			showFrame(activeSourceId_, 0);
		}
	} else {
		const qint64 s = timeline_->start();
		timeline_->setPlayhead(s);
		if (voTrack_)
			voTrack_->setPlayhead(0);
		onScrub(s);
	}
}

EditorSnapshot VideoEditorWindow::snapshot() const
{
	EditorSnapshot s;
	s.segments = tracks_->segments();
	s.trimStart = timeline_->start();
	s.trimEnd = timeline_->end();
	s.speed = speed_;
	s.cropEnabled = canvas_->cropEnabled() || (fullEdit() && trimCropWanted_);
	s.cropRect = canvas_->cropRectVideo();
	s.voiceClips = voTrack_->clips();
	if (timelineView_) {
		s.timeline = timelineView_->model();
		s.selection = timelineView_->selectionState();
	}
	return s;
}

void VideoEditorWindow::scheduleSnapshot()
{
	if (loadingDialog_ || restoring_ || !valid_)
		return;
	histTimer_->start(); // (re)start the coalescing timer
}

// Close the current undo entry now. Used for discrete actions, where waiting
// out the coalescing timer would let the NEXT action join the same entry.
void VideoEditorWindow::commitSnapshot()
{
	if (restoring_ || !valid_)
		return;
	histTimer_->stop();
	captureSnapshot();
}

void VideoEditorWindow::captureSnapshot()
{
	if (restoring_ || !valid_)
		return;
	const EditorSnapshot s = snapshot();
	if (histIndex_ >= 0 && histIndex_ < history_.size() && s == history_[histIndex_])
		return; // nothing actually changed
	// Only the selection moved, in the same pass as the newest entry: an edit
	// selecting what it just made (a paste, a split, a drop). That is part of
	// the edit, not a step of its own -- amend it.
	if (histJustPushed_ && histIndex_ > 0 && histIndex_ + 1 == history_.size() &&
	    s.sameContent(history_[histIndex_])) {
		history_[histIndex_] = s;
		return;
	}
	if (histIndex_ + 1 < history_.size())
		history_.resize(histIndex_ + 1); // drop the redo tail
	history_.push_back(s);
	constexpr int kMaxHistory = 100;
	if (history_.size() > kMaxHistory)
		history_.removeFirst();
	histIndex_ = history_.size() - 1;
	histJustPushed_ = true;
	QTimer::singleShot(0, this, [this]() { histJustPushed_ = false; });
	updateUndoRedoButtons();
}

void VideoEditorWindow::restoreSnapshot(const EditorSnapshot &s)
{
	restoring_ = true;
	stopPlayback();
	scriptScanDirty_ = true; // the whole timeline is being swapped out
	invalidateAudioMix();

	tracks_->setSegments(s.segments);

	// Order the trim setters so the internal start<end clamps don't fight us.
	timeline_->setStart(0);
	timeline_->setEnd(s.trimEnd);
	timeline_->setStart(s.trimStart);

	speed_ = s.speed;

	// In Full editing the crop stays off on screen and is only remembered.
	const bool cropNow = s.cropEnabled && !fullEdit();
	trimCropWanted_ = s.cropEnabled && fullEdit();
	{
		QSignalBlocker b(cropToggle_);
		cropToggle_->setChecked(cropNow);
	}
	canvas_->setCropEnabled(cropNow);
	canvas_->setCropRectVideo(s.cropRect);

	voTrack_->setClips(s.voiceClips);

	if (timelineView_) {
		timelineView_->setModel(s.timeline);
		// After the model: the selection is indices into it.
		timelineView_->setSelectionState(s.selection);
		syncPreviewTransformTarget();
	}

	// Refresh derived UI + speed controls for the current mode.
	if (multiCut()) {
		onSegmentSelected(tracks_->selectedIndex());
	} else {
		speedSlider_->setEnabled(valid_);
		speedSpin_->setEnabled(valid_);
		syncSpeedControls(speed_);
		speedLabel_->setText(QString());
	}
	updateInfoLabel();
	updateInspector();
	updateVoiceoverAxis();

	// Repaint the preview at a sensible frame.
	if (fullEdit()) {
		showTimelineFrame(timelinePlayheadMs());
		syncClipInspector();
	} else if (multiCut()) {
		qint64 srcMs = 0;
		const int seg = tracks_->sourceForOutput(0, &srcMs);
		if (seg >= 0)
			showFrame(tracks_->segments()[seg].sourceId, srcMs);
	} else {
		showFrame(activeSourceId_, timeline_->start());
	}

	restoring_ = false;
}

void VideoEditorWindow::undo()
{
	if (histTimer_->isActive()) { // commit a pending edit first
		histTimer_->stop();
		captureSnapshot();
	}
	if (histIndex_ <= 0)
		return;
	--histIndex_;
	restoreSnapshot(history_[histIndex_]);
	updateUndoRedoButtons();
}

void VideoEditorWindow::redo()
{
	if (histTimer_->isActive()) {
		histTimer_->stop();
		captureSnapshot();
	}
	if (histIndex_ + 1 >= history_.size())
		return;
	++histIndex_;
	restoreSnapshot(history_[histIndex_]);
	updateUndoRedoButtons();
}

void VideoEditorWindow::updateUndoRedoButtons()
{
	if (undoBtn_)
		undoBtn_->setEnabled(histIndex_ > 0);
	if (redoBtn_)
		redoBtn_->setEnabled(histIndex_ + 1 < history_.size());
}

void VideoEditorWindow::setPowerSaving(bool on)
{
	if (on == powerSaving_)
		return;
	// Never interrupt work that has to keep running: a microphone take would lose
	// audio, and an export runs on its own thread with a progress dialog up.
	if (on && (voRecording_ || exportThread_.joinable() || !powerSaveEnabled_))
		return;
	powerSaving_ = on;

	if (on) {
		if (playing_) {
			resumeOnFocus_ = true; // only resume what WE paused
			stopPlayback();
		}
		// Drop any debounced preview still queued: nobody is looking at it.
		if (previewTimer_)
			previewTimer_->stop();
		pendingMs_ = -1;
		return;
	}
	if (resumeOnFocus_) {
		resumeOnFocus_ = false;
		startPlayback();
	}
}

// What the mix depends on. Anything that changes the sound changes this string,
// so a cached mix is reused across plays but thrown away after a real edit.
QString VideoEditorWindow::audioMixKey() const
{
	return timelineView_ ? TimelineAudio::mixKey(timelineView_->model(), [this](int id) {
		for (const EditorSource &source : sources_)
			if (source.id == id)
				return source.path;
		return QString();
	}) : QString();
}

void VideoEditorWindow::invalidateAudioMix()
{
	audioMixValid_ = false;
	if (audioPreview_)
		audioPreview_->clear();

	// ...and if the sound was PLAYING, it has just stopped. The buffer it was
	// reading is the old mix and no longer describes the timeline, so clearing
	// it is right -- but nothing used to bring it back, and playback carried on
	// in silence until you stopped and pressed play again.
	//
	// That is what made the clip Volume control look broken: you drag it while
	// listening, which is the only way to set a level, and the sound simply
	// goes away. Same for moving or trimming a clip mid-playback.
	//
	// Re-mixing is safe to ask for on every edit: startPreviewAudio coalesces.
	// A mix already running for an older timeline is cancelled and restarted
	// once it unwinds, so a slider being dragged queues one re-mix, not thirty.
	if (playing_ && fullEdit())
		startPreviewAudio(timelinePlayheadMs());
	else
		schedulePremix();
}

void VideoEditorWindow::schedulePremix()
{
	if (!loadingDialog_ && audioPremixTimer_)
		audioPremixTimer_->start(); // restarts: the last edit of a run decides
}

void VideoEditorWindow::premixAudio()
{
	// While playing, startPreviewAudio already keeps the sound current.
	if (playing_ || !audioPreview_ || !fullEdit() || audioPreview_->isMuted())
		return;
	const QString key = audioMixKey();
	if (key.isEmpty() || (audioMixValid_ && key == audioMixKeyBuilt_))
		return; // nothing audible, or already mixed
	mixAudioFor(key);
}

void VideoEditorWindow::startPreviewAudio(qint64 fromMs)
{
	if (!audioPreview_ || !fullEdit() || audioPreview_->isMuted())
		return;
	const QString key = audioMixKey();
	if (key.isEmpty()) // nothing audible on the timeline
		return;

	if (audioMixValid_ && key == audioMixKeyBuilt_) {
		audioPreview_->start(fromMs);
		return;
	}

	// Needs (re)mixing: decoding every source is far too slow for the GUI thread,
	// so the picture starts now and the sound joins when the mix lands. With
	// the premix that is rare: only Play pressed within moments of an edit, or
	// before a new source's first decode has finished.
	audioStartMs_ = fromMs;
	startAudioWhenReady_ = true;
	mixAudioFor(key);
}

void VideoEditorWindow::mixAudioFor(const QString &key)
{
	if (audioMixRunning_.load()) {
		if (key != audioMixKeyWanted_)
			audioMixCancel_.store(true); // stale: restart when it unwinds
		return;
	}

	audioMixKeyWanted_ = key;
	audioMixCancel_.store(false);
	audioMixRunning_.store(true);
	if (audioMixThread_.joinable())
		audioMixThread_.join();

	// Snapshot everything the worker touches: it must not read editor state.
	const TimelineModel model = timelineView_->model();
	QMap<int, QString> paths;
	for (const EditorSource &es : sources_)
		paths.insert(es.id, es.path);

	audioMixThread_ = std::thread([this, model, paths]() {
		std::vector<float> pcm = TimelineAudio::mixToBuffer(
			model, [&paths](int id) { return paths.value(id); }, &audioMixCancel_);
		QMetaObject::invokeMethod(
			this,
			[this, pcm = std::move(pcm)]() mutable {
				pendingMix_ = std::move(pcm);
				onAudioMixReady();
			},
			Qt::QueuedConnection);
	});
}

void VideoEditorWindow::onAudioMixReady()
{
	audioMixRunning_.store(false);
	const bool wasCanceled = audioMixCancel_.load();
	audioMixCancel_.store(false);

	if (wasCanceled) { // the timeline moved on: mix again for where it is now
		pendingMix_.clear();
		if (startAudioWhenReady_ && playing_)
			startPreviewAudio(audioStartMs_);
		else
			schedulePremix();
		return;
	}

	audioPreview_->setBuffer(std::move(pendingMix_));
	pendingMix_.clear();
	audioMixKeyBuilt_ = audioMixKeyWanted_;
	audioMixValid_ = true;

	if (!startAudioWhenReady_ || !playing_)
		return;
	startAudioWhenReady_ = false;
	// Join at where the picture has ALREADY reached, not where it was asked to
	// start, or the sound would come in behind by however long the mix took.
	audioPreview_->start(timelinePlayheadMs());
}

void VideoEditorWindow::startPlayback()
{
	if (!valid_)
		return;
	// Playback owns the picture; discard a prior mode's queued hover/scrub.
	previewTimer_->stop();
	pendingMs_ = -1;
	setHoverInspect(-1);
	if (fullEdit()) {
		if (timelineView_->durationMs() <= 0)
			return; // empty timeline
		playing_ = true;
		playBtn_->setIcon(uiIcon(Glyph::Pause));
		const qint64 total = timelineView_->durationMs();
		qint64 pos = timelineView_->playhead();
		if (pos < 0 || pos >= total)
			pos = 0;
		playAnchorMs_ = pos; // output-time
		showTimelineFrame(pos); // warm the exact first frame before starting the clock
		timelineView_->setPlayhead(pos);
		syncPreviewTransformTarget(false);
		syncComponentPanel();
		cursorTimeLabel_->setText(previewTimeText(pos));
		playClock_.restart();
		playTimer_->start();
		startPreviewAudio(pos);
		return;
	}
	if (multiCut()) {
		if (tracks_->segments().isEmpty())
			return; // nothing to assemble yet
		playing_ = true;
		playBtn_->setIcon(uiIcon(Glyph::Pause));
		// Start from the marker (output-time); fall back to the start if it's
		// unset or past the end.
		const qint64 total = tracks_->totalOutputMs();
		qint64 pos = tracks_->playhead();
		if (pos < 0 || pos >= total)
			pos = 0;
		playAnchorMs_ = pos;
		playSeg_ = -1;
		playClock_.restart();
		onPlayTick(); // resolve the first source in-point before starting time
		if (!playing_)
			return;
		playClock_.restart();
		playTimer_->start();
		return;
	}
	if (!seeker_)
		return; // Simple Trim with nothing loaded: nothing to play
	playing_ = true;
	playBtn_->setIcon(uiIcon(Glyph::Pause));
	// Start from the marker (source-time), clamped into the trimmed region.
	const qint64 start = timeline_->start(), end = timeline_->end();
	qint64 pos = timeline_->playhead();
	if (pos < start || pos >= end)
		pos = start;
	playAnchorMs_ = pos;
	playClock_.restart();
	onPlayTick();
	if (!playing_)
		return;
	playClock_.restart();
	playTimer_->start();
}

void VideoEditorWindow::openSlip(int track, int clip)
{
	if (!timelineView_ || !timelineView_->canSlip(track, clip))
		return;
	const TimelineModel &m = timelineView_->model();
	const TlClip c = m.tracks[track].clips[clip];
	const EditorSource *s = sourceById(c.sourceId);
	if (!s)
		return;
	// The window plays the clip on its own; the timeline must not play under it.
	if (playing_)
		stopPlayback();

	SlipDialog::Input in;
	in.clip = c;
	for (const auto &lp : timelineView_->slipPartners(track, clip))
		in.partners.append(m.tracks[lp.first].clips[lp.second]);
	in.soundOnly = m.tracks[track].kind == TlTrack::Kind::Audio;
	in.title = s->name.isEmpty() ? QFileInfo(s->path).fileName() : s->name;
	in.decodePath = playbackPath_.value(c.sourceId, s->path); // the proxy, where there is one
	in.sourceSize = QSize(s->width, s->height);
	in.sourceMs = timelineView_->sourceDurationMs(c.sourceId) > 0 ? timelineView_->sourceDurationMs(c.sourceId)
								       : s->durationMs;
	timelineView_->slipRange(track, clip, &in.loMs, &in.hiMs);
	in.thumbs = timelineView_->sourceThumbs(c.sourceId);
	if (in.soundOnly && in.clip.peaks.isEmpty())
		in.clip.peaks = randomClipPeaks(s->path); // the waveform, for a sound
	in.fps = timelineFps();
	// The same mix the timeline plays, from a snapshot of the files: the
	// window mixes off the GUI thread and must not read editor state.
	QMap<int, QString> paths;
	for (const EditorSource &es : sources_)
		paths.insert(es.id, es.path);
	in.mixAudio = [paths](const TimelineModel &mm) {
		return TimelineAudio::mixToBuffer(mm, [&paths](int id) { return paths.value(id); });
	};

	SlipDialog dlg(in, this);
	if (dlg.exec() != QDialog::Accepted || dlg.deltaMs() == 0)
		return;
	const qint64 moved = timelineView_->applySlip(track, clip, dlg.deltaMs());
	if (moved != 0 && infoLabel_)
		infoLabel_->setText((moved > 0 ? QStringLiteral("Slipped: the clip now shows footage %1 s later.")
					       : QStringLiteral("Slipped: the clip now shows footage %1 s earlier."))
					    .arg(std::abs(moved) / 1000.0, 0, 'f', 1));
}

void VideoEditorWindow::stopPlayback()
{
	playing_ = false;
	playBtn_->setIcon(uiIcon(Glyph::Play));
	playTimer_->stop();
	startAudioWhenReady_ = false; // a mix still running must not start on arrival
	if (audioPreview_)
		audioPreview_->stop();
	if (voTrack_ && !voRecording_)
		voTrack_->clearPlayhead();
	// Playback ran out (or was stopped) during a play-along take: with "stop
	// at the end" on, that is the end of the take. Deferred, since
	// finishVoiceover itself calls back in here.
	if (voRecording_ && voPlayedAlong_ && voStopAtEnd_ && voStopAtEnd_->isChecked()) {
		voPlayedAlong_ = false;
		QTimer::singleShot(0, this, [this]() { finishVoiceover(); });
	}
	// A proxy that landed mid-playback is safe to install now.
	flushPendingPlaybackProxies();
	// The extra per-track decoders exist only for playback. Each holds an open
	// file and a decoder's worth of buffers, and a stopped editor left sitting
	// open should not be carrying several of them around; they cost one seek to
	// rebuild on the next Play.
	playSeekers_.clear();
}

void VideoEditorWindow::stopAtTimelineEnd(qint64 endMs)
{
	stopPlayback();
	timelineView_->setPlayhead(endMs);
	cursorTimeLabel_->setText(previewTimeText(endMs));
	syncPreviewTransformTarget(false);
	syncComponentPanel();
	// The marker names the first instant outside the output. Hold the final
	// in-range frame while leaving the playhead exactly on that boundary.
	showTimelineFrame(std::max<qint64>(0, endMs - 1));
}

void VideoEditorWindow::onPlayTick()
{
	if (!playing_ || !valid_)
		return;

	if (fullEdit()) {
		const qint64 total = timelineView_->durationMs();
		if (total <= 0) {
			stopPlayback();
			return;
		}
		// The sound card is the clock whenever it is playing: a wall-clock timer
		// and an audio device drift apart, and drift is audible long before it is
		// visible. Without audio, fall back to the elapsed timer.
		qint64 outPos = playAnchorMs_ + playClock_.elapsed();
		if (audioPreview_ && audioPreview_->isPlaying()) {
			const qint64 apos = audioPreview_->positionMs();
			if (apos >= 0) {
				outPos = apos;
				// Mute, remix or a device pause hands timing back to the wall
				// clock. Continue from the last audible position, not an older
				// wall clock that may have run ahead during device buffering.
				playAnchorMs_ = apos;
				playClock_.restart();
			}
		}
		if (outPos >= total) {
			if (timelineView_->model().hasVideoEnd()) {
				stopAtTimelineEnd(total);
				return;
			}
			// Automatic duration keeps the existing whole-timeline loop.
			playAnchorMs_ = 0;
			playClock_.restart();
			outPos = 0;
			if (audioPreview_)
				audioPreview_->start(0); // restart the sound with the picture
		}
		timelineView_->setPlayhead(outPos);
		cursorTimeLabel_->setText(previewTimeText(outPos));
		syncPreviewTransformTarget(false); // keyframed transform/mask grips follow the picture
		syncComponentPanel();
		showTimelineFrame(outPos); // composites every visible track + text
		return;
	}

	if (multiCut()) {
		const qint64 total = tracks_->totalOutputMs();
		if (total <= 0) {
			stopPlayback();
			return;
		}
		qint64 outPos = playAnchorMs_ + playClock_.elapsed();
		if (outPos >= total) { // loop the assembled output
			playAnchorMs_ = 0;
			playClock_.restart();
			playSeg_ = -1;
			outPos = 0;
		}
		qint64 srcTarget = 0;
		const int seg = tracks_->sourceForOutput(outPos, &srcTarget);
		if (seg < 0) {
			stopPlayback();
			return;
		}
		const QVector<CutSegment> &segs = tracks_->segments();
		// Each source owns its sequential frameAt cache. It resolves a new
		// in-point exactly and holds low-rate frames without visible catch-up.
		FrameSeeker *segSeeker = seekerFor(segs[seg].sourceId);
		if (!segSeeker) {
			stopPlayback();
			return;
		}
		if (seg != playSeg_ || segs[seg].sourceId != playSourceId_) {
			playSeg_ = seg;
			playSourceId_ = segs[seg].sourceId;
		}
		const QSize render = previewRenderSize(multiCutCanvasSize());
		const QImage img = segSeeker->frameAt(srcTarget, render.width(), render.height());
		if (img.isNull()) {
			// Source ended inside this cut — skip to the next segment.
			playAnchorMs_ = tracks_->outputStartOf(seg) + segs[seg].outDurationMs();
			playClock_.restart();
			playSeg_ = -1;
			return;
		}
		setPreviewFrame(fitIntoCanvas(img, render), outPos);
		tracks_->setPlayhead(outPos);
		voTrack_->setPlayhead(outPos);
		cursorTimeLabel_->setText(previewTimeText(srcTarget));
		return;
	}

	if (!seeker_) { // the source was removed mid-play
		stopPlayback();
		return;
	}
	const qint64 start = timeline_->start();
	const qint64 end = timeline_->end();

	qint64 target = playAnchorMs_ + qint64(playClock_.elapsed() * speed_);
	if (target >= end) {
		// Loop back to the start of the trimmed region.
		playAnchorMs_ = start;
		playClock_.restart();
		target = start;
	}

	const QSize render = previewRenderSize(QSize(1280, 720));
	const QImage img = seeker_->frameAt(target, render.width(), render.height());
	if (img.isNull()) { // reached end of file inside the region — loop
		seeker_->seekTo(start);
		playAnchorMs_ = start;
		playClock_.restart();
		return;
	}
	const qint64 ts = seeker_->positionMs();
	setPreviewFrame(img, ts);
	timeline_->setPlayhead(ts);
	{
		const double sp = speed_ > 0.01 ? speed_ : 1.0;
		voTrack_->setPlayhead(std::max<qint64>(0, qint64((ts - timeline_->start()) / sp)));
	}
	cursorTimeLabel_->setText(previewTimeText(ts));
}

void VideoEditorWindow::onSpeedChanged(int sliderValue)
{
	// Slider → exponential speed; mirror the value into the spin box.
	const double value = sliderToSpeed(sliderValue);
	{
		QSignalBlocker bp(speedSpin_);
		speedSpin_->setValue(value);
	}
	applySpeed(value);
}

void VideoEditorWindow::onSpeedSpinChanged(double value)
{
	// Typed value → matching slider position (exponential), then apply.
	{
		QSignalBlocker bs(speedSlider_);
		speedSlider_->setValue(speedToSlider(value));
	}
	applySpeed(value);
}

void VideoEditorWindow::applySpeed(double value)
{
	value = std::clamp(value, kMinSpeed, kMaxSpeed);

	if (multiCut()) {
		// The control edits EVERY selected cut's speed.
		const QList<int> sel = tracks_->selectedIndices();
		if (sel.isEmpty())
			return;
		for (int idx : sel)
			tracks_->setSegmentSpeed(idx, value);
		speedLabel_->setText(sel.size() > 1 ? QStringLiteral("(%1 cuts)").arg(sel.size())
						    : QString());
		if (playing_) {
			// Output durations shifted — re-anchor and re-map on the next tick.
			playAnchorMs_ = std::min(playAnchorMs_ + playClock_.elapsed(),
						 std::max<qint64>(0, tracks_->totalOutputMs() - 1));
			playClock_.restart();
			playSeg_ = -1;
		}
		updateInfoLabel();
		updateInspector();
		updateVoiceoverAxis();
		scheduleSnapshot();
		return;
	}

	// Re-anchor the playback clock so the speed change is seamless.
	if (playing_) {
		playAnchorMs_ = playAnchorMs_ + qint64(playClock_.elapsed() * speed_);
		playClock_.restart();
	}
	speed_ = value;
	updateInspector();
	updateVoiceoverAxis();
	scheduleSnapshot();
}

// ---- Randomise clip order --------------------------------------------------
//
// A tool window rather than a dialog: it stays open beside the timeline while
// you press its button, watch, undo, press again. Nothing in it applies until
// that button (or Ctrl+R) is pressed, and every press is one undo step through
// the editor's ordinary history -- there is no second undo stack to learn.

namespace {
const QString kRandKeep = QStringLiteral("editor/randomize/keep");
const QString kRandSelOnly = QStringLiteral("editor/randomize/selectedOnly");
const QString kRandAvoid = QStringLiteral("editor/randomize/avoidSame");
const QString kRandStrength = QStringLiteral("editor/randomize/strength");
} // namespace

ShuffleOptions VideoEditorWindow::randomizeOptions() const
{
	ShuffleOptions o;
	if (randomPanel_) {
		o.keep = ShuffleOptions::Keep(std::clamp(randomKeep_->checkedId(), 0, 3));
		o.selectedOnly = randomSelectedOnly_->isChecked();
		o.avoidSameOrder = randomAvoidSame_->isChecked();
		o.strength = ShuffleOptions::Strength(std::clamp(randomStrength_->currentIndex(), 0, 2));
		return o;
	}
	// Ctrl+R before the panel has ever been opened this session: the options
	// it would show, which are the ones saved last time.
	QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	o.keep = ShuffleOptions::Keep(std::clamp(st.value(kRandKeep, 0).toInt(), 0, 3));
	o.selectedOnly = st.value(kRandSelOnly, false).toBool();
	o.avoidSameOrder = st.value(kRandAvoid, true).toBool();
	o.strength = ShuffleOptions::Strength(std::clamp(st.value(kRandStrength, 2).toInt(), 0, 2));
	return o;
}

void VideoEditorWindow::openRandomizePanel()
{
	if (!(fullEdit() && timelineView_) && !(multiCut() && tracks_))
		return;
	if (!randomPanel_) {
		QSettings st(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		auto *p = new QWidget(this, Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
		p->setWindowTitle(QStringLiteral("Randomize clips"));
		auto *lay = new QVBoxLayout(p);
		lay->setContentsMargins(12, 10, 12, 10);
		lay->setSpacing(6);

		lay->addWidget(infoHint(QStringLiteral("Shuffle the order of the clips. Nothing about a clip "
							"changes but where it sits; the edit stays exactly as long."), p), 0, Qt::AlignLeft);

		auto *keepLbl = new QLabel(QStringLiteral("Keep in place"), p);
		keepLbl->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed;"));
		lay->addWidget(keepLbl);
		randomKeep_ = new QButtonGroup(p);
		const char *keepNames[] = {"Nothing: fully random", "The first clip (the intro)",
					   "The last clip (the ending)", "The first and the last"};
		const int keepSaved = std::clamp(st.value(kRandKeep, 0).toInt(), 0, 3);
		for (int i = 0; i < 4; ++i) {
			auto *rb = new QRadioButton(QString::fromLatin1(keepNames[i]), p);
			rb->setChecked(i == keepSaved);
			randomKeep_->addButton(rb, i);
			lay->addWidget(rb);
		}

		randomSelectedOnly_ = new QCheckBox(QStringLiteral("Only the selected clips"), p);
		randomSelectedOnly_->setToolTip(QStringLiteral(
			"Shuffle just the clips you have selected, among their own slots. Everything "
			"else stays where it is. Ctrl+click adds clips to the selection."));
		randomSelectedOnly_->setChecked(st.value(kRandSelOnly, false).toBool());
		lay->addWidget(randomSelectedOnly_);

		randomAvoidSame_ = new QCheckBox(QStringLiteral("Avoid the same order as now"), p);
		randomAvoidSame_->setToolTip(QStringLiteral(
			"Retry until the result is meaningfully different from the current order. "
			"Otherwise a shuffle of a few clips will sometimes hand the same order back."));
		randomAvoidSame_->setChecked(st.value(kRandAvoid, true).toBool());
		lay->addWidget(randomAvoidSame_);

		auto *form = new QFormLayout;
		form->setContentsMargins(0, 2, 0, 0);
		form->setHorizontalSpacing(10);
		randomStrength_ = new QComboBox(p);
		randomStrength_->addItem(QStringLiteral("Low: a few neighbours trade places"));
		randomStrength_->addItem(QStringLiteral("Medium: about half the clips move"));
		randomStrength_->addItem(QStringLiteral("High: a completely different order"));
		randomStrength_->setCurrentIndex(std::clamp(st.value(kRandStrength, 2).toInt(), 0, 2));
		form->addRow(QStringLiteral("Strength"), randomStrength_);
		lay->addLayout(form);

		lay->addWidget(infoHint(QStringLiteral(
			"Which lanes: the one holding the selected clip or header; with nothing selected, "
			"every video lane, each on its own. Locked lanes are never touched."), p), 0, Qt::AlignLeft);

		auto *go = new QPushButton(QStringLiteral("Randomize  (Ctrl+R)"), p);
		go->setDefault(true);
		go->setMinimumHeight(32);
		connect(go, &QPushButton::clicked, this, [this]() { randomizeClips(); });
		lay->addWidget(go);

		randomStatus_ = new QLabel(QStringLiteral("Ctrl+Z brings the previous order back."), p);
		randomStatus_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
		randomStatus_->setWordWrap(true);
		lay->addWidget(randomStatus_);

		// Every option is remembered the moment it changes, so Ctrl+R with the
		// panel closed -- or next session -- does what the panel last said.
		auto persist = [this]() {
			QSettings s(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
			s.setValue(kRandKeep, randomKeep_->checkedId());
			s.setValue(kRandSelOnly, randomSelectedOnly_->isChecked());
			s.setValue(kRandAvoid, randomAvoidSame_->isChecked());
			s.setValue(kRandStrength, randomStrength_->currentIndex());
		};
		connect(randomKeep_, &QButtonGroup::idClicked, this, [persist](int) { persist(); });
		connect(randomSelectedOnly_, &QCheckBox::toggled, this, [persist](bool) { persist(); });
		connect(randomAvoidSame_, &QCheckBox::toggled, this, [persist](bool) { persist(); });
		connect(randomStrength_, &QComboBox::currentIndexChanged, this, [persist](int) { persist(); });

		p->adjustSize();
		p->setFixedWidth(std::max(320, p->sizeHint().width()));
		randomPanel_ = p;
	}
	// Beside the button that opened it, not over the timeline it acts on.
	if (!randomPanel_->isVisible() && randomBtn_) {
		const QPoint below = randomBtn_->mapToGlobal(QPoint(0, randomBtn_->height() + 6));
		randomPanel_->move(below);
	}
	randomPanel_->show();
	randomPanel_->raise();
	randomPanel_->activateWindow();
}

void VideoEditorWindow::randomizeClips(bool selectedOnly)
{
	if (multiCut() && tracks_) {
		randomizeCuts();
		return;
	}
	if (!fullEdit() || !timelineView_)
		return;
	ShuffleOptions o = randomizeOptions();
	// From the clip menu: the right-clicked selection is the target whatever
	// the panel's checkbox says.
	o.selectedOnly = o.selectedOnly || selectedOnly;
	int total = 0;
	for (const TlTrack &t : timelineView_->model().tracks)
		if (t.kind == TlTrack::Kind::Video)
			total += t.clips.size();
	// One call, one editCommitted, one undo step -- the timeline's own signal
	// is what the history listens to, so Ctrl+Z here is the same Ctrl+Z as
	// everywhere else in Full editing. A press that changed nothing records
	// nothing, and says so.
	const int moved = timelineView_->shuffleClips(o);
	if (randomStatus_) {
		if (moved == 0)
			randomStatus_->setText(o.selectedOnly
						       ? QStringLiteral("Nothing moved: select two or more clips first.")
						       : QStringLiteral("Nothing moved: no lane with two clips to shuffle "
									"(a locked lane is left alone)."));
		else
			randomStatus_->setText(QStringLiteral("Moved %1 of %2 clips. Ctrl+Z brings the previous "
							      "order back; Ctrl+R shuffles again.")
						       .arg(moved)
						       .arg(total));
	}
	if (moved > 0)
		showTimelineFrame(timelinePlayheadMs()); // the frame under the playhead is a different clip now
}

void VideoEditorWindow::randomizeCuts()
{
	// Multi-Cut: the output is the cuts played back to back, so a shuffle is a
	// new order of the cut list -- the selected cuts when two or more are
	// selected, otherwise all of them. The track editor does it (the same
	// method its right-click menu uses) and reports it through
	// segmentsChanged, which refreshes the window; this closes the undo step.
	const int n = tracks_->segments().size();
	const bool selOnly = tracks_->selectedIndices().size() >= 2;
	const int moved = tracks_->shuffle(randomizeOptions());
	if (moved == 0) {
		const QString why = n < 2 ? QStringLiteral("Nothing moved: make at least two cuts first.")
					  : QStringLiteral("Nothing moved: the options keep every cut in place.");
		if (randomStatus_)
			randomStatus_->setText(why);
		if (infoLabel_)
			infoLabel_->setText(why);
		return;
	}
	commitSnapshot(); // one undo step, now rather than after the coalescing delay
	{
		qint64 srcMs = 0;
		const int seg = tracks_->sourceForOutput(std::max<qint64>(0, tracks_->playhead()), &srcMs);
		if (seg >= 0)
			showFrame(tracks_->segments()[seg].sourceId, srcMs);
	}
	const QString msg = QStringLiteral("Shuffled %1 of %2 cuts%3. Ctrl+Z brings the previous order back; "
					   "Ctrl+R shuffles again.")
				    .arg(moved)
				    .arg(n)
				    .arg(selOnly ? QStringLiteral(" (the selected ones)") : QString());
	if (randomStatus_)
		randomStatus_->setText(msg);
	if (infoLabel_)
		infoLabel_->setText(msg);
}

void VideoEditorWindow::stepSelectedSpeed(double delta)
{
	if (!multiCut() || !tracks_ || delta == 0.0)
		return;
	const QList<int> sel = tracks_->selectedIndices();
	if (sel.isEmpty())
		return;
	const QVector<CutSegment> &segs = tracks_->segments();
	// Each cut steps from ITS OWN speed. Rounded to the step's grid so a run
	// of presses lands on 1.2, 1.4, 1.6 rather than accumulating 1.2000001s,
	// and because a cut that was hand-set to 1.37 is more useful at 1.4 than
	// at 1.57 -- the keys are for round numbers, the spin box for exact ones.
	const double grid = std::abs(delta) > 0.0 ? std::abs(delta) : 0.2;
	double primary = -1.0;
	for (int idx : sel) {
		if (idx < 0 || idx >= segs.size())
			continue;
		double sp = segs[idx].speed + delta;
		sp = std::round(sp / grid) * grid;
		sp = std::clamp(sp, kMinSpeed, kMaxSpeed);
		tracks_->setSegmentSpeed(idx, sp);
		if (idx == tracks_->selectedIndex())
			primary = sp;
	}
	if (primary < 0.0 && !sel.isEmpty() && sel.front() < segs.size())
		primary = tracks_->segments()[sel.front()].speed;
	// The slider and the spin box show the primary cut, like after a click.
	if (primary > 0.0)
		syncSpeedControls(primary);
	speedLabel_->setText(sel.size() > 1 ? QStringLiteral("(%1 cuts)").arg(sel.size())
					    : QString());
	if (playing_) {
		// Output durations shifted -- re-anchor and re-map on the next tick,
		// exactly as the slider does.
		playAnchorMs_ = std::min(playAnchorMs_ + playClock_.elapsed(),
					 std::max<qint64>(0, tracks_->totalOutputMs() - 1));
		playClock_.restart();
		playSeg_ = -1;
	}
	updateInfoLabel();
	updateInspector();
	updateVoiceoverAxis();
	// A key press is a discrete edit: one undo step each, rather than a run of
	// presses coalescing into one the way a slider drag does.
	commitSnapshot();
}

// ---- The shaders folder --------------------------------------------------
//
// The chain that used to be applied here is gone; shaders are components, and
// the component loader scans this folder. Creating and seeding it stays, so a
// fresh install still has examples to add.

QString VideoEditorWindow::shadersDirPath()
{
	if (shadersDir_.isEmpty()) {
		const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
		shadersDir_ = base + QStringLiteral("/harpia/shaders");
	}
	QDir().mkpath(shadersDir_);
	// Seed the bundled shaders when missing so there are always examples to start
	// from (per-file, so user edits are never overwritten and new bundled shaders
	// arrive on the next launch after an update).
	for (const QString &name :
	     {QStringLiteral("adjust"), QStringLiteral("crt"), QStringLiteral("grayscale"),
	      QStringLiteral("vignette"), QStringLiteral("sharpen"), QStringLiteral("grain"),
	      QStringLiteral("pixelate"), QStringLiteral("backdrop"), QStringLiteral("solid"),
	      QStringLiteral("gradient"), QStringLiteral("huecycle"), QStringLiteral("coloroverlay")}) {
		const QString dst = shadersDir_ + QLatin1Char('/') + name + QStringLiteral(".frag");
		if (QFile::exists(dst))
			continue;
		QFile res(QStringLiteral(":/shaders/") + name + QStringLiteral(".frag"));
		if (res.open(QIODevice::ReadOnly)) {
			QFile out(dst);
			if (out.open(QIODevice::WriteOnly))
				out.write(res.readAll());
		}
	}
	return shadersDir_;
}

// The preview shows the frame the compositor produced, with nothing applied
// after it. A shader component runs INSIDE the compositor, which is what makes
// the preview and the export identical -- the post-pass that used to sit here
// existed only on the preview side and had to be reimplemented in the exporter.
void VideoEditorWindow::setPreviewFrame(const QImage &img, qint64 ms)
{
	lastPreviewRaw_ = img;
	lastPreviewMs_ = ms;
	canvas_->setFrame(img);
}

void VideoEditorWindow::refreshPreviewFrame()
{
	if (canvas_ && !lastPreviewRaw_.isNull())
		canvas_->setFrame(lastPreviewRaw_);
}

// Render whatever position is pending, right now.
void VideoEditorWindow::renderPendingPreview()
{
	if (pendingMs_ < 0)
		return;
	const qint64 ms = pendingMs_;
	const int src = pendingSource_;
	pendingMs_ = -1; // consumed before rendering, so a request made DURING the
			 // decode is seen as new rather than swallowed
	// src == -1 means "composite the whole timeline at this OUTPUT time"
	// (Full editing); otherwise it's a single source frame.
	if (src < 0)
		showTimelineFrame(ms);
	else
		showFrame(src, ms);
}

// Ask for a preview at `ms`. The first request renders immediately, so a scrub
// tracks the cursor from the very first pixel; the ones behind it are rate
// limited to the timer's interval and only the NEWEST survives, so a fast drag
// never queues up stale frames it would have to catch up on.
//
// The timer must never be restarted while requests keep arriving -- restarting a
// single-shot timer on every mouse-move is exactly how the preview ends up
// frozen until the mouse stops.
void VideoEditorWindow::requestPreview(int sourceId, qint64 ms)
{
	pendingSource_ = sourceId;
	pendingMs_ = ms;
	if (previewTimer_->isActive())
		return; // still cooling down; the tick will pick up this position
	renderPendingPreview();
	previewTimer_->start(); // cooldown before the next one
}

void VideoEditorWindow::onPreviewTick()
{
	if (pendingMs_ < 0)
		return; // nothing new arrived while cooling down
	renderPendingPreview();
	previewTimer_->start(); // keep pacing while the drag continues
}

// Re-render whatever the preview is currently showing (after a change that
// alters HOW it is rendered rather than what).
void VideoEditorWindow::refreshPreviewAtPlayhead()
{
	if (!valid_)
		return;
	if (fullEdit()) {
		showTimelineFrame(timelinePlayheadMs());
		return;
	}
	showFrame(activeSourceId_, std::max<qint64>(0, lastPreviewMs_));
}

void VideoEditorWindow::showFrame(int sourceId, qint64 ms)
{
	if (!valid_)
		return;
	previewRenderWarning_->clearFailure();
	if (!sourceById(sourceId))
		sourceId = activeSourceId_; // fall back to the active source
	// Trim / Multi-Cut show one source frame; the same quality setting applies,
	// against the 720p these modes have always previewed at.
	//
	// Multi-Cut decodes into its own canvas SHAPE, reduced to preview size. It
	// used to decode into a 1280x720 box and then letterbox into
	// multiCutCanvasSize(), which is the primary source's REAL size -- 3840x2160
	// for a 4K project. That built a 33 MB image with a smooth rescale on every
	// scrubbed frame, to be drawn into a widget a fraction of the size:
	// 24.4 ms a frame measured, against 1.1 ms at preview scale.
	const QSize mcCanvas = multiCut() ? previewRenderSize(multiCutCanvasSize()) : QSize();
	const QSize dec = multiCut() ? mcCanvas : previewRenderSize(QSize(1280, 720));
	shownSource_ = sourceId;
	shownMs_ = ms;

	bool exact = false;
	QImage img;
	if (previewDecoder_)
		img = previewDecoder_->frame(sourceId, ms, dec.width(), dec.height(), &exact);
	shownExact_ = exact;
	if (img.isNull())
		return; // nothing decoded for this source yet -- keep the last picture
			// on screen rather than flashing black while the worker catches up
	// Multi-Cut: a cut from a differently-shaped file goes into the output
	// letterboxed, so it has to preview that way too. A no-op when every source
	// is the same shape, which is nearly every project. The bars land in the
	// same proportions whatever size the canvas is, so doing it at preview scale
	// shows the same framing for a twentieth of the work.
	if (multiCut() && mcCanvas.isValid())
		img = fitIntoCanvas(img, mcCanvas);
	setPreviewFrame(img, ms);
}

// The proxy for a source is on disk: point the preview decoder at it and redraw.
// Nothing else changes -- the timeline, the model and the exporter all still
// refer to the original file, which is the only one that will be encoded.
void VideoEditorWindow::onProxyReady(int sourceId, const QString &proxyPath)
{
	if (!previewDecoder_)
		return;
	previewDecoder_->setProxy(sourceId, proxyPath);
	proxyProgress_.remove(sourceId);
	proxied_.insert(sourceId);
	// Playback reads the source directly rather than through the decoder, so it
	// has to be pointed at the proxy too or pressing Play on a 4K clip still
	// stutters -- scrubbing was only half of it.
	applyProxyToPlayback(sourceId, proxyPath);
	proxyPending_.remove(sourceId);
	invalidatePreviewCache(); // the pixels now come from the proxy
	startFilmstrip(sourceId, proxyPath);
	if (EditorSource *s = sourceById(sourceId))
		qInfo("harpia: preview proxy ready for %s", qUtf8Printable(s->name));
	updateProxyStatus();
	// The frame on screen came out of the original file; ask for it again so
	// what is shown is what the preview will keep serving from now on.
	shownExact_ = false;
	if (!loadingDialog_)
		refreshPreviewAtPlayhead();
}

// Point a source's own FrameSeeker -- the one playback pulls frames from -- at
// the proxy. Everything that describes the source (its size, its duration, its
// rate, its path) still comes from the original: the proxy is the same video at
// the same times, and only the pixels being decoded change. Export never looks
// here at all; it opens the paths in sources_.
//
// Never while playing. A reopen puts the decoder back at the start, and
// onPlayTick pulls frames in sequence, so swapping underneath it would jump the
// picture. It waits for the next stop instead, which costs nothing: the proxy
// only ever arrives once.
void VideoEditorWindow::applyProxyToPlayback(int sourceId, const QString &proxyPath)
{
	if (playing_ || loadingDialog_) {
		pendingPlaybackProxy_[sourceId] = proxyPath;
		return;
	}
	EditorSource *s = sourceById(sourceId);
	if (!s || !s->seeker)
		return;
	if (!s->seeker->open(proxyPath)) {
		// Fall back to the original rather than leaving a closed seeker behind,
		// which would make playback of this source show nothing at all.
		s->seeker->open(s->path);
		return;
	}
	if (activeSourceId_ == sourceId)
		seeker_ = s->seeker.get(); // same object, but be explicit about it
	// Remember where playback now reads from, and drop the extra per-track
	// decoders so they are rebuilt against the proxy on the next frame. They
	// are cheap to recreate and would otherwise keep chewing through the 4K
	// original while track 0 sails along on the proxy.
	playbackPath_[sourceId] = proxyPath;
	playSeekers_.clear();
}

// Kick off the filmstrip for a source, reading from `fromPath`. TimelineThumbs
// ignores a second start, so calling this after the source already has a strip
// is harmless.
void VideoEditorWindow::startFilmstrip(int sourceId, const QString &fromPath)
{
	EditorSource *s = sourceById(sourceId);
	if (s && s->thumbs && !fromPath.isEmpty())
		s->thumbs->start(fromPath, 60, 128, 72);
}

void VideoEditorWindow::flushPendingPlaybackProxies()
{
	if (pendingPlaybackProxy_.isEmpty())
		return;
	const QHash<int, QString> pend = pendingPlaybackProxy_;
	pendingPlaybackProxy_.clear();
	for (auto it = pend.constBegin(); it != pend.constEnd(); ++it)
		applyProxyToPlayback(it.key(), it.value());
}

void VideoEditorWindow::onProxyProgress(int sourceId, int percent)
{
	if (!proxyProgress_.contains(sourceId))
		return; // finished or removed while this was in flight
	proxyProgress_[sourceId] = percent;
	updateProxyStatus();
}

// One line, in the label the editor already uses for status. Deliberately not a
// modal or a progress dialog: the proxy is an optimisation running behind you,
// and the editor is fully usable against the original while it builds.
void VideoEditorWindow::updateProxyStatus()
{
	if (!infoLabel_)
		return;
	if (proxyProgress_.isEmpty()) {
		if (!proxyStatusShown_)
			return; // never wrote here; leave whatever else did
		proxyStatusShown_ = false;
		infoLabel_->clear();
		return;
	}
	int pct = 100;
	QString name;
	for (auto it = proxyProgress_.constBegin(); it != proxyProgress_.constEnd(); ++it) {
		if (it.value() <= pct) {
			pct = it.value();
			EditorSource *s = sourceById(it.key());
			name = s ? s->name : QString();
		}
	}
	const QString what = proxyProgress_.size() > 1
				     ? QStringLiteral("%1 videos").arg(proxyProgress_.size())
				     : name;
	proxyStatusShown_ = true;
	infoLabel_->setText(
		QStringLiteral("Preparing %1 for smooth scrubbing… %2%").arg(what).arg(pct));
}

// A frame the decode thread was asked for has arrived. Re-render the position
// the preview is trying to show; now that the real frame is in the cache, the
// stale one on screen is replaced. Converges: once everything the render needs
// is exact, no new decode is scheduled and no further frames arrive.
void VideoEditorWindow::onPreviewFrameReady(int sourceId, qint64 ms)
{
	Q_UNUSED(sourceId);
	Q_UNUSED(ms);
	if (loadingDialog_ || !valid_ || playing_ || shownExact_ || shownMs_ < 0)
		return;
	requestPreview(shownSource_, shownMs_);
}

// Delete, from the one shortcut that owns the key. A window-level QShortcut
// beats the focused widget to it, so the routing that focus would have done has
// to happen here instead. The rule itself lives in DeleteRouting.hpp, where it
// can be read and tested in one piece.
void VideoEditorWindow::deleteSelection()
{
	if (!valid_)
		return;
	const QWidget *f = QApplication::focusWidget();
	DeleteContext ctx;
	// Qt normally lets an editable widget veto a shortcut via ShortcutOverride,
	// so this should be belt and braces -- but the cost of being wrong is a clip
	// silently deleted while renaming one, and that is not a bet worth taking on
	// every custom widget in the window behaving.
	ctx.editingText = qobject_cast<const QLineEdit *>(f) ||
			  qobject_cast<const QAbstractSpinBox *>(f) ||
			  qobject_cast<const QPlainTextEdit *>(f) ||
			  qobject_cast<const QTextEdit *>(f) || qobject_cast<const QComboBox *>(f);
	ctx.voiceoverFocused = voTrack_ && f == voTrack_;
	ctx.voiceoverHasSel = voTrack_ && voTrack_->selectedIndex() >= 0;
	ctx.keyListFocused = keyList_ && keyList_->isVisible() && f && (f == keyList_ || keyList_->isAncestorOf(f));
	ctx.keyListHasSel = keyList_ && keyList_->currentRow() >= 0;
	ctx.pathKeyPicked = pathKeyPicked_ >= 0;
	ctx.fullEdit = fullEdit();
	ctx.timelineHasSel = timelineView_ && timelineView_->hasSelection();
	ctx.multiCut = multiCut();
	ctx.multiCutHasSel = tracks_ && tracks_->selectedIndex() >= 0;

	switch (deleteTargetFor(ctx)) {
	case DeleteTarget::VoiceoverTake:
		voTrack_->removeSelected();
		break;
	case DeleteTarget::ListKeyframe:
		keyRowAction(keyList_->currentRow(), KeyRowAction::Delete);
		break;
	case DeleteTarget::PathKeyframe:
		deleteMotionPathKey(pathKeyPicked_);
		break;
	case DeleteTarget::TimelineClips:
		timelineView_->deleteSelected();
		break;
	case DeleteTarget::MultiCutSegment:
		tracks_->removeSelected();
		break;
	case DeleteTarget::TextCursor:
	case DeleteTarget::Nothing:
		break;
	}
}

void VideoEditorWindow::onCropToggled(bool on)
{
	canvas_->setCropEnabled(on);
}

void VideoEditorWindow::onExportRangeRequested(qint64 fromMs, qint64 toMs)
{
	// Deliberately the same door as the Export button: one dialog, one set of
	// settings, one code path that builds the Options. The only difference is
	// that this one arrives with the range already chosen.
	pendingExportSpan_ = TlSpan{fromMs, toMs};
	onSave();
}

void VideoEditorWindow::onSave()
{
	if (!valid_ || exporter_ || batch_) // ignore while an export is already running
		return;
	stopPlayback();

	// What the dialog may offer as an alternative to the whole timeline. Taken
	// from the clip menu when it came from there, otherwise from whatever is
	// selected — pressing Export offers the selection, it does not assume it.
	const bool spanFromMenu = pendingExportSpan_.isValid();
	TlSpan span = pendingExportSpan_;
	pendingExportSpan_ = TlSpan(); // consumed, whichever way this call ends
	if (fullEdit() && !span.isValid())
		span = timelineView_->selectionSpan();

	if (fullEdit() && timelineView_->model().isEmpty()) {
		QMessageBox::information(
			this, QStringLiteral("Export"),
			QStringLiteral("The timeline is empty — add a clip before exporting."));
		return;
	}

	const bool cuts = multiCut();
	if (cuts && tracks_->segments().isEmpty()) {
		QMessageBox::information(
			this, QStringLiteral("Multi-Cut"),
			QStringLiteral("Drag on the Source track to create at least one cut first."));
		return;
	}

	const QString base = QFileInfo(inPath_).completeBaseName() + QStringLiteral("_clip");

	// Everything the dialog needs to describe what is about to be written. It
	// used to be told only a default name, which is why it could not show a
	// duration, a resolution or a size.
	ExportOptionsDialog::Context ec;
	ec.defaultName = base;
	ec.defaultFolder = QFileInfo(inPath_).absolutePath();
	ec.allowGif = !cuts;
	if (fullEdit()) {
		ec.sourceSize = timelineCanvasSize();
		ec.fps = timelineFps();
		ec.seconds = timelineView_->durationMs() / 1000.0;
		ec.previewFrame = canvas_->currentFrame();
		// Offer the chosen range when its bounds differ from the full output.
		// An explicit selection can include clips past the video-end marker.
		span.fromMs = std::max<qint64>(0, span.fromMs);
		span.toMs = std::min(span.toMs, timelineView_->model().contentDurationMs());
		// Text variations: every combination is a version (Variations tab).
		{
			const QVector<text_variations::Slot> vs = text_variations::slotsOf(
				timelineView_->model(), [this](int id) { return sourceLabel(id); });
			const qint64 n = text_variations::combinationCount(vs);
			constexpr qint64 kListMax = 2000;
			if (n > kListMax)
				QMessageBox::information(
					this, QStringLiteral("Variations"),
					QStringLiteral("The Random texts and clips make %1 versions; the Variations tab "
						       "lists the first %2.")
						.arg(n)
						.arg(kListMax));
			for (qint64 i = 0; i < std::min(n, kListMax); ++i) {
				QStringList parts;
				for (const QString &t : text_variations::textsAt(vs, text_variations::combinationAt(vs, i)))
					parts << rich_text::plainText(t).simplified();
				ec.variantLabels << parts.join(QStringLiteral("  ·  "));
			}
		}
		if (span.isValid() && (span.fromMs != 0 || span.toMs != timelineView_->durationMs())) {
			ec.rangeSeconds = span.durationMs() / 1000.0;
			ec.rangeLabel = QStringLiteral("Selection  (%1 – %2)")
						.arg(timeTextCentis(span.fromMs),
						     timeTextCentis(span.toMs));
			ec.rangeDefault = spanFromMenu;
			// A distinct name, so an excerpt cannot quietly overwrite the
			// full export sitting in the same folder.
			ec.rangeName = base + QStringLiteral("_part");
		} else {
			span = TlSpan();
		}
	} else if (cuts) {
		// Through the same answer the preview uses, so the dialog cannot quote
		// one resolution while the picture shows another.
		ec.sourceSize = multiCutCanvasSize();
		ec.fps = !sources_.empty() && sources_.front().seeker
			 ? sources_.front().seeker->fps() : 30.0;
		ec.seconds = tracks_->totalOutputMs() / 1000.0;
		ec.previewFrame = canvas_->currentFrame();
		// The Batch tab: each cut to its own file.
		for (const CutSegment &cs : tracks_->segments())
			ec.batchCuts.push_back({cs.srcStartMs, cs.srcEndMs, cs.speed});
	} else {
		const EditorSource *s0 = activeSource();
		// The cropped area IS the output for a trim export, so the summary has
		// to show that rather than the source's full size.
		const QRect cr = canvas_->cropRectVideo();
		ec.sourceSize = (cropToggle_->isChecked() && cr.width() > 1)
					? cr.size()
					: (s0 ? QSize(s0->width, s0->height) : QSize());
		ec.fps = seeker_ ? seeker_->fps() : 30.0;
		ec.seconds = std::max<qint64>(0, timeline_->end() - timeline_->start()) /
			     1000.0 / std::max(0.01, speed_);
		ec.previewFrame = canvas_->currentFrame();
	}


	ExportOptionsDialog dlg(ec, this);
	if (dlg.exec() != QDialog::Accepted)
		return;

	const ClipExporter::Format fmt = dlg.format();
	outPath_ = dlg.outputPath();

	ClipExporter::Options o;
	o.format = fmt;
	o.startMs = timeline_->start();
	o.endMs = timeline_->end();
	o.crop = cropToggle_->isChecked();
	const QRect c = canvas_->cropRectVideo();
	o.cropX = c.x();
	o.cropY = c.y();
	o.cropW = c.width();
	o.cropH = c.height();
	o.gifFps = dlg.gifFps();
	o.gifWidth = 0; // the chosen size drives it, via Options::outWidth
	o.gifColors = dlg.gifColors();
	o.gifDither = dlg.gifDither();
	o.gifLoop = dlg.gifLoop();
	const QSize outSize = dlg.outputSize();
	if (fmt != ClipExporter::Format::Gif)
		o.outFps = dlg.frameRate(); // 0 = the original
	o.outWidth = outSize.width();
	o.outHeight = outSize.height();
	o.speed = speed_;   // from the editor's speed slider
	o.videoCrf = dlg.videoCrf();
	o.effort = dlg.effort();
	o.chroma444 = dlg.chroma444();
	o.keepAudio = dlg.keepAudio();
	if (cuts && !sources_.empty()) {
		// Multi-cut assembly: the cut list replaces trim range + global speed.
		// inputs[0] is always the primary (canvas) source; other used sources
		// follow. When only the primary is used, inputs stays empty so the
		// single-input export path runs unchanged.
		//
		// Guarded on the media pool: a project can now have clips and no
		// sources at all (a caption, a still, an effect), and front() on an
		// empty vector is undefined behaviour rather than an empty answer.
		const int primaryId = sources_.front().id;
		std::vector<int> order{primaryId}; // source ids; index 0 = canvas source
		auto indexOf = [&](int sid) -> int {
			for (size_t i = 0; i < order.size(); ++i)
				if (order[i] == sid)
					return int(i);
			order.push_back(sid);
			return int(order.size() - 1);
		};
		for (const CutSegment &cs : tracks_->segments()) {
			ClipExporter::Cut cut;
			cut.startMs = cs.srcStartMs;
			cut.endMs = cs.srcEndMs;
			cut.speed = cs.speed;
			cut.source = indexOf(cs.sourceId);
			o.cuts.push_back(cut);
		}
		if (order.size() > 1) { // more than the primary → multi-source export
			for (int sid : order)
				if (EditorSource *es = sourceById(sid))
					o.inputs.push_back(es->path.toStdString());
		}
		o.startMs = 0;
		o.endMs = 0;
		o.speed = 1.0;
	}

	// Batch: the same cut list, one file per selected cut, through the batch
	// runner. Voiceover is skipped -- it is positioned on the JOINED output
	// and has no place on a single cut.
	if (cuts && dlg.batchMode()) {
		startBatchExport(o, dlg.batchSelection(), dlg.batchPrefix(), dlg.folder(), dlg.batchOpenFolder());
		return;
	}

	// Full editing: hand over the whole timeline. It replaces the trim range and
	// the cut list — the exporter composites every visible track per frame.
	if (fullEdit()) {
		// A range export is the SAME render through the same code, handed a
		// timeline that has been cut down to the chosen window and rebased to
		// zero. Nothing downstream — the compositor, the audio mix, the
		// progress arithmetic — has to know a range was involved.
		o.timeline = (dlg.exportRange() && span.isValid())
				     ? sliceTimeline(timelineView_->model(), span.fromMs, span.toMs)
				     : timelineView_->model();
		for (const EditorSource &es : sources_)
			o.timelineSources[es.id] = es.path.toStdString();
		// Ship each referenced script's SOURCE so the worker compiles its own
		// engine without touching the scripts folder mid-render.
		for (const TlTrack &t : o.timeline.tracks)
			for (const TlClip &c : t.clips)
				for (const TlScript &s : c.scripts) {
					if (s.name.isEmpty() || o.timelineScripts.contains(s.name))
						continue;
					QFile sf(scriptsDirPath() + QLatin1Char('/') + s.name +
						 QStringLiteral(".js"));
					if (sf.open(QIODevice::ReadOnly))
						o.timelineScripts.insert(s.name,
									 QString::fromUtf8(sf.readAll()));
				}
		const QSize canvas = timelineCanvasSize();
		o.canvasW = canvas.width();
		o.canvasH = canvas.height();
		// Through the same resolver the preview uses, so the export cannot end
		// up on a different frame rate from the one you were watching.
		o.timelineFps = timelineFps();
		o.cuts.clear();
		o.inputs.clear();
		o.crop = false;
		o.startMs = 0;
		o.endMs = 0;
		o.speed = 1.0;
	}

	// Voiceover: mixed onto the finished output (skipped for GIF, which has no
	// audio). Each take's output-time position + volume + fades carry through.
	if (fmt != ClipExporter::Format::Gif && !voTrack_->isEmpty()) {
		for (const VoiceoverClip &vc : voTrack_->clips())
			o.voiceovers.push_back({vc.path, vc.outStartMs, vc.srcStartMs, vc.durationMs,
						vc.volume, vc.fadeInMs, vc.fadeOutMs});
		o.originalVolume = voOrigVol_->value() / 100.0;
		o.duckOriginal = voDuck_->isChecked();
	}

	// Text variations: the same render, once per ticked version, with only the
	// words swapped. Built from the WHOLE project (the Selection scope is a
	// Single-video choice); voiceover and everything else as set above.
	if (fullEdit() && dlg.batchMode()) {
		const TimelineModel whole = timelineView_->model();
		const QVector<text_variations::Slot> vs =
			text_variations::slotsOf(whole, [this](int id) { return sourceLabel(id); });
		const qint64 total = text_variations::combinationCount(vs);
		const QVector<int> picked = dlg.batchSelection();
		if (picked.size() > 50 &&
		    QMessageBox::question(this, QStringLiteral("Export variations"),
					  QStringLiteral("This renders %1 videos of %2 s each. Continue?")
						  .arg(picked.size())
						  .arg(dlg.exportSeconds() / std::max<qsizetype>(1, picked.size()), 0, 'f', 1),
					  QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
			return;
		const QString prefix = batch_export::safePrefix(dlg.batchPrefix());
		QVector<BatchExporter::Item> items;
		for (int k : picked) {
			const QVector<int> pick = text_variations::combinationAt(vs, k);
			BatchExporter::Item it;
			// Numbered as listed, so a file's number is its version whichever
			// others were ticked.
			it.name = text_variations::fileName(prefix, k + 1, int(std::min<qint64>(total, 1 << 30)),
							    text_variations::textsAt(vs, pick));
			it.useTimeline = true;
			it.timeline = text_variations::applyCombination(whole, vs, pick);
			items.push_back(it);
		}
		o.timeline = whole;
		startBatchItems(o, items, dlg.batchPrefix(), dlg.folder(), dlg.batchOpenFolder());
		return;
	}

	// Nothing to bake here any more: a shader is a component, so it runs inside
	// the compositor the exporter already shares with the preview.

	exporter_ = new ClipExporter(this);
	connect(exporter_, &ClipExporter::progress, this, &VideoEditorWindow::onExportProgress);
	connect(exporter_, &ClipExporter::finished, this, &VideoEditorWindow::onExportFinished);

	progress_ = new ExportProgressDialog(outPath_, exportSettingsSummary(o), this);
	connect(exporter_, &ClipExporter::stageChanged, progress_, &ExportProgressDialog::setStage);
	connect(exporter_, &ClipExporter::frameProgress, progress_, &ExportProgressDialog::setFrameProgress);
	connect(progress_, &ExportProgressDialog::cancelRequested, this, [this]() {
		if (exporter_)
			exporter_->cancel();
	});
	progress_->show(); // ensure the modal progress is visible immediately

	exportThread_ = std::thread([this, o]() { exporter_->run(inPath_, outPath_, o); });
}

QString VideoEditorWindow::exportSettingsSummary(const ClipExporter::Options &o) const
{
	const FrameSeeker *source = seeker_;
	if (!o.cuts.empty() && !sources_.empty() && sources_.front().seeker)
		source = sources_.front().seeker.get();
	QSize size(o.outWidth, o.outHeight);
	if (size.isEmpty()) {
		if (o.canvasW > 0 && o.canvasH > 0)
			size = QSize(o.canvasW, o.canvasH);
		else if (o.crop && o.cropW > 0 && o.cropH > 0)
			size = QSize(o.cropW, o.cropH);
		else if (source)
			size = QSize(source->width(), source->height());
	}
	const bool gif = o.format == ClipExporter::Format::Gif;
	double fps = gif ? o.gifFps : o.outFps;
	if (fps <= 0.0)
		fps = o.canvasW > 0 ? o.timelineFps : (source ? source->fps() : 0.0);
	QStringList parts{ClipExporter::extensionFor(o.format).toUpper()};
	if (!size.isEmpty())
		parts << QStringLiteral("%1 × %2").arg(std::max(2, size.width() & ~1))
						.arg(std::max(2, size.height() & ~1));
	if (fps > 0.0)
		parts << QStringLiteral("%1 fps").arg(fps, 0, 'f', qFuzzyCompare(fps, std::round(fps)) ? 0 : 2);
	if (gif) {
		parts << QStringLiteral("%1 colors").arg(o.gifColors);
	} else {
		parts << (o.videoCrf == 0 ? QStringLiteral("Lossless") : QStringLiteral("Quality CRF %1").arg(o.videoCrf));
		switch (o.effort) {
		case ClipExporter::Options::Effort::Fast: parts << QStringLiteral("Fast encoding"); break;
		case ClipExporter::Options::Effort::Balanced: parts << QStringLiteral("Balanced encoding"); break;
		case ClipExporter::Options::Effort::Best: parts << QStringLiteral("Best compression"); break;
		}
	}
	return parts.join(QStringLiteral("  ·  "));
}

void VideoEditorWindow::startBatchExport(const ClipExporter::Options &base, const QVector<int> &selection,
					 const QString &prefix, const QString &baseFolder, bool openFolder)
{
	QVector<BatchExporter::Item> items;
	const int n = selection.size();
	for (int k = 0; k < n; ++k) {
		const int i = selection[k];
		if (i < 0 || i >= int(base.cuts.size()))
			continue;
		BatchExporter::Item it;
		it.name = batch_export::clipName(prefix, k + 1, n);
		it.cut = base.cuts[size_t(i)];
		items.push_back(it);
	}
	startBatchItems(base, items, prefix, baseFolder, openFolder);
}

void VideoEditorWindow::startBatchItems(const ClipExporter::Options &base,
					const QVector<BatchExporter::Item> &items, const QString &prefix,
					const QString &baseFolder, bool openFolder)
{
	if (items.isEmpty())
		return;
	const QString folder = batch_export::uniqueFolder(baseFolder, prefix);
	batchOpenFolder_ = openFolder;
	batchTotal_ = items.size();

	batch_ = new BatchExporter(this);
	connect(batch_, &BatchExporter::progress, this, &VideoEditorWindow::onBatchProgress);
	connect(batch_, &BatchExporter::finished, this, &VideoEditorWindow::onBatchFinished);

	const QString firstPath = QDir(folder).filePath(items.front().name + QLatin1Char('.') +
						      ClipExporter::extensionFor(base.format));
	batchProgress_ = new ExportProgressDialog(firstPath, exportSettingsSummary(base), this);
	batchProgress_->setItemProgress(0, items.size());
	connect(batch_, &BatchExporter::stageChanged, batchProgress_, &ExportProgressDialog::setStage);
	connect(batch_, &BatchExporter::frameProgress, batchProgress_, &ExportProgressDialog::setFrameProgress);
	connect(batch_, &BatchExporter::outputBytes, batchProgress_, &ExportProgressDialog::setBytes);
	connect(batch_, &BatchExporter::itemStarted, batchProgress_,
		[panel = batchProgress_](int index, int count, const QString &path) {
			panel->setItemProgress(index, count);
			panel->setOutputPath(path);
		});
	connect(batchProgress_, &ExportProgressDialog::cancelRequested, this, [this]() {
		if (batch_)
			batch_->cancel();
	});
	batchProgress_->show();

	if (!batch_->start(inPath_, base, items, folder)) {
		batchProgress_->finish(false);
		batchProgress_->deleteLater();
		batchProgress_ = nullptr;
		batch_->deleteLater();
		batch_ = nullptr;
		warnAndLog(this, QStringLiteral("Batch export"),
			   QStringLiteral("Could not create the folder\n%1").arg(QDir::toNativeSeparators(folder)));
	}
}

void VideoEditorWindow::startBatchForTest(const QString &folder)
{
	ClipExporter::Options base;
	base.format = ClipExporter::Format::Mp4;
	BatchExporter::Item item;
	item.name = QStringLiteral("test");
	item.cut.startMs = 0;
	item.cut.endMs = 1000;
	startBatchItems(base, {item}, QStringLiteral("test"), folder, false);
}

void VideoEditorWindow::onBatchProgress(int index, int count, int overallPct, qint64 etaMs)
{
	if (!batchProgress_)
		return;
	batchProgress_->setItemProgress(index, count);
	batchProgress_->setProgress(overallPct, etaMs, 0);
}

void VideoEditorWindow::onBatchFinished(int okCount, int failCount, bool canceled)
{
	if (batchProgress_) {
		batchProgress_->finish(!canceled && failCount == 0);
		batchProgress_->deleteLater();
		batchProgress_ = nullptr;
	}
	if (!batch_)
		return;
	const QVector<BatchExporter::Result> results = batch_->results();
	const QString folder = batch_->folder();
	batch_->deleteLater(); // joins its (finished) thread
	batch_ = nullptr;

	QStringList failed;
	for (const BatchExporter::Result &r : results) {
		if (r.ok)
			emit exported(r.path); // the library lists each clip
		else
			failed << QStringLiteral("%1 — %2").arg(r.name, r.error);
	}
	// The total is what was asked for: the clips that finished, plus the one
	// a cancel interrupted, plus the ones after it that never started.
	const int total = batchTotal_;
	const QString head = batch_export::summary(okCount, failCount, total, canceled);
	if (batchOpenFolder_ && okCount > 0)
		QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
	if (failCount > 0 || (canceled && okCount == 0))
		warnAndLog(this, QStringLiteral("Batch export"),
			   head + (failed.isEmpty() ? QString() : QStringLiteral("\n\n") + failed.join(QLatin1Char('\n'))));
	else
		QMessageBox::information(this, QStringLiteral("Batch export"),
					 head + QStringLiteral("\n\n") + QDir::toNativeSeparators(folder));
}

void VideoEditorWindow::onExportProgress(int pct, qint64 etaMs, qint64 bytes)
{
	if (progress_)
		progress_->setProgress(pct, etaMs, bytes);
}

void VideoEditorWindow::onExportFinished(bool ok, bool canceled, const QString &err)
{
	joinExport();
	if (canceled || !ok)
		QFile::remove(outPath_); // cleanup completes before the progress panel closes
	if (progress_) {
		progress_->finish(ok && !canceled);
		progress_->deleteLater();
		progress_ = nullptr;
	}
	if (exporter_) {
		exporter_->deleteLater();
		exporter_ = nullptr;
	}

	if (canceled || !ok) {
		if (!canceled) {
			const QString report = err.isEmpty() ? QStringLiteral("Could not export the clip.") : err;
			editorLog(EditorLogLevel::Error, QStringLiteral("Export failed"), report);
			QMessageBox box(QMessageBox::Critical, QStringLiteral("Export failed"),
				       report.section(QLatin1Char('\n'), 0, 0), QMessageBox::Close, this);
			box.setTextFormat(Qt::PlainText);
			box.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
			QString reason;
			for (const QString &line : report.split(QLatin1Char('\n')))
				if (line.startsWith(QStringLiteral("Reason: "))) {
					reason = line + QStringLiteral("\n\n");
					break;
				}
			box.setInformativeText(reason + QStringLiteral("View or copy the details when reporting this problem."));
			box.setDetailedText(report);
			QPushButton *copy = box.addButton(QStringLiteral("Copy details"), QMessageBox::ActionRole);
			copy->setAutoDefault(false);
			box.exec();
			if (box.clickedButton() == copy)
				QApplication::clipboard()->setText(report);
		}
		return;
	}

	emit exported(outPath_);

	// The thumbnail comes from the FILE THAT WAS JUST WRITTEN, not from the
	// preview: it is the one picture that proves the export is what you meant,
	// and it is also what gets dragged out. A little way in rather than at zero,
	// because the first frame of a cut is often a fade or a black frame.
	QImage thumb;
	{
		FrameSeeker fs;
		if (fs.open(outPath_)) {
			const qint64 at = fs.durationMs() > 0 ? fs.durationMs() / 10 : 0;
			thumb = fs.frameAt(at, 480, 270);
		}
		if (thumb.isNull() && canvas_)
			thumb = canvas_->currentFrame(); // a GIF, or anything that would not reopen
	}

	// The editor STAYS OPEN. It used to close itself the moment this dialog was
	// dismissed, which undid the point of the dialog: you would finish handling
	// the file and find the project gone, with nothing to go back to for a
	// second export or one more tweak. Nothing downstream needs the close --
	// exported() has already fired, and both listeners only refresh a list.
	ExportDoneDialog dlg(outPath_, thumb, this);
	dlg.exec();
}

} // namespace harpia
