#include "VideoEditorWindow.hpp"

#include <QUuid>

#include "../ui/UiIcons.hpp"
#include "component/BuiltinComponents.hpp"
#include "component/ComponentRegistry.hpp"
#include "component/ScriptComponent.hpp"
#include "../ui/UiText.hpp"
#include "KeyList.hpp"
#include "Parallel.hpp"
#include "ParamSlider.hpp"
#include "StillImage.hpp"
#include "TimeText.hpp"

#include "AudioRecorder.hpp"
#include "ClipExporter.hpp"
#include "DevPanel.hpp"
#include "EditorWidgets.hpp"
#include "ExportOptionsDialog.hpp"
#include "FrameSeeker.hpp"
#include "PreviewDecoder.hpp"
#include "ProxyMedia.hpp"
#include "LevelMeter.hpp"
#include "SceneDetector.hpp"
#include "AudioPreview.hpp"
#include "TimelineAudio.hpp"
#include "TimelineOverview.hpp"
#include "TimelineThumbs.hpp"
#include "timeline/TextStyleJson.hpp"
#include "timeline/TimelineJson.hpp"
#include "TrackEditor.hpp"
#include "VoiceoverMixer.hpp"
#include "VoiceoverTrack.hpp"
#include "MediaFiles.hpp"
#include "../ui/WheelGuard.hpp"
#include "script/TransformScript.hpp"
#include "component/ShaderComponent.hpp"
#include "component/TransformScriptComponent.hpp"
#include "shader/ShaderRenderer.hpp"
#include "timeline/TimelineCompositor.hpp"
#include "ShortcutPanel.hpp"
#include "ShortcutRegistry.hpp"
#include "timeline/KeyframeEditor.hpp"
#include "CanvasFit.hpp"
#include "timeline/TimelineSlice.hpp"
#include "timeline/TimelineView.hpp"
#include "timeline/Transitions.hpp"

#include "../Version.hpp"

#include <QCheckBox>
#include <QComboBox>
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
#include <QFileSystemWatcher>
#include <QFormLayout>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QAction>
#include <QColorDialog>
#include <QFontComboBox>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSpinBox>
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
#include <QIcon>
#include <QListWidget>
#include <QMimeData>
#include <QPixmap>
#include <QTabWidget>

#include "../library/ClipLibrary.hpp"
#include "../library/ThumbnailCache.hpp"
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace harpia {



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


int VideoEditorWindow::openCount_ = 0;

VideoEditorWindow::VideoEditorWindow(const QString &inPath, const QStringList &libraryFolders,
				     QWidget *parent)
	: QDialog(parent), inPath_(inPath), libraryFolders_(libraryFolders)
{
	++openCount_; // the recorder hides its region overlay while one is up
	setWindowTitle(QStringLiteral("Edit — %1").arg(QFileInfo(inPath).fileName()));
	// A real window with minimize/maximize (QDialog hides them by default), so
	// the editor can use the full screen — the preview canvas takes the extra
	// space and the timelines widen for finer control.
	setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
		       Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
	setSizeGripEnabled(true);
	// The size it RESTORES to when un-maximised; the editor itself opens
	// maximised (below), because every part of it wants the room -- the preview,
	// the track stack and the Inspector are all things you immediately drag
	// bigger otherwise.
	resize(1040, 680);
	// Maximised rather than true full-screen: full-screen takes the title bar
	// with it, and this is a window you close, move to another monitor, and put
	// beside the thing you are recording.
	setWindowState(windowState() | Qt::WindowMaximized);
	setAcceptDrops(true); // drop video files to add them as sources

	auto *root = new QVBoxLayout(this);

	// A draggable vertical splitter lets you trade preview height for timeline
	// height: more preview when reviewing, more timeline for detailed cut work.
	auto *splitter = new QSplitter(Qt::Vertical, this);
	splitter->setChildrenCollapsible(false);
	splitter->setHandleWidth(6);

	// ---- Top pane: the preview and a transport bar directly beneath it ----
	auto *topPane = new QWidget(this);
	auto *topLayout = new QVBoxLayout(topPane);
	topLayout->setContentsMargins(0, 0, 0, 0);
	canvas_ = new PreviewCanvas(this);
	// A child of the preview, so it floats over the picture and is clipped to
	// it, and it occupies no layout space -- layOutOverview() places it by hand
	// whenever the preview resizes. It does take the mouse, but only while it is
	// on screen: hidden widgets get no mouse events, so the preview keeps every
	// click except during the couple of seconds this is up.
	overview_ = new TimelineOverview(canvas_);
	canvas_->installEventFilter(this);
	topLayout->addWidget(canvas_, 1);

	// One unified toolbar under the preview, left → right:
	//   [Simple Trim | Multi-Cut]  [undo][redo]  [reset][play] timecode
	//   Speed <slider><value>  [Inspector]  [Dev]
	auto *bar = new QHBoxLayout;

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
		"Keep several sections of one video, joined in order, each at its own speed.\n\n"
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
	// Segmented look: shared fill, joined borders, accent on the active segment.
	const QString segBase = QStringLiteral(
		"QPushButton{background:#2b2f36;color:#c8ccd4;border:1px solid #3a3f47;padding:5px 14px;}"
		"QPushButton:checked{background:#3d7eff;color:#ffffff;border-color:#3d7eff;}");
	trimModeBtn_->setStyleSheet(segBase + QStringLiteral("QPushButton{border-right:none;}"));
	cutModeBtn_->setStyleSheet(segBase + QStringLiteral("QPushButton{border-right:none;}"));
	fullModeBtn_->setStyleSheet(segBase);
	auto *segBox = new QHBoxLayout;
	segBox->setSpacing(0); // no gap — the segments read as one control
	segBox->setContentsMargins(0, 0, 0, 0);
	segBox->addWidget(trimModeBtn_);
	segBox->addWidget(cutModeBtn_);
	segBox->addWidget(fullModeBtn_);
	bar->addLayout(segBox);
	bar->addSpacing(12);

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
	bar->addLayout(urBox);
	bar->addSpacing(12);

	// Transport: reset · play · a big monospace timecode.
	auto *resetBtn = new QPushButton(this);
	resetBtn->setToolTip(QStringLiteral("Move the playhead back to the start"));
	resetBtn->setIcon(uiIcon(Glyph::SkipStart));
	resetBtn->setFixedWidth(40);
	connect(resetBtn, &QPushButton::clicked, this, &VideoEditorWindow::onResetMarker);
	bar->addWidget(resetBtn);
	playBtn_ = new QPushButton(this);
	playBtn_->setIcon(uiIcon(Glyph::Play));
	playBtn_->setToolTip(QStringLiteral("Play from the marker at the current speed"));
	playBtn_->setFixedWidth(40);
	bar->addWidget(playBtn_);
	bar->addSpacing(8);
	// Live cursor readout: the source time of the frame being previewed — large
	// and monospace so it reads as the editor's primary timecode.
	cursorTimeLabel_ = new QLabel(QStringLiteral("0:00.000"), this);
	cursorTimeLabel_->setStyleSheet(QStringLiteral(
		"color:#e8eaed; font-family:monospace; font-weight:bold; font-size:%1px;")
						.arg(uiTimecodePx()));
	cursorTimeLabel_->setToolTip(QStringLiteral("Time of the frame shown in the preview"));
	bar->addWidget(cursorTimeLabel_);
	bar->addSpacing(16);

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
	bar->addWidget(previewQualityCombo_);
	bar->addSpacing(8);

	// Monitor toggle. Preview audio is Full-editing only for now, so it hides
	// with the other Full-only controls.
	audioPreview_ = new AudioPreview(this);
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
	});
	bar->addWidget(muteBtn_);
	bar->addSpacing(10);

	// Speed: label · slider (stretches) · editable value · per-cut count.
	// Full editing has per-clip speed in the Inspector instead, so the whole group
	// hides there rather than sitting greyed out taking up the toolbar.
	speedCaption_ = new QLabel(QStringLiteral("Speed"), this);
	bar->addWidget(speedCaption_);
	speedSlider_ = new QSlider(Qt::Horizontal, this);
	speedSlider_->setRange(0, kSpeedTicks); // exponential 0.1×..50×
	speedSlider_->setPageStep(kSpeedTicks / 20);
	speedSlider_->setValue(speedToSlider(1.0));
	speedSlider_->setMinimumWidth(96); // see EditorChromeParams::speedSliderMinW
	speedSlider_->setToolTip(QStringLiteral("Playback speed (0.1×–50×); scaled so low speeds are easy to fine-tune"));
	bar->addWidget(speedSlider_); // compact, fixed width (see applyChrome)
	speedSpin_ = new QDoubleSpinBox(this);
	speedSpin_->setRange(kMinSpeed, kMaxSpeed);
	speedSpin_->setDecimals(2);
	speedSpin_->setSingleStep(0.1);
	speedSpin_->setSuffix(QStringLiteral("×"));
	speedSpin_->setValue(1.0);
	speedSpin_->setKeyboardTracking(false); // apply on Enter/focus-out, not each digit
	speedSpin_->setFixedWidth(72);
	bar->addWidget(speedSpin_);
	speedLabel_ = new QLabel(QString(), this); // "(N cuts)" / "—" status
	// No reserved width: it is empty in Simple Trim and hidden in Full editing,
	// and 56px of nothing was pushing the toolbar's minimum width up.
	speedLabel_->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	bar->addWidget(speedLabel_);
	bar->addStretch(1); // slack here: transport+speed left, panel toggles right

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
	bar->addWidget(sourcesBtn_);
	bar->addSpacing(6);

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
	bar->addWidget(effectsBtn_);
	bar->addSpacing(6);

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
	bar->addWidget(inspectorBtn_);
	bar->addSpacing(6);
	// Developer Panel: live-tweak every timeline layout variable to find the
	// best UI configuration (editor-only tool, values are not persisted).
	auto *devBtn = new QPushButton(QStringLiteral("Dev"), this);
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
	bar->addWidget(devBtn);
	topLayout->addLayout(bar);
	splitter->addWidget(topPane);

	// ---- Bottom pane: timelines, voiceover and file controls ----
	auto *bottomPane = new QWidget(this);
	bottomPane_ = bottomPane;
	auto *bottomLayout = new QVBoxLayout(bottomPane);
	bottomLayout->setContentsMargins(0, 0, 0, 0);

	timeline_ = new Timeline(this);
	tracks_ = new TrackEditor(this);
	timelineView_ = new TimelineView(this);
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
	connect(timelineView_, &TimelineView::clipsChanged, this, [this]() {
		// A fade drag rounds to the project frame, which only exists once a
		// source is loaded — refresh it whenever the timeline changes.
		timelineView_->setFrameRate(timelineFps());
		updateInfoLabel();
		syncPreviewTransformTarget();
		scriptScanDirty_ = true;
		invalidateAudioMix(); // clips moved/trimmed: the mix no longer matches
		// Moving or trimming a clip on the timeline changes where the playhead
		// falls inside it, so the pose and keyframe readout the Inspector shows
		// have to follow along live.
		syncClipInspector();
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
			requestPreview(-1, timelinePlayheadMs());
		scheduleSnapshot();
	});
	// A finished timeline action closes its undo entry at once. Without this,
	// two edits inside the 350ms coalescing window share one entry -- and a move
	// followed by a move back collapses to no change at all, which is what made
	// Ctrl+Z look broken.
	connect(timelineView_, &TimelineView::editCommitted, this,
		&VideoEditorWindow::commitSnapshot);
	connect(timelineView_, &TimelineView::selectionChanged, this, [this](int, int) {
		syncPreviewTransformTarget();
		updateInspector();
		refreshKeyframeEditor(); // it follows the selection
	});
	// "Show in inspector" from a clip's right-click menu (any mode).
	connect(timelineView_, &TimelineView::filesDropped, this,
		&VideoEditorWindow::onFilesDroppedOnTimeline);
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
			zoom->setEnabled(sel != nullptr);
			zoom->setToolTip(QStringLiteral(
				"Push in to this point at the playhead and back out again. It is written "
				"as ordinary keyframes, so you can drag, retime or delete it afterwards."));
			if (!sel)
				zoom->setText(QStringLiteral("Zoom here  (select a clip first)"));
			if (menu.exec(globalPos) == zoom && sel)
				addZoomAtPreviewPoint(xn, yn);
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

	auto *voRow = new QHBoxLayout;
	voRecordBtn_ = new QPushButton(QStringLiteral("Record voiceover"), this);
	voRecordBtn_->setIcon(uiIcon(Glyph::Record, 12, QColor(0xe5, 0x48, 0x4d)));
	voRecordBtn_->setToolTip(QStringLiteral("Record narration from your microphone onto the Voiceover track"));
	voRow->addWidget(voRecordBtn_);
	voImportBtn_ = new QPushButton(QStringLiteral("Import audio…"), this);
	voImportBtn_->setToolTip(QStringLiteral("Add an existing audio file (mp3/wav/m4a/…) to the Voiceover track"));
	voRow->addWidget(voImportBtn_);
	voMeter_ = new LevelMeter(this);
	voRow->addWidget(voMeter_, 1);
	voStatus_ = new QLabel(QString(), this);
	voStatus_->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	voRow->addWidget(voStatus_);
	voRow->addSpacing(8);
	voDevice_ = new QComboBox(this);
	voDevice_->setToolTip(QStringLiteral("Microphone to record from"));
	voDevice_->setMinimumWidth(160);
	for (const AudioInputDevice &d : AudioRecorder::inputDevices())
		voDevice_->addItem(d.name, d.id);
	voRow->addWidget(voDevice_);
	voTalkAlong_ = new QCheckBox(QStringLiteral("Play while recording"), this);
	voTalkAlong_->setChecked(true);
	voTalkAlong_->setToolTip(QStringLiteral("Play the video (silently) from the start while you narrate"));
	voRow->addWidget(voTalkAlong_);
	voCountdown_ = new QCheckBox(QStringLiteral("Countdown"), this);
	voCountdown_->setToolTip(QStringLiteral("Count 3-2-1 before capture starts"));
	voRow->addWidget(voCountdown_);
	audioLayout->addLayout(voRow);

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

	// Wire the disclosure header: toggle the body, swap the arrow, persist state.
	{
		QSettings s(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		const bool expanded = s.value(QStringLiteral("editor/audioExpanded"), false).toBool();
		auto apply = [audioHeader, audioBody](bool on) {
			audioBody->setVisible(on);
			audioHeader->setText(
				QStringLiteral("Audio — record voiceover over the video"));
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
	connect(voRecorder_, &AudioRecorder::level, this,
		[this](qreal rms, qreal peak) { voMeter_->setLevel(rms, peak); });
	connect(voRecorder_, &AudioRecorder::error, this, [this](const QString &msg) {
		QMessageBox::warning(this, QStringLiteral("Microphone"), msg);
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
	connect(voImportBtn_, &QPushButton::clicked, this, &VideoEditorWindow::onImportAudioClicked);
	connect(voTrack_, &VoiceoverTrack::clipsChanged, this, [this]() {
		updateInfoLabel();
		scheduleSnapshot();
	});

	auto *controls = new QHBoxLayout;
	cropToggle_ = new QCheckBox(QStringLiteral("Crop"), this);
	cropToggle_->setToolTip(QStringLiteral("Drag the rectangle to crop the image (great for smaller GIFs)"));
	controls->addWidget(cropToggle_);
	auto *resetCrop = new QPushButton(QStringLiteral("Reset crop"), this);
	resetCropBtn_ = resetCrop;
	controls->addWidget(resetCrop);
	// Full editing: drop a styled text/caption clip on the timeline.
	addTextBtn_ = new QPushButton(QStringLiteral("Add text"), this);
	addTextBtn_->setToolTip(
		QStringLiteral("Add a text clip at the playhead (font, colour, outline and background "
			       "box are set in the Inspector)"));
	addTextBtn_->setVisible(false); // shown only in Full editing
	connect(addTextBtn_, &QPushButton::clicked, this, &VideoEditorWindow::addTextClip);
	controls->addWidget(addTextBtn_);
	addAudioBtn_ = new QPushButton(QStringLiteral("Add audio"), this);
	addAudioBtn_->setToolTip(QStringLiteral(
		"Put audio on its own timeline track — a source's own audio, an imported file, "
		"or an empty track"));
	addAudioBtn_->setVisible(false); // Full editing only
	connect(addAudioBtn_, &QPushButton::clicked, this, &VideoEditorWindow::onAddAudioClicked);
	controls->addWidget(addAudioBtn_);
	addImageBtn_ = new QPushButton(QStringLiteral("Add image"), this);
	addImageBtn_->setToolTip(
		QStringLiteral("Place a still image (logo, arrow, callout) on the timeline"));
	addImageBtn_->setVisible(false); // Full editing only
	connect(addImageBtn_, &QPushButton::clicked, this, &VideoEditorWindow::addImageClip);
	controls->addWidget(addImageBtn_);
	// Ctrl+V does this too, but a screenshot pasted straight onto the timeline is
	// not something anyone tries unprompted -- so it gets a button, greyed out
	// when there is nothing on the clipboard to place.
	pasteImageBtn_ = new QPushButton(QStringLiteral("Paste image"), this);
	pasteImageBtn_->setToolTip(QStringLiteral(
		"Place the image on the clipboard \u2014 a screenshot, or a picture copied "
		"from a browser \u2014 on the timeline (Ctrl+V)"));
	pasteImageBtn_->setVisible(false); // Full editing only
	connect(pasteImageBtn_, &QPushButton::clicked, this,
		[this]() { pasteImageFromClipboard(timelinePlayheadMs()); });
	connect(QApplication::clipboard(), &QClipboard::dataChanged, this, [this]() {
		if (pasteImageBtn_)
			pasteImageBtn_->setEnabled(systemClipboardHasMedia());
	});
	pasteImageBtn_->setEnabled(systemClipboardHasMedia());
	controls->addWidget(pasteImageBtn_);
	addFxClipBtn_ = new QPushButton(QStringLiteral("Add effect clip"), this);
	addFxClipBtn_->setToolTip(
		QStringLiteral("Drop an effect clip on its own track. It grades every track "
			       "below it, for as long as the clip lasts."));
	addFxClipBtn_->setVisible(false); // Full editing only
	connect(addFxClipBtn_, &QPushButton::clicked, this, &VideoEditorWindow::addEffectClip);
	controls->addWidget(addFxClipBtn_);
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
	controls->addWidget(snapBtn_);
	fitBtn_ = new QPushButton(QStringLiteral("Fit"), this);
	fitBtn_->setToolTip(QStringLiteral("Zoom the timeline out so the whole edit fits"));
	fitBtn_->setVisible(false); // Full editing only
	connect(fitBtn_, &QPushButton::clicked, this, [this]() {
		if (timelineView_)
			timelineView_->zoomToFit();
	});
	controls->addWidget(fitBtn_);
	// View > Keyboard Shortcuts, always available (not just in Full editing):
	// the panel is the reference for every mode.
	auto *keysBtn = new QPushButton(this);
	keysBtn->setIcon(uiIcon(Glyph::Keyboard));
	keysBtn->setToolTip(QStringLiteral("Keyboard shortcuts (Ctrl+/)"));
	keysBtn->setFixedWidth(30);
	connect(keysBtn, &QPushButton::clicked, this, &VideoEditorWindow::openShortcutPanel);
	controls->addWidget(keysBtn);
	controls->addStretch(1);
	infoLabel_ = new QLabel(this);
	infoLabel_->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	controls->addWidget(infoLabel_);
	controls->addSpacing(12);
	auto *openProjBtn = new QPushButton(QStringLiteral("Load project"), this);
	openProjBtn->setToolTip(QStringLiteral("Load a saved editing project (.harpiaproj)"));
	auto *saveProjBtn = new QPushButton(QStringLiteral("Save project"), this);
	saveProjBtn->setToolTip(QStringLiteral("Save the current editing as a project to continue later"));
	controls->addWidget(openProjBtn);
	controls->addWidget(saveProjBtn);
	controls->addSpacing(12);
	auto *saveBtn = new QPushButton(QStringLiteral("Export…"), this);
	saveBtn->setDefault(true);
	auto *cancelBtn = new QPushButton(QStringLiteral("Close"), this);
	controls->addWidget(saveBtn);
	controls->addWidget(cancelBtn);
	bottomLayout->addLayout(controls);

	splitter->addWidget(bottomPane);
	splitter->setStretchFactor(0, 3); // preview grows more than the editing area
	splitter->setStretchFactor(1, 2);
	vsplit_ = splitter;

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

	// Project, Effects, clip transform, keyframes, text and script add up to far
	// more than a window's height. Without a scroll area the layout squeezes the
	// sections past their minimums and they overlap, so the content lives in one.
	auto *insScroll = new QScrollArea(inspector_);
	insScroll->setWidgetResizable(true);
	insScroll->setFrameShape(QFrame::NoFrame);
	// AsNeeded, not AlwaysOff: the panel can be dragged narrower than a row's
	// minimum width, and with no bar those controls would just be cut off.
	insScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	insScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	insScroll->viewport()->setAutoFillBackground(false);
	insScroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
	// The wheel scrolls this panel; it never edits what happens to be under the
	// pointer. See ui/WheelGuard.hpp -- a spin box quietly taking a scroll meant
	// for the panel is a value changed without anyone deciding to change it.
	new WheelGuard(insScroll, this);
	insOuter->addWidget(insScroll, 1);

	auto *insContent = new QWidget(insScroll);
	insContent->setAutoFillBackground(false);
	insScroll->setWidget(insContent);
	auto *insLayout = new QVBoxLayout(insContent);
	insContentLayout_ = insLayout;
	insLayout->setContentsMargins(10, 6, 10, 8);
	insLayout->setSpacing(6);

	// Project-level metadata + actions (collapsible, above the per-clip panels).
	buildProjectInspector(insLayout);
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

	// ---- Clip properties -------------------------------------------------
	auto *clipHdr = new QLabel(QStringLiteral("Clip"), this);
	clipHdr->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed; margin-top:6px;"));
	insLayout->addWidget(clipHdr);
	inspTitle_ = new QLabel(QString(), this);
	inspTitle_->setStyleSheet(QStringLiteral("color:#c8ccd4;"));
	inspTitle_->setWordWrap(true);
	insLayout->addWidget(inspTitle_);
	auto *insForm = new QFormLayout;
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
	insLayout->addLayout(insForm);
	inspHint_ = new QLabel(QString(), this);
	inspHint_->setWordWrap(true);
	inspHint_->setStyleSheet(QStringLiteral("color:#7f858e;"));
	insLayout->addWidget(inspHint_);

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
					return;
				}
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

	insLayout->addStretch(1);

	// ---- Sources: a floating, toggleable panel with two tabs ----------------
	sourcesPanel_ = new QWidget(this, Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
	sourcesPanel_->setWindowTitle(QStringLiteral("Sources"));
	sourcesPanel_->resize(240, 460);
	auto *sideLayout = new QVBoxLayout(sourcesPanel_);
	sideLayout->setContentsMargins(8, 8, 8, 8);
	sideLayout->setSpacing(6);
	auto *srcTabs = new QTabWidget(sourcesPanel_);

	// Tab 1 — "Sources": the videos currently loaded into this edit.
	auto *usedTab = new QWidget(srcTabs);
	auto *usedLayout = new QVBoxLayout(usedTab);
	usedLayout->setContentsMargins(0, 6, 0, 0);
	usedLayout->setSpacing(6);
	sourceList_ = new QListWidget(usedTab);
	sourceList_->setIconSize(QSize(96, 54));
	sourceList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	sourceList_->setToolTip(QStringLiteral(
		"Videos you can cut from. Click one to cut from it; double-click to add its "
		"whole clip to the output."));
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
	libraryList_ = new QListWidget(libTab);
	libraryList_->setIconSize(QSize(96, 54));
	libraryList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	libraryList_->setToolTip(
		QStringLiteral("Recordings from your library — double-click to add one as a source."));
	libLayout->addWidget(libraryList_, 1);
	auto *refreshLibBtn = new QPushButton(QStringLiteral("Refresh"), libTab);
	connect(refreshLibBtn, &QPushButton::clicked, this, &VideoEditorWindow::refreshLibrary);
	libLayout->addWidget(refreshLibBtn);
	connect(libraryList_, &QListWidget::itemDoubleClicked, this,
		&VideoEditorWindow::onLibraryDoubleClicked);
	srcTabs->addTab(libTab, QStringLiteral("Library"));

	sideLayout->addWidget(srcTabs, 1);
	auto *srcHint = new QLabel(
		QStringLiteral("Drag videos onto the editor · double-click a clip to add it."),
		sourcesPanel_);
	srcHint->setWordWrap(true);
	srcHint->setStyleSheet(QStringLiteral("color:#7f858e;"));
	sideLayout->addWidget(srcHint);

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
	connect(QApplication::clipboard(), &QClipboard::dataChanged, this,
		[this]() { systemCopySeq_ = ++seqCounter_; });
	auto onTimeline = [this]() { return fullEdit() && timelineView_; };
	auto frameMs = [this]() {
		const double fps = timelineFps();
		return qint64(std::llround(1000.0 / (fps > 1.0 ? fps : 30.0)));
	};
	auto seekTo = [this](qint64 ms) {
		const qint64 t = std::clamp<qint64>(ms, 0, timelineView_->durationMs());
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
	cmd("playback.playPause", "Play / Pause", "Playback", {QKeySequence(Qt::Key_Space)},
	    [this]() { onPlayPause(); });
	cmd("project.save", "Save project", "Project", {QKeySequence::Save},
	    [this]() { onSaveProject(); });
	cmd("project.export", "Export…", "Export", {QKeySequence(Qt::CTRL | Qt::Key_E)},
	    [this]() { onSave(); });
	cmd("view.shortcuts", "Keyboard shortcuts…", "View",
	    {QKeySequence(Qt::CTRL | Qt::Key_Slash)}, [this]() { openShortcutPanel(); });
	cmd("view.inspector", "Show inspector", "View", {QKeySequence(Qt::CTRL | Qt::Key_I)},
	    [this]() { revealInspector(); });

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
	cmd("edit.paste", "Paste clips or image", "Editing", {QKeySequence::Paste},
	    [this]() { pasteFromClipboard(); });
	tlCmd("edit.delete", "Delete selection", "Editing",
	      {QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)},
	      [this]() { timelineView_->deleteSelected(); });
	tlCmd("marker.toggle", "Add / remove marker", "Markers", {QKeySequence(Qt::Key_M)},
	      [this]() { timelineView_->toggleMarkerAtPlayhead(); });
	// N, not S — S already splits. (Resolve uses N for the magnet too.)
	tlCmd("timeline.snap", "Toggle snapping", "Timeline", {QKeySequence(Qt::Key_N)}, [this]() {
		if (snapBtn_)
			snapBtn_->toggle();
	});
	tlCmd("timeline.fit", "Zoom to fit", "View", {QKeySequence(Qt::Key_F)},
	      [this]() { timelineView_->zoomToFit(); });

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

	connect(timeline_, &Timeline::scrub, this, &VideoEditorWindow::onScrub);
	connect(timeline_, &Timeline::hoverScrub, this, &VideoEditorWindow::onHoverScrub);
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
		voRecordBtn_->setEnabled(false);
		voImportBtn_->setEnabled(false);
		voDevice_->setEnabled(false);
	}

	// Collect every button now (the Developer Panel is created lazily, so its
	// own buttons are excluded) and apply the saved Dev-tunable chrome.
	uiButtons_ = findChildren<QPushButton *>();
	applyChrome(DevPanel::loadChrome());
	applyInspectorParams(DevPanel::loadInspector());

	// Battery saving keys off the APPLICATION losing focus, not this window's.
	// Window focus would also fire for the editor's own Sources panel and its
	// dialogs, pausing playback every time one of them is clicked.
	connect(qApp, &QGuiApplication::applicationStateChanged, this,
		[this](Qt::ApplicationState st) { setPowerSaving(st != Qt::ApplicationActive); });
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
	if (inspector_->isVisible() && hsplit_) {
		const QList<int> sizes = hsplit_->sizes();
		if (sizes.size() == 2 && sizes[1] < p.minWidth) {
			const int total = sizes[0] + sizes[1];
			hsplit_->setSizes({total - p.minWidth, p.minWidth});
		}
	}
}

void VideoEditorWindow::applyChrome(const EditorChromeParams &p)
{
	powerSaveEnabled_ = p.powerSaveOnBlur;
	if (!powerSaveEnabled_ && powerSaving_)
		setPowerSaving(false); // turned off while already saving: come back now

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

EditorSource *VideoEditorWindow::activeSource()
{
	return sourceById(activeSourceId_);
}

FrameSeeker *VideoEditorWindow::seekerFor(int id)
{
	EditorSource *s = sourceById(id);
	return s ? s->seeker.get() : nullptr;
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
	auto seeker = std::make_unique<FrameSeeker>();
	if (!seeker->open(path)) {
		QMessageBox::warning(this, QStringLiteral("Add video"),
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
	return id;
}

void VideoEditorWindow::setActiveSource(int id)
{
	EditorSource *s = sourceById(id);
	if (!s)
		return;
	stopPlayback();
	activeSourceId_ = id;
	seeker_ = s->seeker.get();
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

void VideoEditorWindow::refreshSourceList()
{
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
	const QString dir = activeSource() ? QFileInfo(activeSource()->path).absolutePath() : QString();
	const QString path = QFileDialog::getOpenFileName(
		this, QStringLiteral("Add video"), dir,
		QStringLiteral("Video files (*.mp4 *.mov *.mkv *.webm *.avi *.m4v *.gif *.wmv *.flv *.ts);;"
			       "All files (*)"));
	if (path.isEmpty())
		return;
	const int id = addSource(path);
	if (id >= 0)
		setActiveSource(id);
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
		QMessageBox::warning(
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
			sources_.erase(it);
			break;
		}
	}
	refreshSourceList(); // rebuild the sidebar without the removed row
	if (activeSourceId_ == id) // removed the active source — fall back to another
		setActiveSource(sources_.front().id);
}

void VideoEditorWindow::refreshLibrary()
{
	if (!libraryList_)
		return;
	libraryList_->clear();
	const QSize thumbSz(96, 54);
	const QVector<ClipInfo> clips = ClipLibrary::scan(libraryFolders_);
	for (const ClipInfo &c : clips) {
		auto *item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(c.fileName, c.relativeAge()));
		item->setData(Qt::UserRole, c.filePath);
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
	const QImage img = thumbCache_->cached(path, QSize(96, 54));
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
	// Already loaded as a source? Just make it active. Otherwise add it.
	for (const EditorSource &es : sources_)
		if (QFileInfo(es.path).absoluteFilePath() == QFileInfo(path).absoluteFilePath()) {
			setActiveSource(es.id);
			return;
		}
	const int id = addSource(path);
	if (id >= 0)
		setActiveSource(id);
}

void VideoEditorWindow::onSourceDoubleClicked(QListWidgetItem *item)
{
	if (!item)
		return;
	const int id = item->data(Qt::UserRole).toInt();
	EditorSource *s = sourceById(id);
	if (!s || s->durationMs <= 0)
		return;
	// In Full editing, append the whole clip to the end of the timeline's video
	// track; otherwise drop it onto the Multi-Cut Output track as one cut.
	if (fullEdit()) {
		TlClip c;
		c.sourceId = id;
		c.srcStartMs = 0;
		c.srcEndMs = s->durationMs;
		c.outStartMs = timelineView_->durationMs();
		timelineView_->addClip(TlTrack::Kind::Video, c);
		updateInfoLabel();
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

int VideoEditorWindow::addImageSource(const QString &path)
{
	QString why;
	QImage img = readStillImage(path, &why);
	if (img.isNull()) {
		// The reason, not just the fact. "Could not read that image." left the
		// user with nowhere to go -- most often it was a WebP and a Qt build
		// with no WebP plugin, which is not something anyone could guess.
		QMessageBox::warning(this, QStringLiteral("Add image"), why);
		return -1;
	}
	img = img.convertToFormat(QImage::Format_RGBA8888);
	EditorSource s;
	s.id = nextSourceId_++;
	s.path = path;
	s.name = QFileInfo(path).fileName();
	s.durationMs = 5000; // a still has no length of its own; 5s is the default
	s.width = img.width();
	s.height = img.height();
	stillImages_.insert(s.id, img);
	const int id = s.id;
	sources_.push_back(std::move(s));
	refreshSourceList();
	return id;
}

void VideoEditorWindow::addImageClip()
{
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
	timelineView_->addClip(TlTrack::Kind::Video, c);
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
	for (const QString &f : paths) {
		TlClip c;
		// A GIF is both, and the video reader is what people mean by dropping
		// one -- so the video test goes first, matching MediaFiles.hpp.
		if (isAudioFile(f)) {
			// An audio-only file has no video stream for FrameSeeker to open,
			// so this decodes it to a session WAV and registers THAT. Before
			// this it fell through to the image branch and produced "could not
			// be decoded", which was true of the still reader and useless.
			const int id = addAudioSource(f);
			if (id < 0)
				continue; // addAudioSource already said why
			const EditorSource *src = sourceById(id);
			const QString wav = audioProxyFor(id);
			c.type = TlClip::Type::Video; // "media clip"; the LANE makes it audio
			c.sourceId = id;
			c.srcStartMs = 0;
			c.srcEndMs = src ? src->durationMs : 0;
			c.peaks = VoiceoverTrack::loadPeaks(wav, 600); // so it draws a waveform
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
		if (isVideoFile(f)) {
			const int id = addSource(f);
			if (id < 0)
				continue; // addSource already said why
			const EditorSource *src = sourceById(id);
			c.type = TlClip::Type::Video;
			c.sourceId = id;
			c.srcStartMs = 0;
			c.srcEndMs = src ? src->durationMs : 0;
		} else {
			const int id = addImageSource(f);
			if (id < 0)
				continue;
			c.type = TlClip::Type::Image;
			c.sourceId = id;
			c.srcStartMs = 0;
			c.srcEndMs = 5000; // a still has no length of its own
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
			QGuiApplication::setOverrideCursor(Qt::WaitCursor);
			const bool ok = VoiceoverMixer::decodeToWav(s->path, cand);
			QGuiApplication::restoreOverrideCursor();
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
	QGuiApplication::setOverrideCursor(Qt::WaitCursor);
	const bool ok = VoiceoverMixer::decodeToWav(path, wav);
	QGuiApplication::restoreOverrideCursor();
	if (!ok) {
		--nextSourceId_;
		QMessageBox::warning(this, QStringLiteral("Add audio"),
				     QStringLiteral("Could not read any audio from that file."));
		return -1;
	}
	EditorSource s;
	s.id = id;
	s.path = wav; // the decoded proxy IS the source (export decodes it again)
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
	EditorSource *s = sourceById(sourceId);
	if (!s || !timelineView_)
		return;
	const QString wav = audioProxyFor(sourceId);
	if (wav.isEmpty()) {
		QMessageBox::information(this, QStringLiteral("Add audio"),
					 QStringLiteral("\"%1\" has no audio track.").arg(s->name));
		return;
	}
	qint64 dur = s->durationMs;
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
	c.peaks = VoiceoverTrack::loadPeaks(wav, 600); // waveform
	timelineView_->addClip(TlTrack::Kind::Audio, c);
	if (!fullEdit())
		setEditMode(EditMode::Full);
	updateInfoLabel();
	showTimelineFrame(timelinePlayheadMs());
}

void VideoEditorWindow::onAddAudioClicked()
{
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

	QAction *chosen = menu.exec(addAudioBtn_->mapToGlobal(QPoint(0, addAudioBtn_->height())));
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
	c.outStartMs = timelineView_->durationMs();
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
	const int wantW = int(std::lround(canvas_->width() * dpr));
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
		QImage frameFor(int sourceId, qint64 srcMs) override
		{
			// A still serves the same picture at every timestamp.
			if (const auto it = w->stillImages_.constFind(sourceId);
			    it != w->stillImages_.constEnd())
				return it.value();
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
			if (!exact)
				w->shownExact_ = false;
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
	const QImage composed = TimelineCompositor::compose(timelineView_->model(), outMs, renderSize,
							    fp, scriptEval_.get(), fps, canvasSize);
	setPreviewFrame(composed, outMs);
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
	requestPreview(-1, outMs); // -1 = "composite the timeline"
}

void VideoEditorWindow::onTimelineHoverScrub(qint64 outMs)
{
	if (playing_)
		return;
	requestPreview(-1, outMs);
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
	b->setStyleSheet(QStringLiteral("QPushButton{background:%1;border:1px solid #4a4f57;"
					"min-width:36px;min-height:18px;}")
				 .arg(c.name(QColor::HexRgb)));
}
} // namespace

QWidget *VideoEditorWindow::addSection(QVBoxLayout *into, const QString &title, bool expanded)
{
	auto *head = new QPushButton(this);
	head->setFlat(true);
	head->setCursor(Qt::PointingHandCursor);
	head->setStyleSheet(QStringLiteral(
		"QPushButton{text-align:left;color:#e8eaed;font-weight:bold;border:none;padding:4px 0;}"
		"QPushButton:hover{color:#ffffff;}"));
	auto *body = new QWidget(this);
	auto *bl = new QVBoxLayout(body);
	bl->setContentsMargins(2, 2, 2, 6);
	bl->setSpacing(4);
	body->setVisible(expanded);
	head->setText(title);
	head->setIcon(uiIcon(expanded ? Glyph::ChevronDown : Glyph::ChevronRight, 12));
	connect(head, &QPushButton::clicked, this, [head, body, title]() {
		const bool on = !body->isVisible();
		body->setVisible(on);
		head->setText(title);
		head->setIcon(uiIcon(on ? Glyph::ChevronDown : Glyph::ChevronRight, 12));
	});
	into->addWidget(head);
	into->addWidget(body);
	return body;
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
	pjFormat_ = mkVal();
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

	autosaveChk_ = new QCheckBox(QStringLiteral("Autosave every 5 minutes"), this);
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
	auto *hint = new QLabel(
		QStringLiteral("Dim everything except the chosen areas — the tutorial "
			       "spotlight. Renders identically in the export."),
		spotBox_);
	hint->setWordWrap(true);
	hint->setStyleSheet(QStringLiteral("color:#7f858e;"));
	v->addWidget(hint);
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
		const QColor c = QColorDialog::getColor(cur, this, QStringLiteral("Dim colour"));
		if (!c.isValid())
			return;
		editSpotlight([c](SpotlightSpec &s) { s.dimColor = c; });
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

	const qint64 ph = timelinePlayheadMs();
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
	spotColor_->setStyleSheet(
		QStringLiteral("background:%1; color:%2; border:1px solid #444; padding:3px;")
			.arg(s.dimColor.name(QColor::HexRgb),
			     s.dimColor.lightness() > 140 ? QStringLiteral("#101214")
							  : QStringLiteral("#f0f0f0")));

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
		const qint64 now = timelinePlayheadMs();
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

	// Everything below applies to a picture: an audio clip has no transform,
	// keyframes or scripts, so the whole group hides and the audio group below
	// takes its place.
	videoClipBox_ = new QWidget(clipBox_);
	clipOuter->addWidget(videoClipBox_);
	auto *v = new QVBoxLayout(videoClipBox_);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(5);

	auto *hdr = new QLabel(QStringLiteral("Transform"), clipBox_);
	hdr->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed;"));
	v->addWidget(hdr);

	auto *hint = new QLabel(
		QStringLiteral("Scroll on the preview to zoom, drag to reposition."), clipBox_);
	hint->setWordWrap(true);
	hint->setStyleSheet(QStringLiteral("color:#7f858e;"));
	v->addWidget(hint);

	// Zoom, Position, Rotation, Opacity and Speed all used to live here in a
	// form of their own. They are the pinned Transform and Speed rows of the
	// component list now — one place to look for everything a clip does, rather
	// than some of it here and the rest there. Nothing is left of the form.

	// Reset transform used to be a button here. It is the Transform component's
	// own action now, so it sits inside that component's box with the values it
	// resets rather than floating above them.

	// ---- Keyframes -------------------------------------------------------
	auto *kfHdr = new QLabel(QStringLiteral("Animation"), clipBox_);
	kfHdr->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed; margin-top:6px;"));
	v->addWidget(kfHdr);
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
	kfRow->addWidget(kfPrev);
	kfRow->addWidget(kfAdd, 1);
	kfRow->addWidget(kfDel);
	kfRow->addWidget(kfNext);
	v->addLayout(kfRow);
	connect(kfAdd, &QPushButton::clicked, this, &VideoEditorWindow::addKeyframeAtPlayhead);
	connect(kfDel, &QPushButton::clicked, this, &VideoEditorWindow::removeKeyframeAtPlayhead);
	connect(kfPrev, &QPushButton::clicked, this, [this]() { stepKeyframe(-1); });
	connect(kfNext, &QPushButton::clicked, this, [this]() { stepKeyframe(1); });
	// Every keyframe, listed. "3 keyframes" tells you how many there are and
	// nothing about WHERE they are, so finding the one you want meant stepping
	// through them with the arrows and watching the preview. Each row is its
	// time and the channels it pins; clicking one jumps the playhead to it.
	keyList_ = new QListWidget(clipBox_);
	keyList_->setToolTip(QStringLiteral(
		"Every keyframe on this clip. Click one to jump to it; the ✦ marks show which "
		"channels that key pins."));
	keyList_->setAlternatingRowColors(true);
	keyList_->setUniformItemSizes(true);
	keyList_->setMaximumHeight(120);
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
	keyInfo_->setStyleSheet(QStringLiteral("color:#7f858e;"));
	v->addWidget(keyInfo_);

	// The transform-script panel used to live here. Scripts are components now
	// ("Add Component -> Script"), so the stack, its drag-to-reorder and its
	// parameter controls are the ones every component gets rather than a second
	// set built only for scripts.

	// ---- Text style (text clips only) ------------------------------------
	textBox_ = new QWidget(clipBox_);
	textBox_->setVisible(false);
	auto *tv = new QVBoxLayout(textBox_);
	tv->setContentsMargins(0, 6, 0, 0);
	tv->setSpacing(4);
	auto *tHdr = new QLabel(QStringLiteral("Text"), textBox_);
	tHdr->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed;"));
	tv->addWidget(tHdr);

	// Style presets: save the look you settled on, apply it to any other caption.
	auto *presetRow = new QHBoxLayout;
	textPresetCombo_ = new QComboBox(textBox_);
	textPresetCombo_->setToolTip(
		QStringLiteral("Apply a saved text style. The caption keeps its own words."));
	auto *presetSave = new QPushButton(QStringLiteral("Save…"), textBox_);
	presetSave->setToolTip(QStringLiteral("Save this caption's style under a name"));
	auto *presetDel = new QPushButton(textBox_);
	presetDel->setIcon(uiIcon(Glyph::Cross, 13));
	presetDel->setFixedWidth(28);
	presetDel->setToolTip(QStringLiteral("Delete the selected style"));
	presetRow->addWidget(textPresetCombo_, 1);
	presetRow->addWidget(presetSave);
	presetRow->addWidget(presetDel);
	tv->addLayout(presetRow);
	connect(textPresetCombo_, &QComboBox::activated, this, [this](int i) {
		if (i > 0 && !syncingClip_)
			applyTextPreset(textPresetCombo_->currentText());
	});
	connect(presetSave, &QPushButton::clicked, this,
		&VideoEditorWindow::saveTextPresetFromSelection);
	connect(presetDel, &QPushButton::clicked, this, &VideoEditorWindow::deleteSelectedTextPreset);
	refreshTextPresets();

	textEdit_ = new QPlainTextEdit(textBox_);
	textEdit_->setPlaceholderText(QStringLiteral("Type your caption… (Enter for a new line)"));
	textEdit_->setFixedHeight(56);
	tv->addWidget(textEdit_);
	connect(textEdit_, &QPlainTextEdit::textChanged, this, [this]() {
		if (syncingClip_)
			return;
		const QString t = textEdit_->toPlainText();
		editSelectedClip([&t](TlClip &c) { c.text.text = t; });
	});

	fontCombo_ = new QFontComboBox(textBox_);
	tv->addWidget(fontCombo_);
	connect(fontCombo_, &QFontComboBox::currentFontChanged, this, [this](const QFont &f) {
		if (syncingClip_)
			return;
		editSelectedClip([&f](TlClip &c) { c.text.fontFamily = f.family(); });
	});

	auto *tForm = new QFormLayout;
	tForm->setContentsMargins(0, 0, 0, 0);
	tForm->setHorizontalSpacing(8);
	tForm->setVerticalSpacing(4);

	fontSizeSpin_ = new QSpinBox(textBox_);
	fontSizeSpin_->setRange(6, 400);
	fontSizeSpin_->setKeyboardTracking(false);
	tForm->addRow(QStringLiteral("Size"), fontSizeSpin_);
	connect(fontSizeSpin_, &QSpinBox::valueChanged, this, [this](int v) {
		if (syncingClip_)
			return;
		editSelectedClip([v](TlClip &c) { c.text.fontPx = v; });
	});

	auto *styleRow = new QHBoxLayout;
	boldChk_ = new QCheckBox(QStringLiteral("Bold"), textBox_);
	italicChk_ = new QCheckBox(QStringLiteral("Italic"), textBox_);
	styleRow->addWidget(boldChk_);
	styleRow->addWidget(italicChk_);
	styleRow->addStretch(1);
	tForm->addRow(QStringLiteral("Style"), [&] {
		auto *w = new QWidget(textBox_);
		w->setLayout(styleRow);
		return w;
	}());
	connect(boldChk_, &QCheckBox::toggled, this, [this](bool on) {
		if (syncingClip_)
			return;
		editSelectedClip([on](TlClip &c) { c.text.bold = on; });
	});
	connect(italicChk_, &QCheckBox::toggled, this, [this](bool on) {
		if (syncingClip_)
			return;
		editSelectedClip([on](TlClip &c) { c.text.italic = on; });
	});

	alignCombo_ = new QComboBox(textBox_);
	alignCombo_->addItems({QStringLiteral("Left"), QStringLiteral("Centre"), QStringLiteral("Right")});
	tForm->addRow(QStringLiteral("Align"), alignCombo_);
	connect(alignCombo_, &QComboBox::currentIndexChanged, this, [this](int i) {
		if (syncingClip_)
			return;
		editSelectedClip([i](TlClip &c) { c.text.align = i; });
	});

	textColorBtn_ = new QPushButton(textBox_);
	tForm->addRow(QStringLiteral("Colour"), textColorBtn_);
	connect(textColorBtn_, &QPushButton::clicked, this, [this]() {
		const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
		if (!sel)
			return;
		const QColor c = QColorDialog::getColor(sel->text.color, this,
							QStringLiteral("Text colour"));
		if (c.isValid())
			editSelectedClip([c](TlClip &cl) { cl.text.color = c; });
	});

	outlineWSpin_ = new QDoubleSpinBox(textBox_);
	outlineWSpin_->setRange(0.0, 30.0);
	outlineWSpin_->setSingleStep(0.5);
	outlineWSpin_->setDecimals(1);
	outlineWSpin_->setKeyboardTracking(false);
	tForm->addRow(QStringLiteral("Outline"), outlineWSpin_);
	connect(outlineWSpin_, &QDoubleSpinBox::valueChanged, this, [this](double w) {
		if (syncingClip_)
			return;
		editSelectedClip([w](TlClip &c) { c.text.outlineWidth = w; });
	});

	outlineColorBtn_ = new QPushButton(textBox_);
	tForm->addRow(QStringLiteral("Outline colour"), outlineColorBtn_);
	connect(outlineColorBtn_, &QPushButton::clicked, this, [this]() {
		const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
		if (!sel)
			return;
		const QColor c = QColorDialog::getColor(sel->text.outlineColor, this,
							QStringLiteral("Outline colour"));
		if (c.isValid())
			editSelectedClip([c](TlClip &cl) { cl.text.outlineColor = c; });
	});

	boxChk_ = new QCheckBox(QStringLiteral("Background box"), textBox_);
	tForm->addRow(QString(), boxChk_);
	connect(boxChk_, &QCheckBox::toggled, this, [this](bool on) {
		if (syncingClip_)
			return;
		editSelectedClip([on](TlClip &c) { c.text.boxEnabled = on; });
	});

	boxColorBtn_ = new QPushButton(textBox_);
	tForm->addRow(QStringLiteral("Box colour"), boxColorBtn_);
	connect(boxColorBtn_, &QPushButton::clicked, this, [this]() {
		const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
		if (!sel)
			return;
		// No alpha channel here: the transparency is its own control below, so
		// two places can't disagree about how see-through the box is.
		const QColor c = QColorDialog::getColor(sel->text.boxColor, this,
							QStringLiteral("Box colour"));
		if (c.isValid())
			editSelectedClip([c](TlClip &cl) { cl.text.boxColor = c; });
	});

	boxOpacitySpin_ = new QDoubleSpinBox(textBox_);
	boxOpacitySpin_->setRange(0.0, 1.0);
	boxOpacitySpin_->setSingleStep(0.05);
	boxOpacitySpin_->setDecimals(2);
	boxOpacitySpin_->setKeyboardTracking(false);
	tForm->addRow(QStringLiteral("Box opacity"), boxOpacitySpin_);
	connect(boxOpacitySpin_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
		if (syncingClip_)
			return;
		editSelectedClip([v](TlClip &c) { c.text.boxOpacity = v; });
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
		editSelectedClip([v](TlClip &c) { c.text.boxPadX = v; });
	});
	boxPadYSpin_ = new QSpinBox(textBox_);
	boxPadYSpin_->setRange(0, 200);
	boxPadYSpin_->setKeyboardTracking(false);
	tForm->addRow(QStringLiteral("Box padding Y"), boxPadYSpin_);
	connect(boxPadYSpin_, &QSpinBox::valueChanged, this, [this](int v) {
		if (syncingClip_)
			return;
		editSelectedClip([v](TlClip &c) { c.text.boxPadY = v; });
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
		editSelectedClip([v](TlClip &c) { c.text.boxRadius = v; });
	});

	tv->addLayout(tForm);
	v->addWidget(textBox_);

	// ---- Audio clip: level and fades -------------------------------------
	// These already existed in the model, were saved with the project and were
	// applied at export — there was simply no way to reach them.
	audioClipBox_ = new QWidget(clipBox_);
	audioClipBox_->setVisible(false);
	clipOuter->addWidget(audioClipBox_);
	auto *av = new QVBoxLayout(audioClipBox_);
	av->setContentsMargins(0, 0, 0, 0);
	av->setSpacing(5);
	auto *aHdr = new QLabel(QStringLiteral("Audio"), audioClipBox_);
	aHdr->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed;"));
	av->addWidget(aHdr);

	auto *aForm = new QFormLayout;
	aForm->setContentsMargins(0, 2, 0, 0);
	aForm->setHorizontalSpacing(10);
	aForm->setVerticalSpacing(4);

	clipVolSpin_ = new QDoubleSpinBox(audioClipBox_);
	clipVolSpin_->setRange(0.0, 2.0);
	clipVolSpin_->setDecimals(2);
	clipVolSpin_->setSingleStep(0.05);
	clipVolSpin_->setKeyboardTracking(false);
	clipVolSpin_->setToolTip(QStringLiteral("Level for this clip. 1.00 leaves it as recorded."));
	aForm->addRow(QStringLiteral("Volume"), clipVolSpin_);
	connect(clipVolSpin_, &QDoubleSpinBox::valueChanged, this, [this](double val) {
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

	auto *aHint = new QLabel(
		QStringLiteral("Drag the round grips in an audio clip's top corners to set "
			       "the fades (double-click one to clear it, Shift for a "
			       "precise drag). Fades are applied to the mix, so the "
			       "preview plays what the export will render."),
		audioClipBox_);
	aHint->setWordWrap(true);
	aHint->setStyleSheet(QStringLiteral("color:#7f858e;"));
	av->addWidget(aHint);

	into->addWidget(clipBox_);
}

void VideoEditorWindow::editSelectedClip(const std::function<void(TlClip &)> &fn)
{
	if (!timelineView_)
		return;
	const TlClip *sel = timelineView_->selectedClipPtr();
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
	};
	const Hint hints[] = {
		{playBtn_, "playback.playPause", "Play / pause the preview"},
		{undoBtn_, "edit.undo", "Undo"},
		{redoBtn_, "edit.redo", "Redo"},
		{snapBtn_, "timeline.snap", nullptr}, // has its own on/off tooltip
		{fitBtn_, "timeline.fit", "Zoom the timeline out so the whole edit fits"},
	};
	for (const Hint &h : hints) {
		if (!h.btn || !h.base)
			continue;
		const QString keys = shortcuts_->displayText(QLatin1String(h.id));
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
	const QString label =
		(t >= 0 && t < tracks.size())
			? QStringLiteral("%1 · %2").arg(tracks[t].name).arg(
				  sel->type == TlClip::Type::Text
					  ? sel->text.text.split(QLatin1Char('\n')).value(0)
					  : QStringLiteral("#%1").arg(sel->sourceId))
			: QString();
	keyEditor_->setClip(*sel, label);
	keyEditor_->setPlayheadOut(timelinePlayheadMs());
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
	clipBox_->setVisible(c != nullptr);
	if (!c)
		return;

	// Save/restore rather than clear: a live edit can reach here while an outer
	// sync is already in progress, and clearing the flag would let the widgets
	// being repopulated write back into the clip.
	const bool wasSyncing = syncingClip_;
	syncingClip_ = true;
	const qint64 ph = timelinePlayheadMs();
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
			keyList_->addItem(item);
		}
		// Highlight the one the playhead is on, so the list and the preview
		// agree about where you are.
		keyList_->setCurrentRow(here);
		keyList_->setVisible(!c->keys.isEmpty());
	}

	// Transform script stack + the selected entry's parameter controls.
	refreshScriptList();
	rebuildScriptParams();


	// Which half of the panel applies. A clip on an audio track has no picture,
	// so showing it zoom/rotation controls that do nothing would be a lie.
	const int selTrack = timelineView_->selectedTrack();
	const auto &tracks = timelineView_->model().tracks;
	const bool onAudioTrack = selTrack >= 0 && selTrack < tracks.size() &&
				  tracks[selTrack].kind == TlTrack::Kind::Audio;
	if (videoClipBox_)
		videoClipBox_->setVisible(!onAudioTrack);
	if (audioClipBox_) {
		audioClipBox_->setVisible(onAudioTrack);
		if (onAudioTrack) {
			clipVolSpin_->setValue(c->volume);
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
	if (isText) {
		refreshTextPresets();
		if (textEdit_->toPlainText() != c->text.text)
			textEdit_->setPlainText(c->text.text);
		// Only when it actually differs: setCurrentFont on every refresh makes the
		// combo re-resolve the family, which is needless work now that this runs
		// on each mouse-move of a drag.
		if (!c->text.fontFamily.isEmpty() &&
		    fontCombo_->currentFont().family() != c->text.fontFamily)
			fontCombo_->setCurrentFont(QFont(c->text.fontFamily));
		fontSizeSpin_->setValue(c->text.fontPx);
		boldChk_->setChecked(c->text.bold);
		italicChk_->setChecked(c->text.italic);
		alignCombo_->setCurrentIndex(std::clamp(c->text.align, 0, 2));
		outlineWSpin_->setValue(c->text.outlineWidth);
		boxChk_->setChecked(c->text.boxEnabled);
		boxPadXSpin_->setValue(c->text.boxPadX);
		boxPadYSpin_->setValue(c->text.boxPadY);
		boxRadiusSpin_->setValue(c->text.boxRadius);
		boxOpacitySpin_->setValue(c->text.boxOpacity);
		styleSwatch(textColorBtn_, c->text.color);
		styleSwatch(outlineColorBtn_, c->text.outlineColor);
		styleSwatch(boxColorBtn_, c->text.boxColor);
	}
	syncingClip_ = wasSyncing;
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
	editSelectedClip([idx](TlClip &c) {
		c.keys.remove(idx);
		if (c.keys.size() == 1) { // a single key is just a static pose
			c.setBaseTransform(c.keys.front().tf);
			c.keys.clear();
		}
	});
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
	timelineView_->addClip(TlTrack::Kind::Video, c);
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
	if (const auto it = stillImages_.constFind(c.sourceId); it != stillImages_.constEnd())
		return it.value().size();
	if (EditorSource *s = sourceById(c.sourceId))
		return (!c.crop.isNull() && c.crop.width() > 1) ? c.crop.size()
							       : QSize(s->width, s->height);
	return QSize();
}

void VideoEditorWindow::syncPreviewTransformTarget()
{
	if (!canvas_ || !timelineView_)
		return;
	const TlClip *c = fullEdit() ? timelineView_->selectedClipPtr() : nullptr;
	canvas_->setTransformMode(c != nullptr);
	if (!c) {
		canvas_->setTransformBox(QRectF(), 0.0);
		canvas_->setMaskEdit(PreviewCanvas::MaskEdit{});
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
}

void VideoEditorWindow::applySelectedClipTransform(const TlTransform &tf)
{
	if (!timelineView_)
		return;
	const TlClip *sel = timelineView_->selectedClipPtr();
	if (!sel)
		return;
	TlClip c = *sel;
	// Keyframed clips (or auto-key) record the pose at the playhead so the zoom
	// animates; otherwise the clip's static pose moves.
	if (autoKeyframe_ || !c.keys.isEmpty()) {
		const qint64 ph = timelineEditMs();
		if (c.keys.isEmpty()) {
			// Seed the animation with the current static pose at the clip start so
			// the first recorded key doesn't snap the whole clip.
			TlKeyframe seed;
			seed.tMs = 0;
			seed.tf = c.baseTransform();
			c.keys.append(seed);
		}
		c.setKeyframeAt(ph, tf);
	} else {
		c.setBaseTransform(tf);
	}
	timelineView_->updateSelectedClip(c);
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
	for (const QUrl &u : e->mimeData()->urls()) {
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
		sourcesPlaced_ = true;
	}
	sourcesPanel_->show();
	sourcesPanel_->raise();
}

void VideoEditorWindow::showEvent(QShowEvent *e)
{
	QDialog::showEvent(e);
	// The Sources panel stays closed until the user opens it with the toolbar
	// "Sources" button — it never opens on its own.

	// First real geometry: now the split can be sized to the starting mode.
	QTimer::singleShot(0, this, [this]() { applyModeSplit(); });
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
	if (watched == canvas_ && e->type() == QEvent::Resize)
		layOutOverview();
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
	return QDialog::eventFilter(watched, e);
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
	if (timeline_->start() != 0 || timeline_->end() != seeker_->durationMs())
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
	const QString keep = textPresetCombo_->currentIndex() > 0 ? textPresetCombo_->currentText()
								  : QString();
	const QSignalBlocker b(textPresetCombo_);
	textPresetCombo_->clear();
	textPresetCombo_->addItem(QStringLiteral("Style preset…"));
	textPresetCombo_->addItems(loadTextPresets().keys()); // QMap keys are sorted
	const int i = keep.isEmpty() ? 0 : textPresetCombo_->findText(keep);
	textPresetCombo_->setCurrentIndex(i >= 0 ? i : 0);
}

void VideoEditorWindow::applyTextPreset(const QString &name)
{
	const auto presets = loadTextPresets();
	const auto it = presets.constFind(name);
	if (it == presets.constEnd())
		return;
	// Style only: the clip keeps its own words, which is the whole point of a
	// preset — apply the look you settled on to whatever this caption says.
	const TlText style = it.value();
	editSelectedClip([&style](TlClip &c) {
		const QString words = c.text.text;
		c.text = style;
		c.text.text = words;
	});
	syncClipInspector();
}

void VideoEditorWindow::saveTextPresetFromSelection()
{
	const TlClip *sel = timelineView_ ? timelineView_->selectedClipPtr() : nullptr;
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
		textPresetCombo_->setCurrentIndex(textPresetCombo_->findText(name));
	}
}

void VideoEditorWindow::deleteSelectedTextPreset()
{
	if (!textPresetCombo_ || textPresetCombo_->currentIndex() <= 0)
		return;
	const QString name = textPresetCombo_->currentText();
	if (QMessageBox::question(this, QStringLiteral("Delete style"),
				  QStringLiteral("Delete the text style \"%1\"?").arg(name)) !=
	    QMessageBox::Yes)
		return;
	auto presets = loadTextPresets();
	presets.remove(name);
	saveTextPresets(presets);
	refreshTextPresets();
}

void VideoEditorWindow::copySelectedClips(bool cut)
{
	if (!timelineView_)
		return;
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
QString VideoEditorWindow::writePastedImage(const QImage &img, QString *why)
{
	const auto fail = [why](const QString &msg) {
		if (why)
			*why = msg;
		return QString();
	};
	if (img.isNull())
		return fail(QStringLiteral("The clipboard image was empty."));

	const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
			    QStringLiteral("/pasted");
	if (!QDir().mkpath(dir))
		return fail(QStringLiteral("Could not create the folder for pasted images:\n%1")
				    .arg(QDir::toNativeSeparators(dir)));

	// Timestamped so the folder stays browsable, plus a counter because two
	// pastes inside one second is normal and losing the first would be silent.
	const QString stamp =
		QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"));
	QString path;
	for (int n = 0; n < 10000; ++n) {
		path = n == 0 ? QStringLiteral("%1/pasted-%2.png").arg(dir, stamp)
			      : QStringLiteral("%1/pasted-%2-%3.png").arg(dir, stamp).arg(n);
		if (!QFileInfo::exists(path))
			break;
	}
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

QString VideoEditorWindow::infoTextForTest() const
{
	return infoLabel_ ? infoLabel_->text() : QString();
}

bool VideoEditorWindow::systemClipboardHasMedia() const
{
	const QMimeData *mime = QApplication::clipboard()->mimeData();
	if (!mime)
		return false;
	if (mime->hasImage() && !qvariant_cast<QImage>(mime->imageData()).isNull())
		return true;
	// A file copied in Explorer or Finder arrives as a URL list, not a picture.
	// The timeline already knows which of those it can take.
	return !TimelineView::droppableFiles(mime).isEmpty();
}

bool VideoEditorWindow::pasteImageFromClipboard(qint64 atMs)
{
	const QMimeData *mime = QApplication::clipboard()->mimeData();
	if (!mime) {
		if (infoLabel_)
			infoLabel_->setText(QStringLiteral("There is nothing on the clipboard."));
		return false;
	}

	// Files first: copying a file in the file manager also puts a thumbnail on
	// some clipboards, and the file is unambiguously what was meant.
	const QStringList files = TimelineView::droppableFiles(mime);
	if (!files.isEmpty()) {
		onFilesDroppedOnTimeline(files, -1, -1, atMs);
		return true;
	}

	const QImage img = qvariant_cast<QImage>(mime->imageData());
	if (img.isNull()) {
		// Say which of the two ways it can be unusable it was: an empty
		// clipboard and a clipboard full of text are different mistakes.
		if (infoLabel_)
			infoLabel_->setText(
				mime->hasText()
					? QStringLiteral("The clipboard holds text, not an image.")
					: QStringLiteral("The clipboard holds no image."));
		return false;
	}

	QString why;
	const QString path = writePastedImage(img, &why);
	if (path.isEmpty()) {
		QMessageBox::warning(this, QStringLiteral("Paste image"), why);
		return false;
	}
	const int id = addImageSource(path);
	if (id < 0)
		return false; // addImageSource already said why

	if (!fullEdit())
		setEditMode(EditMode::Full);
	TlClip c;
	c.type = TlClip::Type::Image;
	c.sourceId = id;
	c.srcStartMs = 0;
	c.srcEndMs = 5000; // a still has no length of its own, like Add image
	c.outStartMs = std::max<qint64>(0, atMs);
	timelineView_->addClip(TlTrack::Kind::Video, c);
	updateInfoLabel();
	showTimelineFrame(timelinePlayheadMs());
	return true;
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
	const bool haveClips = timelineView_ && fullEdit() && !clipboard_.isEmpty();
	const bool haveMedia = systemClipboardHasMedia();
	if (!haveClips && !haveMedia) {
		// Not silence: Ctrl+V doing nothing at all reads as a broken shortcut.
		pasteImageFromClipboard(timelinePlayheadMs()); // says which kind of nothing
		return;
	}
	if (haveMedia && (!haveClips || systemCopySeq_ > copySeq_)) {
		pasteImageFromClipboard(timelinePlayheadMs());
		return;
	}
	pasteClips();
}

void VideoEditorWindow::applyModeSplit()
{
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
	// The stack reports the TALLEST page, so ask this page directly and add the
	// rows around it (mode bar, audio foldout, buttons) measured, not guessed.
	const int chrome = bottomPane_->sizeHint().height() - stack_->sizeHint().height();
	const int wanted = std::clamp(chrome + stack_->currentWidget()->sizeHint().height(), 120,
				      std::max(120, total / 2));
	if (sizes[1] > wanted)
		vsplit_->setSizes({total - wanted, wanted});
}

void VideoEditorWindow::showInspector(bool on)
{
	if (!inspector_)
		return;
	inspector_->setVisible(on);
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
		const QList<int> sizes = hsplit_->sizes();
		if (sizes.size() != 2)
			return;
		const int total = sizes[0] + sizes[1];
		const int want = std::clamp(inspectorParams_.openWidth, inspector_->minimumWidth(),
					    std::max(inspector_->minimumWidth(), total / 3));
		if (sizes[1] < want)
			hsplit_->setSizes({total - want, want});
	});
}

void VideoEditorWindow::setEditMode(EditMode m)
{
	stopPlayback();
	// The strip describes the timeline you were on; leaving it makes it a lie.
	if (overview_)
		overview_->hideNow();
	const bool wasFull = fullEdit();
	trimModeBtn_->setChecked(m == EditMode::Trim);
	cutModeBtn_->setChecked(m == EditMode::MultiCut);
	fullModeBtn_->setChecked(m == EditMode::Full);
	stack_->setCurrentIndex(int(m));

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
			QSignalBlocker b(cropToggle_);
			cropToggle_->setChecked(false);
			canvas_->setCropEnabled(false);
		}
	}
	if (addTextBtn_)
		addTextBtn_->setVisible(full);
	if (addAudioBtn_)
		addAudioBtn_->setVisible(full);
	if (addImageBtn_)
		addImageBtn_->setVisible(full);
	if (pasteImageBtn_)
		pasteImageBtn_->setVisible(full);
	if (addFxClipBtn_)
		addFxClipBtn_->setVisible(full);
	if (snapBtn_)
		snapBtn_->setVisible(full);
	if (fitBtn_)
		fitBtn_->setVisible(full);
	if (muteBtn_)
		muteBtn_->setVisible(full); // only Full editing has a timeline to mix
	if (!full && audioPreview_)
		audioPreview_->stop();
	syncPreviewTransformTarget();
	if (full)
		showTimelineFrame(timelinePlayheadMs());

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
	syncComponentPanel();
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
		voStatus_->clear();
		voRecordBtn_->setText(QStringLiteral("Record voiceover"));
	voRecordBtn_->setIcon(uiIcon(Glyph::Record, 12, QColor(0xe5, 0x48, 0x4d)));
		return;
	}
	if (voRecording_) {
		finishVoiceover();
		return;
	}
	if (!valid_)
		return;
	if (voCountdown_->isChecked()) {
		voCountdownLeft_ = 3;
		voStatus_->setText(QStringLiteral("Starting in %1…").arg(voCountdownLeft_));
		voRecordBtn_->setText(QStringLiteral("Cancel"));
		voCountdownTimer_->start();
	} else {
		startVoiceoverCapture();
	}
}

void VideoEditorWindow::startVoiceoverCapture()
{
	stopPlayback();
	voRecorder_->setDeviceId(voDevice_->currentData().toString());
	if (!voRecorder_->start(voiceoverTempDir())) {
		voStatus_->clear();
		voRecordBtn_->setText(QStringLiteral("Record voiceover"));
	voRecordBtn_->setIcon(uiIcon(Glyph::Record, 12, QColor(0xe5, 0x48, 0x4d)));
		return;
	}
	voRecording_ = true;
	// Anchor the take at the output-time under the playhead (0 when idle).
	voClipStartMs_ = currentOutputMs();
	voRecordBtn_->setText(QStringLiteral("Stop"));
	voRecordBtn_->setIcon(uiIcon(Glyph::Stop, 12));
	voStatus_->setStyleSheet(QStringLiteral("color:#e5484d;"));
	voStatus_->setText(QStringLiteral("Recording"));
	voTrack_->setPlayhead(voClipStartMs_);
	// Talk-along: play the video (silently — the editor preview has no audio)
	// so you can narrate to what you see.
	if (voTalkAlong_->isChecked())
		startPlayback();
}

void VideoEditorWindow::finishVoiceover()
{
	if (!voRecording_)
		return;
	voRecording_ = false;
	const QString path = voRecorder_->stop();
	const qint64 durMs = voRecorder_->capturedMs();
	stopPlayback();
	voMeter_->reset();
	voStatus_->clear();
	voStatus_->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	voRecordBtn_->setText(QStringLiteral("Record voiceover"));
	voRecordBtn_->setIcon(uiIcon(Glyph::Record, 12, QColor(0xe5, 0x48, 0x4d)));
	if (path.isEmpty())
		return; // nothing captured
	// In Full editing the voiceover track is hidden — narration belongs on its
	// own timeline audio lane instead.
	if (fullEdit() && timelineView_) {
		const int id = addAudioSource(path);
		if (id >= 0) {
			TlClip c;
			c.sourceId = id;
			c.srcStartMs = 0;
			c.srcEndMs = durMs;
			c.outStartMs = voClipStartMs_;
			c.peaks = VoiceoverTrack::loadPeaks(path, 600);
			timelineView_->addClip(TlTrack::Kind::Audio, c);
			updateInfoLabel();
		}
		return;
	}
	VoiceoverClip clip;
	clip.path = path;
	clip.outStartMs = voClipStartMs_;
	clip.durationMs = durMs;
	clip.srcTotalMs = durMs;
	voTrack_->addClip(clip);
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
		QMessageBox::warning(this, QStringLiteral("Import audio"),
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
	auto *info = new QLabel(
		QStringLiteral("Detect visual scene changes and add a cut at each one.\n"
			       "Lower threshold = more cuts, higher = fewer."),
		&dlg);
	info->setWordWrap(true);
	v->addWidget(info);
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
		QMessageBox::warning(this, QStringLiteral("Auto-cut"), err);
		return;
	}

	// Build cut spans from the boundaries: 0, each scene change, end.
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
		QMessageBox::warning(this, QStringLiteral("Save project"), err);
	refreshProjectInspector();
}

void VideoEditorWindow::onSaveProjectAs()
{
	if (!valid_)
		return;
	const QString suggested =
		projectPath_.isEmpty()
			? QFileInfo(inPath_).absolutePath() + QLatin1Char('/') +
				  QFileInfo(inPath_).completeBaseName() + QStringLiteral("_edit.harpiaproj")
			: projectPath_;
	QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save project as"), suggested,
						    QStringLiteral("Harpia project (*.harpiaproj)"));
	if (path.isEmpty())
		return;
	if (!path.endsWith(QStringLiteral(".harpiaproj"), Qt::CaseInsensitive))
		path += QStringLiteral(".harpiaproj");
	const QString err = saveProjectTo(path, /*quiet=*/false);
	if (!err.isEmpty())
		QMessageBox::warning(this, QStringLiteral("Save project"), err);
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
		so[QStringLiteral("path")] = QDir::toNativeSeparators(es.path);
		so[QStringLiteral("name")] = es.name;
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

	// "Full editing" multi-track timeline (project v3). Clip source ids are
	// remapped through the same `sources` array as the Multi-Cut segments.
	if (!s.timeline.isEmpty()) {
		QJsonArray trackArr;
		for (const TlTrack &t : s.timeline.tracks)
			trackArr.append(trackToJson(t));
		root[QStringLiteral("tracks")] = trackArr;
		if (!s.timeline.markers.isEmpty()) {
			QJsonArray mk;
			for (const qint64 m : s.timeline.markers)
				mk.append(double(m));
			root[QStringLiteral("markers")] = mk;
		}
		root[QStringLiteral("harpiaProject")] = 3; // timelines need a v3 reader
	}

	// No project-level "effects" array any more: a shader is a component, so it
	// is written with the clip that carries it. The reader still understands the
	// old key and migrates it -- see applyProjectJson.

	QFile f(path);
	if (!f.open(QIODevice::WriteOnly))
		return QStringLiteral("Could not write the project file.");
	f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	f.close();

	if (quiet) { // autosave sidecar — don't adopt it as the project file
		lastAutosave_ = QDateTime::currentDateTime();
		return QString();
	}
	projectPath_ = path;
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
	if (!valid_)
		return;
	const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open project"),
							  QFileInfo(inPath_).absolutePath(),
							  QStringLiteral("Harpia project (*.harpiaproj)"));
	if (path.isEmpty())
		return;
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(this, QStringLiteral("Open project"),
				     QStringLiteral("Could not read the project file."));
		return;
	}
	QJsonParseError perr;
	const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
	f.close();
	if (perr.error != QJsonParseError::NoError || !doc.isObject() ||
	    !doc.object().contains(QStringLiteral("harpiaProject"))) {
		QMessageBox::warning(this, QStringLiteral("Open project"),
				     QStringLiteral("This is not a valid Harpia project file."));
		return;
	}
	applyProjectJson(doc.object(), path, /*quiet=*/false);
}

// Open a project from a known path. Same validation as the menu item, without
// the dialog and without a message box on failure.
bool VideoEditorWindow::openProjectAt(const QString &path)
{
	if (!valid_)
		return false;
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return false;
	QJsonParseError perr;
	const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
	f.close();
	if (perr.error != QJsonParseError::NoError || !doc.isObject() ||
	    !doc.object().contains(QStringLiteral("harpiaProject")))
		return false;
	applyProjectJson(doc.object(), path, /*quiet=*/true);
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
// all of them keeps the sequencing exactly as it was. The GUI thread is one of
// the workers, so no core sits idle waiting for the others.
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

void VideoEditorWindow::applyProjectJson(const QJsonObject &root, const QString &path, bool quiet)
{
	const int ver = root.value(QStringLiteral("harpiaProject")).toInt(1);

	// v2 projects carry their own list of sources — open (or relink) each and
	// map its saved id onto the editor's live source id. v1 projects (single
	// source) fall back to the primary source and the "different file" hint.
	QHash<int, int> srcMap; // project source id -> editor source id
	int defaultSrcId = sources_.empty() ? 0 : sources_.front().id;
	if (ver >= 2 && root.value(QStringLiteral("sources")).isArray()) {
		bool first = true;
		for (const QJsonValue &jv : root.value(QStringLiteral("sources")).toArray()) {
			const QJsonObject so = jv.toObject();
			const int pid = so.value(QStringLiteral("id")).toInt();
			QString spath = so.value(QStringLiteral("path")).toString();
			const QString sname = so.value(QStringLiteral("name")).toString();
			int eid = -1;
			for (const EditorSource &es : sources_)
				if (QFileInfo(es.path).absoluteFilePath() ==
				    QFileInfo(spath).absoluteFilePath()) {
					eid = es.id;
					break;
				}
			if (eid < 0) {
				// A quiet open never puts a dialog up: a missing file is
				// left missing rather than stopping an automated caller.
				if (!quiet && !QFileInfo::exists(spath)) {
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
						isImageFile(spath)
							? imageOpenFilter()
							: QStringLiteral(
								  "Video files (*.mp4 *.mov *.mkv *.webm "
								  "*.avi *.m4v *.gif *.wmv *.flv "
								  "*.ts);;All files (*)"));
					if (!picked.isEmpty())
						spath = picked;
				}
				eid = addSource(spath);
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

	EditorSnapshot s;
	s.trimStart = qint64(root.value(QStringLiteral("trimStart")).toDouble(0));
	s.trimEnd = qint64(root.value(QStringLiteral("trimEnd")).toDouble(double(seeker_->durationMs())));
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
	for (const QJsonValue &mv : root.value(QStringLiteral("markers")).toArray())
		s.timeline.markers.append(qint64(mv.toDouble()));
	std::sort(s.timeline.markers.begin(), s.timeline.markers.end());
	for (const QJsonValue &tv : root.value(QStringLiteral("tracks")).toArray()) {
		TlTrack t = trackFromJson(tv.toObject());
		// Source ids in the file index the project's own `sources` array; map
		// them onto whatever those files became in this session's media pool.
		for (TlClip &c : t.clips)
			c.sourceId = srcMap.isEmpty() ? defaultSrcId
						      : srcMap.value(c.sourceId, defaultSrcId);

		s.timeline.tracks.append(t);
	}

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
		const QHash<QString, QVector<float>> peaks = decodePeaks(want);
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

	// Show a source that the project actually uses, then apply the edits (which
	// restore the trim range / segments on top).
	setActiveSource(defaultSrcId);
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

	// Open in the mode the project was authored in.
	if (!s.timeline.isEmpty()) {
		timelineSeeded_ = true; // don't seed over the loaded timeline
		if (!fullEdit())
			setEditMode(EditMode::Full);
	} else if (!s.segments.isEmpty() && !multiCut()) {
		setEditMode(EditMode::MultiCut);
	}
	captureSnapshot(); // make the load an undo step
	updateUndoRedoButtons();
	if (!quiet)
		QMessageBox::information(this, QStringLiteral("Open project"),
					 QStringLiteral("Project loaded."));
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
	if (multiCut()) {
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
	s.cropEnabled = canvas_->cropEnabled();
	s.cropRect = canvas_->cropRectVideo();
	s.voiceClips = voTrack_->clips();
	if (timelineView_)
		s.timeline = timelineView_->model();
	return s;
}

void VideoEditorWindow::scheduleSnapshot()
{
	if (restoring_ || !valid_)
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
	if (histIndex_ + 1 < history_.size())
		history_.resize(histIndex_ + 1); // drop the redo tail
	history_.push_back(s);
	constexpr int kMaxHistory = 100;
	if (history_.size() > kMaxHistory)
		history_.removeFirst();
	histIndex_ = history_.size() - 1;
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

	{
		QSignalBlocker b(cropToggle_);
		cropToggle_->setChecked(s.cropEnabled);
	}
	canvas_->setCropEnabled(s.cropEnabled);
	canvas_->setCropRectVideo(s.cropRect);

	voTrack_->setClips(s.voiceClips);

	if (timelineView_) {
		timelineView_->setModel(s.timeline);
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
	if (!timelineView_)
		return QString();
	QString k;
	for (const TlTrack &t : timelineView_->model().tracks) {
		if (t.muted)
			continue;
		k += (t.kind == TlTrack::Kind::Audio) ? QLatin1Char('A') : QLatin1Char('V');
		for (const TlClip &c : t.clips) {
			if (c.type == TlClip::Type::Text || c.type == TlClip::Type::Image)
				continue;
			k += QStringLiteral("|%1,%2,%3,%4,%5,%6,%7,%8")
				     .arg(c.sourceId)
				     .arg(c.srcStartMs)
				     .arg(c.srcEndMs)
				     .arg(c.outStartMs)
				     .arg(c.speed, 0, 'f', 4)
				     .arg(c.volume, 0, 'f', 4)
				     .arg(c.fadeInMs)
				     .arg(c.fadeOutMs);
		}
	}
	return k;
}

void VideoEditorWindow::invalidateAudioMix()
{
	audioMixValid_ = false;
	if (audioPreview_)
		audioPreview_->clear();
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
	// so the picture starts now and the sound joins when the mix lands.
	audioStartMs_ = fromMs;
	startAudioWhenReady_ = true;
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
		playSeg_ = -1; // force the first segment seek
		playClock_.restart();
		playTimer_->start();
		return;
	}
	playing_ = true;
	playBtn_->setIcon(uiIcon(Glyph::Pause));
	// Start from the marker (source-time), clamped into the trimmed region.
	const qint64 start = timeline_->start(), end = timeline_->end();
	qint64 pos = timeline_->playhead();
	if (pos < start || pos >= end)
		pos = start;
	playAnchorMs_ = pos;
	seeker_->seekTo(playAnchorMs_);
	playClock_.restart();
	playTimer_->start();
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
	// A proxy that landed mid-playback is safe to install now.
	flushPendingPlaybackProxies();
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
			if (apos >= 0)
				outPos = apos;
		}
		if (outPos >= total) { // loop
			playAnchorMs_ = 0;
			playClock_.restart();
			outPos = 0;
			if (audioPreview_)
				audioPreview_->start(0); // restart the sound with the picture
		}
		timelineView_->setPlayhead(outPos);
		cursorTimeLabel_->setText(previewTimeText(outPos));
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
		// Each cut may come from a different source — decode from that source's
		// seeker, seeking on any segment OR source change.
		FrameSeeker *segSeeker = seekerFor(segs[seg].sourceId);
		if (!segSeeker) {
			stopPlayback();
			return;
		}
		if (seg != playSeg_ || segs[seg].sourceId != playSourceId_) {
			segSeeker->seekTo(segs[seg].srcStartMs);
			playSeg_ = seg;
			playSourceId_ = segs[seg].sourceId;
		}
		// nextFrameAt cannot hold position: it always advances at least one
		// frame. So when the clock has not yet moved past the frame already on
		// screen, asking again runs the PICTURE ahead of the TIMELINE -- and
		// since it is the picture that decides what you see, it sails straight
		// past the cut's end handle and keeps playing the rest of the source.
		// It bites whenever a frame lasts longer than a tick: a 15 fps source
		// against the 30 fps preview timer advanced twice as fast as the output
		// clock and overran a one-second cut by a full second, as did any cut
		// slowed below 1x. Hold the frame instead; the clock catches up.
		if (segSeeker->positionMs() >= 0 && segSeeker->positionMs() >= srcTarget) {
			tracks_->setPlayhead(outPos);
			voTrack_->setPlayhead(outPos);
			cursorTimeLabel_->setText(previewTimeText(srcTarget));
			return;
		}
		// Decode forward to the target source time; only the shown frame is
		// converted (skipped catch-up frames stay in YUV — see nextFrameAt).
		const QImage img = segSeeker->nextFrameAt(srcTarget, nullptr, 1280, 720, 240);
		if (img.isNull()) {
			// Source ended inside this cut — skip to the next segment.
			playAnchorMs_ = tracks_->outputStartOf(seg) + segs[seg].outDurationMs();
			playClock_.restart();
			playSeg_ = -1;
			return;
		}
		setPreviewFrame(img, outPos);
		tracks_->setPlayhead(outPos);
		voTrack_->setPlayhead(outPos);
		cursorTimeLabel_->setText(previewTimeText(srcTarget));
		return;
	}

	const qint64 start = timeline_->start();
	const qint64 end = timeline_->end();

	qint64 target = playAnchorMs_ + qint64(playClock_.elapsed() * speed_);
	if (target >= end) {
		// Loop back to the start of the trimmed region.
		playAnchorMs_ = start;
		playClock_.restart();
		seeker_->seekTo(start);
		target = start;
	}

	// The same rule as Multi-Cut above: do not ask for a frame the clock has
	// not reached, or the picture outruns the trimmed region and plays past the
	// end handle. Most visible at speeds below 1x and on low-frame-rate sources.
	if (seeker_->positionMs() >= 0 && seeker_->positionMs() >= target) {
		timeline_->setPlayhead(seeker_->positionMs());
		return;
	}
	// Decode forward to the target time; only the shown frame is converted
	// (skipped catch-up frames stay in YUV — see nextFrameAt).
	qint64 ts = -1;
	const QImage img = seeker_->nextFrameAt(target, &ts, 1280, 720, 240);
	if (img.isNull()) { // reached end of file inside the region — loop
		seeker_->seekTo(start);
		playAnchorMs_ = start;
		playClock_.restart();
		return;
	}
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
	      QStringLiteral("pixelate")}) {
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
	startFilmstrip(sourceId, proxyPath);
	if (EditorSource *s = sourceById(sourceId))
		qInfo("harpia: preview proxy ready for %s", qUtf8Printable(s->name));
	updateProxyStatus();
	// The frame on screen came out of the original file; ask for it again so
	// what is shown is what the preview will keep serving from now on.
	shownExact_ = false;
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
	if (playing_) {
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
	if (!valid_ || playing_ || shownExact_ || shownMs_ < 0)
		return;
	requestPreview(shownSource_, shownMs_);
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
	if (!valid_ || exporter_) // ignore while an export is already running
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
		// Offer the range only when it is actually a smaller thing than the
		// project. A "Selection" entry that covers everything is a choice with
		// one outcome, which is worse than no choice at all.
		span.fromMs = std::max<qint64>(0, span.fromMs);
		span.toMs = std::min(span.toMs, timelineView_->durationMs());
		if (span.isValid() && span.durationMs() < timelineView_->durationMs()) {
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
		ec.fps = seeker_ ? seeker_->fps() : 30.0;
		ec.seconds = tracks_->totalOutputMs() / 1000.0;
		ec.previewFrame = canvas_->currentFrame();
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
	o.outWidth = outSize.width();
	o.outHeight = outSize.height();
	o.speed = speed_;   // from the editor's speed slider
	o.videoCrf = dlg.videoCrf();
	o.effort = dlg.effort();
	o.chroma444 = dlg.chroma444();
	o.keepAudio = dlg.keepAudio();
	if (cuts) {
		// Multi-cut assembly: the cut list replaces trim range + global speed.
		// inputs[0] is always the primary (canvas) source; other used sources
		// follow. When only the primary is used, inputs stays empty so the
		// single-input export path runs unchanged.
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

	// Nothing to bake here any more: a shader is a component, so it runs inside
	// the compositor the exporter already shares with the preview.

	exporter_ = new ClipExporter(this);
	connect(exporter_, &ClipExporter::progress, this, &VideoEditorWindow::onExportProgress);
	connect(exporter_, &ClipExporter::finished, this, &VideoEditorWindow::onExportFinished);

	progress_ = new QProgressDialog(QStringLiteral("Exporting %1…").arg(ClipExporter::extensionFor(fmt).toUpper()),
					QStringLiteral("Cancel"), 0, 100, this);
	progress_->setWindowModality(Qt::WindowModal);
	progress_->setAutoClose(false);
	progress_->setAutoReset(false);
	progress_->setMinimumDuration(0);
	connect(progress_, &QProgressDialog::canceled, this, [this]() {
		if (exporter_)
			exporter_->cancel();
	});
	progress_->setValue(0);
	progress_->show(); // ensure the modal progress is visible immediately

	exportThread_ = std::thread([this, o]() { exporter_->run(inPath_, outPath_, o); });
}

void VideoEditorWindow::onExportProgress(int pct, qint64 etaMs, qint64 bytes)
{
	if (!progress_)
		return;
	progress_->setValue(pct);
	QString label = QStringLiteral("Exporting… %1%").arg(pct);
	if (bytes > 0)
		label += QStringLiteral("   %1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
	if (etaMs > 0)
		label += QStringLiteral("   ~%1s left").arg((etaMs + 500) / 1000);
	progress_->setLabelText(label);
}

void VideoEditorWindow::onExportFinished(bool ok, bool canceled, const QString &err)
{
	joinExport();
	if (progress_) {
		progress_->reset();
		progress_->deleteLater();
		progress_ = nullptr;
	}
	if (exporter_) {
		exporter_->deleteLater();
		exporter_ = nullptr;
	}

	if (canceled || !ok) {
		QFile::remove(outPath_); // never leave a partial file
		if (!canceled)
			QMessageBox::warning(this, QStringLiteral("Export failed"),
					     err.isEmpty() ? QStringLiteral("Could not export the clip.") : err);
		return;
	}

	emit exported(outPath_);

	QMessageBox box(this);
	box.setWindowTitle(QStringLiteral("Clip exported"));
	box.setIcon(QMessageBox::Information);
	box.setText(QStringLiteral("Saved %1").arg(QDir::toNativeSeparators(outPath_)));
	QPushButton *openFileBtn = box.addButton(QStringLiteral("Open File"), QMessageBox::ActionRole);
	QPushButton *openFolderBtn = box.addButton(QStringLiteral("Open Folder"), QMessageBox::ActionRole);
	box.addButton(QStringLiteral("Done"), QMessageBox::AcceptRole);
	box.exec();
	if (box.clickedButton() == openFileBtn)
		QDesktopServices::openUrl(QUrl::fromLocalFile(outPath_));
	else if (box.clickedButton() == openFolderBtn)
		revealInFolder(outPath_);

	accept(); // close the editor
}

} // namespace harpia
