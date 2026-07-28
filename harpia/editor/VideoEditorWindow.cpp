#include "VideoEditorWindow.hpp"

#include "AudioRecorder.hpp"
#include "ClipExporter.hpp"
#include "DevPanel.hpp"
#include "EditorWidgets.hpp"
#include "ExportOptionsDialog.hpp"
#include "FrameSeeker.hpp"
#include "LevelMeter.hpp"
#include "SceneDetector.hpp"
#include "AudioPreview.hpp"
#include "TimelineAudio.hpp"
#include "TimelineThumbs.hpp"
#include "timeline/TextStyleJson.hpp"
#include "timeline/TimelineJson.hpp"
#include "TrackEditor.hpp"
#include "VoiceoverMixer.hpp"
#include "VoiceoverTrack.hpp"
#include "script/TransformScript.hpp"
#include "shader/ShaderRenderer.hpp"
#include "timeline/TimelineCompositor.hpp"
#include "timeline/KeyframeEditor.hpp"
#include "timeline/TimelineView.hpp"

#include "../Version.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QButtonGroup>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <QApplication>
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
#include <QScrollArea>
#include <QCryptographicHash>
#include <QSet>
#include <QSettings>
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
// Accepted source video extensions (drag-drop + Add video filter).
bool isVideoFile(const QString &path)
{
	static const QStringList kExts = {QStringLiteral("mp4"), QStringLiteral("mov"),
					  QStringLiteral("mkv"), QStringLiteral("webm"),
					  QStringLiteral("avi"), QStringLiteral("m4v"),
					  QStringLiteral("gif"), QStringLiteral("wmv"),
					  QStringLiteral("flv"), QStringLiteral("ts")};
	return kExts.contains(QFileInfo(path).suffix().toLower());
}

QString previewTimeText(qint64 ms)
{
	return QStringLiteral("%1:%2.%3")
		.arg(ms / 60000)
		.arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
		.arg(ms % 1000, 3, 10, QLatin1Char('0'));
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

VideoEditorWindow::VideoEditorWindow(const QString &inPath, const QStringList &libraryFolders,
				     QWidget *parent)
	: QDialog(parent), inPath_(inPath), libraryFolders_(libraryFolders)
{
	setWindowTitle(QStringLiteral("Edit — %1").arg(QFileInfo(inPath).fileName()));
	// A real window with minimize/maximize (QDialog hides them by default), so
	// the editor can use the full screen — the preview canvas takes the extra
	// space and the timelines widen for finer control.
	setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
		       Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
	setSizeGripEnabled(true);
	resize(1040, 680);
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
	trimModeBtn_->setToolTip(QStringLiteral("Trim one start/end range"));
	cutModeBtn_ = new QPushButton(QStringLiteral("Multi-Cut"), this);
	cutModeBtn_->setCheckable(true);
	cutModeBtn_->setToolTip(QStringLiteral(
		"Drag on the Source track to select the sections to keep; they are joined in order. "
		"Each cut gets its own playback speed."));
	fullModeBtn_ = new QPushButton(QStringLiteral("Full Editing"), this);
	fullModeBtn_->setCheckable(true);
	fullModeBtn_->setToolTip(QStringLiteral(
		"Multi-track timeline: place clips on stacked video/audio tracks, with overlays, "
		"picture-in-picture and gaps (like a full video editor)."));
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
	undoBtn_ = new QPushButton(QStringLiteral("↶"), this);
	undoBtn_->setToolTip(QStringLiteral("Undo (Ctrl+Z)"));
	undoBtn_->setFixedWidth(34);
	undoBtn_->setEnabled(false);
	connect(undoBtn_, &QPushButton::clicked, this, &VideoEditorWindow::undo);
	urBox->addWidget(undoBtn_);
	redoBtn_ = new QPushButton(QStringLiteral("↷"), this);
	redoBtn_->setToolTip(QStringLiteral("Redo (Ctrl+Shift+Z)"));
	redoBtn_->setFixedWidth(34);
	redoBtn_->setEnabled(false);
	connect(redoBtn_, &QPushButton::clicked, this, &VideoEditorWindow::redo);
	urBox->addWidget(redoBtn_);
	bar->addLayout(urBox);
	bar->addSpacing(12);

	// Transport: reset · play · a big monospace timecode.
	auto *resetBtn = new QPushButton(QStringLiteral("⏮"), this);
	resetBtn->setToolTip(QStringLiteral("Move the playhead back to the start"));
	resetBtn->setFixedWidth(40);
	connect(resetBtn, &QPushButton::clicked, this, &VideoEditorWindow::onResetMarker);
	bar->addWidget(resetBtn);
	playBtn_ = new QPushButton(QStringLiteral("▶"), this);
	playBtn_->setToolTip(QStringLiteral("Play from the marker at the current speed"));
	playBtn_->setFixedWidth(40);
	bar->addWidget(playBtn_);
	bar->addSpacing(8);
	// Live cursor readout: the source time of the frame being previewed — large
	// and monospace so it reads as the editor's primary timecode.
	cursorTimeLabel_ = new QLabel(QStringLiteral("0:00.000"), this);
	cursorTimeLabel_->setStyleSheet(QStringLiteral(
		"color:#e8eaed; font-family:monospace; font-size:18px; font-weight:bold;"));
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
	muteBtn_ = new QPushButton(QStringLiteral("🔊"), this);
	muteBtn_->setCheckable(true);
	muteBtn_->setFixedWidth(34);
	const bool canHear = AudioPreview::available();
	muteBtn_->setEnabled(canHear);
	muteBtn_->setToolTip(canHear ? QStringLiteral("Mute the preview (the export is unaffected)")
				     : QStringLiteral("No audio output device was found"));
	connect(muteBtn_, &QPushButton::toggled, this, [this](bool off) {
		muteBtn_->setText(off ? QStringLiteral("🔇") : QStringLiteral("🔊"));
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

	// Effects toggle — opens the right panel (post-processing effects sit at its
	// top). A dedicated button so the effect stack is easy to find.
	effectsBtn_ = new QPushButton(QStringLiteral("Effects"), this);
	effectsBtn_->setCheckable(true);
	effectsBtn_->setChecked(false);
	effectsBtn_->setToolTip(QStringLiteral(
		"Show/hide post-processing effects (color grade, CRT, …) — applied to the preview and baked into export"));
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
	connect(timelineView_, &TimelineView::inspectClipRequested, this,
		&VideoEditorWindow::revealInspector);
	connect(timelineView_, &TimelineView::keyframeEditorRequested, this,
		&VideoEditorWindow::openKeyframeEditor);
	// Direct manipulation of the selected clip straight in the preview.
	connect(canvas_, &PreviewCanvas::transformDragged, this,
		&VideoEditorWindow::onPreviewTransformDrag);
	connect(canvas_, &PreviewCanvas::transformZoomed, this,
		&VideoEditorWindow::onPreviewTransformZoom);

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
	voRecordBtn_ = new QPushButton(QStringLiteral("●  Record voiceover"), this);
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
			audioHeader->setText(on
				? QStringLiteral("▾  Audio — record voiceover over the video")
				: QStringLiteral("▸  Audio — record voiceover over the video"));
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
	addAudioBtn_ = new QPushButton(QStringLiteral("Add audio  ▾"), this);
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
	// Magnet: snap dragged clips to the playhead, 0 and other clips' edges.
	// A sticky toggle — whichever way you leave it is how the next session opens.
	snapBtn_ = new QPushButton(QStringLiteral("🧲 Snap"), this);
	snapBtn_->setCheckable(true);
	snapBtn_->setVisible(false); // Full editing only
	auto applySnap = [this](bool on) {
		if (timelineView_)
			timelineView_->setSnapEnabled(on);
		snapBtn_->setText(on ? QStringLiteral("🧲 Snap on")
				     : QStringLiteral("🧲 Snap off"));
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
	// ---- Effect stack: user GLSL post-processing (preview + baked export) ----
	// Placed first so it's the panel's headline feature (open via the Effects
	// toolbar button). Effects stack top-to-bottom; each is a .frag in the user
	// shaders folder with its own //@param controls.
	{
		auto *fxHdr = new QLabel(QStringLiteral("Effects"), this);
		fxHdr->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed;"));
		insLayout->addWidget(fxHdr);

		auto *fxBtns = new QHBoxLayout;
		addEffectBtn_ = new QPushButton(QStringLiteral("Add effect  ▾"), this);
		addEffectBtn_->setToolTip(QStringLiteral(
			"Add a post-processing shader. Effects stack top-to-bottom and bake into export."));
		auto *folderBtn = new QPushButton(QStringLiteral("Folder"), this);
		folderBtn->setToolTip(QStringLiteral("Open the shaders folder — drop .frag files here"));
		fxBtns->addWidget(addEffectBtn_, 1);
		fxBtns->addWidget(folderBtn);
		insLayout->addLayout(fxBtns);

		shaderError_ = new QLabel(QString(), this);
		shaderError_->setWordWrap(true);
		shaderError_->setStyleSheet(
			QStringLiteral("color:#e5484d; font-family:monospace; font-size:11px;"));
		shaderError_->setVisible(false);
		insLayout->addWidget(shaderError_);

		effectsBox_ = new QWidget(this);
		auto *ebl = new QVBoxLayout(effectsBox_);
		ebl->setContentsMargins(0, 2, 0, 2);
		ebl->setSpacing(6);
		insLayout->addWidget(effectsBox_);

		connect(addEffectBtn_, &QPushButton::clicked, this, [this]() {
			QMenu menu(this);
			const QStringList names = availableShaders();
			if (names.isEmpty())
				menu.addAction(QStringLiteral("(no shaders in folder)"))->setEnabled(false);
			for (const QString &n : names) {
				QAction *a = menu.addAction(n);
				connect(a, &QAction::triggered, this, [this, n]() { addEffect(n); });
			}
			menu.exec(addEffectBtn_->mapToGlobal(QPoint(0, addEffectBtn_->height())));
		});
		connect(folderBtn, &QPushButton::clicked, this,
			[this]() { QDesktopServices::openUrl(QUrl::fromLocalFile(shadersDirPath())); });
	}

	shaderRenderer_ = std::make_unique<ShaderRenderer>();
	shaderWatch_ = new QFileSystemWatcher(this);
	connect(shaderWatch_, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
		// Editors often replace the file on save (breaking the watch); re-add it and
		// recompile the whole chain from disk so live edits show immediately.
		if (QFile::exists(path) && !shaderWatch_->files().contains(path))
			shaderWatch_->addPath(path);
		for (EditorEffect &e : effects_) {
			EditorEffect fresh;
			QString err;
			if (loadShaderFile(e.name, &fresh, &err)) {
				fresh.params = e.params; // keep current values
				e.defs = fresh.defs;
				e.wrapped = fresh.wrapped;
			}
		}
		recompileChain();
		rebuildEffectsUI();
		refreshPreviewFrame();
	});
	shadersDirPath(); // ensure the folder exists + presets are seeded
	rebuildEffectsUI(); // show the empty-state hint

	// Transform scripts: GUI-thread evaluator + live reload of the folder.
	scriptEval_ = std::make_unique<TransformEvaluator>();
	scriptsDirPath(); // create + seed the examples
	refreshScriptList();
	scriptWatch_ = new QFileSystemWatcher(this);
	scriptWatch_->addPath(scriptsDir_);
	connect(scriptWatch_, &QFileSystemWatcher::directoryChanged, this, [this](const QString &) {
		refreshScriptList();
		reloadScriptsFromDisk();
	});
	connect(scriptWatch_, &QFileSystemWatcher::fileChanged, this, [this](const QString &p) {
		if (QFile::exists(p) && !scriptWatch_->files().contains(p))
			scriptWatch_->addPath(p); // editors replace files on save
		reloadScriptsFromDisk();
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
	buildSpotlightInspector(insLayout);

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

	// Undo/redo history: coalesce a burst of edits (a drag, slider sweep) into
	// one snapshot taken shortly after they settle.
	histTimer_ = new QTimer(this);
	histTimer_->setSingleShot(true);
	histTimer_->setInterval(350);
	connect(histTimer_, &QTimer::timeout, this, &VideoEditorWindow::captureSnapshot);
	new QShortcut(QKeySequence::Undo, this, this, &VideoEditorWindow::undo);
	new QShortcut(QKeySequence::Redo, this, this, &VideoEditorWindow::redo);
	new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Y), this, this, &VideoEditorWindow::redo);
	// Spacebar toggles play/pause of the current preview (the assembled output
	// in Multi-Cut). A window shortcut so it works regardless of which control
	// has focus.
	new QShortcut(QKeySequence(Qt::Key_Space), this, this, &VideoEditorWindow::onPlayPause);

	// ---- Full-editing timeline shortcuts --------------------------------
	// All of them no-op outside Full editing, so they never surprise you in the
	// other modes. Frame-sized steps come from the project frame rate, so a
	// nudge always lands on a frame boundary rather than a round number of ms.
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
	auto add = [this](QKeySequence k, auto fn) {
		auto *sc = new QShortcut(k, this);
		sc->setContext(Qt::WindowShortcut);
		connect(sc, &QShortcut::activated, this, fn);
	};

	add(QKeySequence(Qt::Key_S), [this, onTimeline]() {
		if (onTimeline())
			timelineView_->splitAtPlayhead();
	});
	add(QKeySequence(Qt::CTRL | Qt::Key_K), [this, onTimeline]() {
		if (onTimeline())
			timelineView_->splitAtPlayhead();
	});
	add(QKeySequence(Qt::CTRL | Qt::Key_A), [this, onTimeline]() {
		if (onTimeline())
			timelineView_->selectAllClips();
	});
	add(QKeySequence::Copy, [this, onTimeline]() {
		if (onTimeline())
			copySelectedClips(false);
	});
	add(QKeySequence::Cut, [this, onTimeline]() {
		if (onTimeline())
			copySelectedClips(true);
	});
	add(QKeySequence::Paste, [this, onTimeline]() {
		if (onTimeline())
			pasteClips();
	});
	add(QKeySequence(Qt::Key_M), [this, onTimeline]() {
		if (onTimeline())
			timelineView_->toggleMarkerAtPlayhead();
	});
	// N, not S — S already splits. (Resolve uses N for the magnet too.)
	add(QKeySequence(Qt::Key_N), [this, onTimeline]() {
		if (onTimeline() && snapBtn_)
			snapBtn_->toggle();
	});

	// Arrows nudge a selection, or step the playhead when nothing is selected —
	// the same key doing the obvious thing for what you have in hand.
	auto arrow = [this, onTimeline, frameMs, seekTo](int dir, int frames) {
		if (!onTimeline())
			return;
		const qint64 step = frameMs() * frames * dir;
		if (timelineView_->hasSelection())
			timelineView_->nudgeSelection(step);
		else
			seekTo(timelinePlayheadMs() + step);
	};
	add(QKeySequence(Qt::Key_Left), [arrow]() { arrow(-1, 1); });
	add(QKeySequence(Qt::Key_Right), [arrow]() { arrow(+1, 1); });
	add(QKeySequence(Qt::SHIFT | Qt::Key_Left), [arrow]() { arrow(-1, 10); });
	add(QKeySequence(Qt::SHIFT | Qt::Key_Right), [arrow]() { arrow(+1, 10); });

	// Frame stepping, always the playhead whatever is selected.
	add(QKeySequence(Qt::Key_Comma), [this, onTimeline, frameMs, seekTo]() {
		if (onTimeline())
			seekTo(timelinePlayheadMs() - frameMs());
	});
	add(QKeySequence(Qt::Key_Period), [this, onTimeline, frameMs, seekTo]() {
		if (onTimeline())
			seekTo(timelinePlayheadMs() + frameMs());
	});
	add(QKeySequence(Qt::Key_Home), [this, onTimeline, seekTo]() {
		if (onTimeline())
			seekTo(0);
	});
	add(QKeySequence(Qt::Key_End), [this, onTimeline, seekTo]() {
		if (onTimeline())
			seekTo(timelineView_->durationMs());
	});
	// Jump between markers.
	add(QKeySequence(Qt::CTRL | Qt::Key_Left), [this, onTimeline, seekTo]() {
		if (!onTimeline())
			return;
		const qint64 m = timelineView_->markerNear(timelinePlayheadMs(), false);
		if (m >= 0)
			seekTo(m);
	});
	add(QKeySequence(Qt::CTRL | Qt::Key_Right), [this, onTimeline, seekTo]() {
		if (!onTimeline())
			return;
		const qint64 m = timelineView_->markerNear(timelinePlayheadMs(), true);
		if (m >= 0)
			seekTo(m);
	});

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
	src.seeker = std::move(seeker);
	const int id = src.id;
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
	canvas_->setVideoSize(s->width, s->height);
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

int VideoEditorWindow::addImageSource(const QString &path)
{
	QImage img(path);
	if (img.isNull()) {
		QMessageBox::warning(this, QStringLiteral("Add image"),
				     QStringLiteral("Could not read that image."));
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
		QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp);;All files (*)"));
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
			QStringLiteral("Audio files (*.wav *.mp3 *.m4a *.aac *.flac *.ogg *.opus);;"
				       "All files (*)"));
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
	// The first source sets the project frame rate, like the canvas size does.
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
	if (wantW >= canvas.width())
		return canvas; // the area is bigger than the project: nothing to save
	// Keep the aspect exactly, and never go below a size that is still legible.
	const double k = std::max(0.2, double(wantW) / canvas.width());
	return QSize(std::max(160, int(std::lround(canvas.width() * k))),
		     std::max(90, int(std::lround(canvas.height() * k))));
}

QSize VideoEditorWindow::timelineCanvasSize() const
{
	// The first source defines the output canvas (like the multi-source export).
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
			FrameSeeker *fs = w->seekerFor(sourceId);
			// Decoding straight to the size actually being composited saves both
			// the scale and the memory traffic behind it.
			return fs ? fs->frameAt(srcMs, decodeW, decodeH) : QImage();
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
	requestPreview(-1, outMs); // -1 = "composite the timeline"
}

void VideoEditorWindow::onTimelineHoverScrub(qint64 outMs)
{
	if (playing_)
		return;
	requestPreview(-1, outMs);
}

// ---- Per-clip transform scripting ----------------------------------------

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
	head->setText((expanded ? QStringLiteral("▾  ") : QStringLiteral("▸  ")) + title);
	connect(head, &QPushButton::clicked, this, [head, body, title]() {
		const bool on = !body->isVisible();
		body->setVisible(on);
		head->setText((on ? QStringLiteral("▾  ") : QStringLiteral("▸  ")) + title);
	});
	into->addWidget(head);
	into->addWidget(body);
	return body;
}

namespace {
QString humanBytes(qint64 b)
{
	if (b <= 0)
		return QStringLiteral("—");
	const double kb = b / 1024.0;
	if (kb < 1024.0)
		return QStringLiteral("%1 KB").arg(kb, 0, 'f', 0);
	const double mb = kb / 1024.0;
	if (mb < 1024.0)
		return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
	return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 2);
}

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
	form->addRow(mkKey(QStringLiteral("Format")), pjFormat_);
	form->addRow(mkKey(QStringLiteral("Version")), pjVersion_);
	form->addRow(mkKey(QStringLiteral("Size")), pjSize_);
	form->addRow(mkKey(QStringLiteral("Autosave")), pjAutosave_);
	v->addLayout(form);

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

	const QSize c = timelineCanvasSize();
	double fps = 0.0;
	if (!sources_.empty() && sources_.front().seeker)
		fps = sources_.front().seeker->fps();
	pjFormat_->setText(fps > 1.0 ? QStringLiteral("%1 × %2 @ %3 fps")
					       .arg(c.width())
					       .arg(c.height())
					       .arg(fps, 0, 'f', 2)
				     : QStringLiteral("%1 × %2").arg(c.width()).arg(c.height()));

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
					 .arg(humanBytes(bytes), humanBytes(media))
			       : QStringLiteral("—  (media %1)").arg(humanBytes(media)));

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

// Inverse Selection (Spotlight). Project-level rather than per-clip: it dims the
// composited frame, so it applies to whatever is visible underneath it.
void VideoEditorWindow::buildSpotlightInspector(QVBoxLayout *into)
{
	spotBox_ = new QWidget(this);
	auto *v = new QVBoxLayout(spotBox_);
	v->setContentsMargins(0, 6, 0, 0);
	v->setSpacing(5);

	auto *hdr = new QLabel(QStringLiteral("Inverse Selection"), spotBox_);
	hdr->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed;"));
	v->addWidget(hdr);
	auto *hint = new QLabel(
		QStringLiteral("Dim everything except the chosen areas — the tutorial "
			       "spotlight. Applies to the whole composition, and renders "
			       "identically in the export."),
		spotBox_);
	hint->setWordWrap(true);
	hint->setStyleSheet(QStringLiteral("color:#7f858e;"));
	v->addWidget(hint);

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
		const QColor cur = timelineView_ ? timelineView_->model().spotlight.dimColor
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
		editSpotlight([r, p](SpotlightSpec &s) {
			// `visible` is only reachable from a keyframe, so the pose
			// panel must not stamp over whatever it currently holds.
			SpotPose q = p;
			q.visible = s.masks[r].pose.visible;
			s.masks[r].pose = q;
		});
	};
	for (QDoubleSpinBox *sp : {spotX_, spotY_, spotW_, spotH_, spotRot_, spotRadius_})
		connect(sp, &QDoubleSpinBox::valueChanged, this, [applyPose](double) { applyPose(); });

	into->addWidget(spotBox_);
	syncSpotlightInspector();
}

int VideoEditorWindow::selectedMaskRow() const
{
	if (!spotList_ || !timelineView_)
		return -1;
	const int r = spotList_->currentRow();
	return (r >= 0 && r < timelineView_->model().spotlight.masks.size()) ? r : -1;
}

// Every spotlight change goes through here, so all of them repaint the preview
// and land as one undo step -- the same contract the clip editors have.
void VideoEditorWindow::editSpotlight(const std::function<void(SpotlightSpec &)> &fn)
{
	if (!timelineView_)
		return;
	TimelineModel m = timelineView_->model();
	fn(m.spotlight);
	timelineView_->setModel(m);
	syncSpotlightInspector();
	showTimelineFrame(timelinePlayheadMs());
	commitSnapshot();
}

void VideoEditorWindow::syncSpotlightInspector()
{
	if (!spotBox_ || !timelineView_)
		return;
	// Only meaningful in Full editing: the other modes have no composition to
	// dim.
	spotBox_->setVisible(fullEdit());
	if (!fullEdit())
		return;
	const SpotlightSpec &s = timelineView_->model().spotlight;
	const bool wasSyncing = syncingSpot_;
	syncingSpot_ = true;

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

	auto *form = new QFormLayout;
	form->setContentsMargins(0, 2, 0, 0);
	form->setHorizontalSpacing(8);
	form->setVerticalSpacing(4);
	auto mkSpin = [this](double lo, double hi, double step, int dec) {
		auto *s = new QDoubleSpinBox(clipBox_);
		s->setRange(lo, hi);
		s->setSingleStep(step);
		s->setDecimals(dec);
		s->setKeyboardTracking(false);
		return s;
	};
	zoomSpin_ = mkSpin(0.05, 20.0, 0.05, 2);
	posXSpin_ = mkSpin(-2.0, 3.0, 0.01, 3);
	posYSpin_ = mkSpin(-2.0, 3.0, 0.01, 3);
	opacitySpin_ = mkSpin(0.0, 1.0, 0.05, 2);
	rotationSpin_ = mkSpin(-3600.0, 3600.0, 1.0, 1);
	rotationSpin_->setSuffix(QStringLiteral("°"));
	clipSpeedSpin_ = mkSpin(0.1, 20.0, 0.1, 2);
	clipSpeedSpin_->setToolTip(QStringLiteral(
		"Playback speed for this clip. Its length on the timeline changes to match, and "
		"its audio is time-stretched (pitch preserved) on export."));
	form->addRow(QStringLiteral("Zoom"), zoomSpin_);
	form->addRow(QStringLiteral("Position X"), posXSpin_);
	form->addRow(QStringLiteral("Position Y"), posYSpin_);
	form->addRow(QStringLiteral("Rotation"), rotationSpin_);
	form->addRow(QStringLiteral("Opacity"), opacitySpin_);
	form->addRow(QStringLiteral("Speed"), clipSpeedSpin_);
	// Kept so syncClipInspector can mark the rows a script is driving. Without
	// this the value in the box and the framing on screen disagree with no
	// explanation anywhere.
	poseLabels_ = {qobject_cast<QLabel *>(form->labelForField(zoomSpin_)),
		       qobject_cast<QLabel *>(form->labelForField(posXSpin_)),
		       qobject_cast<QLabel *>(form->labelForField(posYSpin_)),
		       qobject_cast<QLabel *>(form->labelForField(rotationSpin_)),
		       qobject_cast<QLabel *>(form->labelForField(opacitySpin_))};
	v->addLayout(form);
	connect(clipSpeedSpin_, &QDoubleSpinBox::valueChanged, this, [this](double sp) {
		if (syncingClip_)
			return;
		editSelectedClip([sp](TlClip &c) { c.speed = std::clamp(sp, 0.1, 20.0); });
	});

	auto applyPose = [this]() {
		if (syncingClip_)
			return;
		TlTransform tf;
		tf.posX = posXSpin_->value();
		tf.posY = posYSpin_->value();
		tf.scale = zoomSpin_->value();
		tf.rotation = rotationSpin_->value();
		tf.opacity = opacitySpin_->value();
		applySelectedClipTransform(tf);
	};
	for (QDoubleSpinBox *s : {zoomSpin_, posXSpin_, posYSpin_, rotationSpin_, opacitySpin_})
		connect(s, &QDoubleSpinBox::valueChanged, this, [applyPose](double) { applyPose(); });

	auto *resetBtn = new QPushButton(QStringLiteral("Reset transform"), clipBox_);
	connect(resetBtn, &QPushButton::clicked, this, [this]() {
		editSelectedClip([](TlClip &c) {
			c.setBaseTransform(TlTransform{});
			c.keys.clear();
		});
	});
	v->addWidget(resetBtn);

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
	auto *kfPrev = new QPushButton(QStringLiteral("◀"), clipBox_);
	auto *kfAdd = new QPushButton(QStringLiteral("◆ Key"), clipBox_);
	auto *kfDel = new QPushButton(QStringLiteral("✕"), clipBox_);
	auto *kfNext = new QPushButton(QStringLiteral("▶"), clipBox_);
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
	keyInfo_ = new QLabel(QString(), clipBox_);
	keyInfo_->setStyleSheet(QStringLiteral("color:#7f858e;"));
	v->addWidget(keyInfo_);

	// ---- Transform script -------------------------------------------------
	auto *scHdr = new QLabel(QStringLiteral("Script"), clipBox_);
	scHdr->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed; margin-top:6px;"));
	v->addWidget(scHdr);
	auto *scHint = new QLabel(
		QStringLiteral("Drives position/scale/rotation/opacity in code. Channels the script "
			       "leaves out keep the values above."),
		clipBox_);
	scHint->setWordWrap(true);
	scHint->setStyleSheet(QStringLiteral("color:#7f858e;"));
	v->addWidget(scHint);

	// Some Qt builds (the trimmed obs-deps Qt used for Windows releases) ship no
	// Qml module, so there is no JS engine to run scripts with. Say so plainly
	// rather than letting the picker look broken.
	if (!TransformEvaluator::available()) {
		auto *scOff = new QLabel(
			QStringLiteral("Unavailable in this build — it was compiled against a Qt "
				       "with no Qml module, so there is no scripting engine. "
				       "Keyframes and the controls above still work."),
			clipBox_);
		scOff->setWordWrap(true);
		scOff->setStyleSheet(QStringLiteral("color:#d5a642;"));
		v->addWidget(scOff);
	}

	// The stack. Scripts run top-to-bottom, so the list order IS the evaluation
	// order; InternalMove gives real drag-and-drop reordering for free, which is
	// far steadier than hand-rolled card dragging.
	scriptList_ = new QListWidget(clipBox_);
	scriptList_->setEnabled(TransformEvaluator::available());
	scriptList_->setDragDropMode(QAbstractItemView::InternalMove);
	scriptList_->setDefaultDropAction(Qt::MoveAction);
	scriptList_->setSelectionMode(QAbstractItemView::SingleSelection);
	scriptList_->setUniformItemSizes(true);
	scriptList_->setMaximumHeight(112);
	scriptList_->setToolTip(QStringLiteral(
		"Scripts run top to bottom — drag to reorder. Each starts from what the one above "
		"produced, so scripts driving different channels combine, and on a shared channel "
		"the lower one wins."));
	v->addWidget(scriptList_);

	connect(scriptList_, &QListWidget::currentRowChanged, this, [this](int row) {
		if (syncingClip_)
			return;
		scriptSel_ = row;
		rebuildScriptParams(); // show the newly selected entry's controls
	});
	// A drag finishing rewrites the clip's stack in the list's new order.
	//
	// Which signal that is depends on how Qt implements the move: QListWidget's
	// InternalMove drop goes through dropMimeData and emits rowsInserted (plus a
	// separate removal of the source row) rather than rowsMoved, so listening for
	// rowsMoved alone silently never fired. Listen for every mutation and read
	// the order back on the next event-loop turn, once the drop has settled —
	// applyScriptOrderFromList ignores the half-finished states in between.
	const auto onListMutated = [this]() {
		if (syncingClip_ || scriptOrderSyncPending_)
			return;
		scriptOrderSyncPending_ = true;
		QTimer::singleShot(0, this, [this]() {
			scriptOrderSyncPending_ = false;
			applyScriptOrderFromList();
		});
	};
	connect(scriptList_->model(), &QAbstractItemModel::rowsMoved, this, onListMutated);
	connect(scriptList_->model(), &QAbstractItemModel::rowsInserted, this, onListMutated);
	connect(scriptList_->model(), &QAbstractItemModel::rowsRemoved, this, onListMutated);
	connect(scriptList_->model(), &QAbstractItemModel::layoutChanged, this, onListMutated);
	connect(scriptList_->model(), &QAbstractItemModel::modelReset, this, onListMutated);

	auto *stkBtns = new QHBoxLayout;
	addScriptBtn_ = new QPushButton(QStringLiteral("Add script  ▾"), clipBox_);
	addScriptBtn_->setEnabled(TransformEvaluator::available());
	addScriptBtn_->setToolTip(QStringLiteral("Stack another transform script on this clip"));
	auto *scDel = new QPushButton(QStringLiteral("✕"), clipBox_);
	scDel->setFixedWidth(28);
	scDel->setToolTip(QStringLiteral("Remove the selected script"));
	stkBtns->addWidget(addScriptBtn_, 1);
	stkBtns->addWidget(scDel);
	v->addLayout(stkBtns);

	connect(addScriptBtn_, &QPushButton::clicked, this, [this]() {
		QMenu menu(this);
		const QStringList names = availableScripts();
		if (names.isEmpty())
			menu.addAction(QStringLiteral("(no scripts in folder)"))->setEnabled(false);
		for (const QString &n : names) {
			QAction *a = menu.addAction(n);
			connect(a, &QAction::triggered, this, [this, n]() { addScriptToClip(n); });
		}
		menu.exec(addScriptBtn_->mapToGlobal(QPoint(0, addScriptBtn_->height())));
	});
	connect(scDel, &QPushButton::clicked, this, [this]() { removeScriptFromClip(scriptSel_); });

	auto *scBtns = new QHBoxLayout;
	auto *scReload = new QPushButton(QStringLiteral("Reload"), clipBox_);
	scReload->setToolTip(QStringLiteral("Recompile the scripts after editing them on disk"));
	auto *scFolder = new QPushButton(QStringLiteral("Folder"), clipBox_);
	scFolder->setToolTip(QStringLiteral("Open the scripts folder — drop .js files here"));
	scBtns->addWidget(scReload);
	scBtns->addWidget(scFolder);
	v->addLayout(scBtns);
	connect(scReload, &QPushButton::clicked, this, [this]() {
		refreshScriptList();
		reloadScriptsFromDisk();
	});
	connect(scFolder, &QPushButton::clicked, this,
		[this]() { QDesktopServices::openUrl(QUrl::fromLocalFile(scriptsDirPath())); });

	scriptError_ = new QLabel(QString(), clipBox_);
	scriptError_->setWordWrap(true);
	scriptError_->setStyleSheet(
		QStringLiteral("color:#e5484d; font-family:monospace; font-size:11px;"));
	scriptError_->setVisible(false);
	v->addWidget(scriptError_);

	scriptParamBox_ = new QWidget(clipBox_);
	auto *spl = new QFormLayout(scriptParamBox_);
	spl->setContentsMargins(0, 2, 0, 0);
	spl->setHorizontalSpacing(8);
	spl->setVerticalSpacing(4);
	v->addWidget(scriptParamBox_);

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
	auto *presetDel = new QPushButton(QStringLiteral("✕"), textBox_);
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
void VideoEditorWindow::openKeyframeEditor()
{
	if (!timelineView_)
		return;
	const TlClip *sel = timelineView_->selectedClipPtr();
	if (!sel)
		return;
	if (!keyEditor_) {
		keyEditor_ = new KeyframeEditor(this);
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
// number in the Inspector box is no longer what you see on screen — it becomes
// the script's STARTING point (the script reads it as ctx.base). Say so on the
// row instead of leaving the mismatch to be discovered.
void VideoEditorWindow::markScriptDrivenRows(const TlClip &c)
{
	if (poseLabels_.isEmpty())
		return;
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
	// Same order the rows were added in.
	const int chan[5] = {TransformEvaluator::ChanScale,    TransformEvaluator::ChanPosition,
			     TransformEvaluator::ChanPosition, TransformEvaluator::ChanRotation,
			     TransformEvaluator::ChanOpacity};
	static const char *names[5] = {"Zoom", "Position X", "Position Y", "Rotation", "Opacity"};
	for (int i = 0; i < poseLabels_.size() && i < 5; ++i) {
		QLabel *lb = poseLabels_[i];
		if (!lb)
			continue;
		const bool driven = (mask & chan[i]) != 0;
		lb->setText(driven ? QStringLiteral("%1  ⟡").arg(QLatin1String(names[i]))
				   : QLatin1String(names[i]));
		lb->setStyleSheet(driven ? QStringLiteral("color:#ffd44f;") : QString());
		lb->setToolTip(driven ? QStringLiteral(
					       "Driven by %1. This value is where the script "
					       "starts from (it reads it as ctx.base), not the "
					       "framing you see — change it and the whole move "
					       "shifts with it.")
					       .arg(driving.join(QStringLiteral(", ")))
				      : QString());
	}
}

void VideoEditorWindow::syncClipInspector()
{
	if (!clipBox_ || !timelineView_)
		return;
	const TlClip *c = fullEdit() ? timelineView_->selectedClipPtr() : nullptr;
	clipBox_->setVisible(c != nullptr);
	if (!c)
		return;

	// Save/restore rather than clear: a live edit can reach here while an outer
	// sync is already in progress, and clearing the flag would let the widgets
	// being repopulated write back into the clip.
	const bool wasSyncing = syncingClip_;
	syncingClip_ = true;
	const qint64 ph = timelinePlayheadMs();
	const TlTransform tf = c->transformAt(ph);
	posXSpin_->setValue(tf.posX);
	posYSpin_->setValue(tf.posY);
	zoomSpin_->setValue(tf.scale);
	rotationSpin_->setValue(tf.rotation);
	opacitySpin_->setValue(tf.opacity);
	clipSpeedSpin_->setValue(c->speed);
	clipSpeedSpin_->setEnabled(!c->freeDuration()); // stills/captions have no source clock
	autoKeyChk_->setChecked(autoKeyframe_);
	const int here = c->keyframeIndexAt(ph);
	keyInfo_->setText(c->keys.isEmpty()
				  ? QStringLiteral("No animation — the clip holds one fixed framing.")
				  : QStringLiteral("%1 keyframe%2%3")
					    .arg(c->keys.size())
					    .arg(c->keys.size() == 1 ? QString() : QStringLiteral("s"))
					    .arg(here >= 0 ? QStringLiteral(" · on one now") : QString()));

	// Transform script stack + the selected entry's parameter controls.
	refreshScriptList();
	rebuildScriptParams();
	markScriptDrivenRows(*c);

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

	const bool isText = !onAudioTrack && c->type == TlClip::Type::Text;
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
	qint64 target = -1;
	if (dir > 0) {
		for (const TlKeyframe &k : sel->keys)
			if (k.tMs > rel + 1) {
				target = k.tMs;
				break;
			}
	} else {
		for (int i = sel->keys.size() - 1; i >= 0; --i)
			if (sel->keys[i].tMs < rel - 1) {
				target = sel->keys[i].tMs;
				break;
			}
	}
	if (target < 0)
		return;
	onTimelineScrub(sel->outStartMs + target);
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

void VideoEditorWindow::syncPreviewTransformTarget()
{
	if (!canvas_ || !timelineView_)
		return;
	const TlClip *c = fullEdit() ? timelineView_->selectedClipPtr() : nullptr;
	canvas_->setTransformMode(c != nullptr);
	if (!c) {
		canvas_->setTransformRect(QRectF());
		return;
	}
	// Outline the clip where it currently sits on the canvas.
	const QSize canvasSize = timelineCanvasSize();
	const qint64 ph = timelinePlayheadMs();
	const TlTransform tf = c->transformAt(ph);
	QSize natural;
	if (c->type == TlClip::Type::Text) {
		natural = TimelineCompositor::textNaturalSize(c->text, canvasSize);
	} else if (const auto it = stillImages_.constFind(c->sourceId);
		   it != stillImages_.constEnd()) {
		natural = it.value().size();
	} else if (EditorSource *s = sourceById(c->sourceId)) {
		natural = (!c->crop.isNull() && c->crop.width() > 1) ? c->crop.size()
								    : QSize(s->width, s->height);
	}
	canvas_->setTransformRect(natural.isEmpty()
					  ? QRectF()
					  : TimelineCompositor::clipRectOnCanvas(tf, canvasSize, natural));
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

void VideoEditorWindow::dragEnterEvent(QDragEnterEvent *e)
{
	if (!e->mimeData()->hasUrls())
		return;
	for (const QUrl &u : e->mimeData()->urls())
		if (isVideoFile(u.toLocalFile())) {
			e->acceptProposedAction();
			return;
		}
}

void VideoEditorWindow::dropEvent(QDropEvent *e)
{
	int firstAdded = -1;
	for (const QUrl &u : e->mimeData()->urls()) {
		const QString f = u.toLocalFile();
		if (f.isEmpty() || !isVideoFile(f))
			continue;
		const int id = addSource(f);
		if (id >= 0 && firstAdded < 0)
			firstAdded = id;
	}
	if (firstAdded >= 0) {
		setActiveSource(firstAdded);
		e->acceptProposedAction();
	}
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

bool VideoEditorWindow::eventFilter(QObject *watched, QEvent *e)
{
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
	return false;
}

void VideoEditorWindow::reject()
{
	// Exports save to a NEW file, so "unsaved" means any edit that would be
	// lost by closing now. A successful export closes via accept() instead.
	if (hasUnsavedEdits()) {
		stopPlayback();
		QMessageBox box(this);
		box.setWindowTitle(QStringLiteral("Discard changes?"));
		box.setIcon(QMessageBox::Warning);
		box.setText(QStringLiteral("You have unsaved edits in this video."));
		box.setInformativeText(
			QStringLiteral("Closing the window will discard everything. Use Save… to export first."));
		QPushButton *closeBtn =
			box.addButton(QStringLiteral("Close window"), QMessageBox::DestructiveRole);
		QPushButton *cancelBtn =
			box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
		box.setDefaultButton(cancelBtn);
		box.exec();
		if (box.clickedButton() != closeBtn)
			return; // keep editing
	}
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
		voRecordBtn_->setText(QStringLiteral("●  Record voiceover"));
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
		voRecordBtn_->setText(QStringLiteral("●  Record voiceover"));
		return;
	}
	voRecording_ = true;
	// Anchor the take at the output-time under the playhead (0 when idle).
	voClipStartMs_ = currentOutputMs();
	voRecordBtn_->setText(QStringLiteral("■  Stop"));
	voStatus_->setStyleSheet(QStringLiteral("color:#e5484d;"));
	voStatus_->setText(QStringLiteral("● Recording"));
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
	voRecordBtn_->setText(QStringLiteral("●  Record voiceover"));
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
		QStringLiteral("Audio (*.mp3 *.wav *.m4a *.aac *.ogg *.flac *.opus *.wma);;All files (*)"));
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
		// Inverse Selection is project-level, so it sits next to the tracks
		// rather than on any one clip. Only written when it differs from the
		// default, to keep an untouched project's file clean.
		if (s.timeline.spotlight != SpotlightSpec())
			root[QStringLiteral("spotlight")] = spotlightToJson(s.timeline.spotlight);
		root[QStringLiteral("harpiaProject")] = 3; // timelines need a v3 reader
	}

	// Post-processing effect stack (names reference .frag files in the user folder).
	if (!s.effects.isEmpty()) {
		QJsonArray fxArr;
		for (const ShaderState &st : s.effects) {
			QJsonObject fo;
			fo[QStringLiteral("name")] = st.name;
			QJsonObject params;
			for (auto it = st.params.constBegin(); it != st.params.constEnd(); ++it)
				params[it.key()] = it.value();
			fo[QStringLiteral("params")] = params;
			fxArr.append(fo);
		}
		root[QStringLiteral("effects")] = fxArr;
	}

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
	const QJsonObject root = doc.object();
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
				if (!QFileInfo::exists(spath)) {
					const QString picked = QFileDialog::getOpenFileName(
						this,
						QStringLiteral("Locate \"%1\"")
							.arg(sname.isEmpty() ? QFileInfo(spath).fileName()
									     : sname),
						QFileInfo(path).absolutePath(),
						QStringLiteral("Video files (*.mp4 *.mov *.mkv *.webm *.avi "
							       "*.m4v *.gif *.wmv *.flv *.ts);;All files (*)"));
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
		if (!projSourceName.isEmpty() && projSourceName != QFileInfo(inPath_).fileName()) {
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
		v.peaks = VoiceoverTrack::loadPeaks(v.path, 600); // for the waveform
		s.voiceClips.push_back(v);
	}

	// "Full editing" timeline (project v3). Absent in v1/v2 projects, which just
	// restore an empty timeline.
	s.timeline.spotlight = spotlightFromJson(root.value(QStringLiteral("spotlight")).toObject());
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

		// Waveforms are a derived cache, so they are not stored in the project —
		// but they DO have to be rebuilt, or an audio clip reopens as a blank bar.
		// One decode per source, not per clip.
		if (t.kind == TlTrack::Kind::Audio) {
			QHash<int, QVector<float>> peaksBySource;
			for (TlClip &c : t.clips) {
				if (c.type == TlClip::Type::Text || c.type == TlClip::Type::Image)
					continue;
				auto it = peaksBySource.constFind(c.sourceId);
				if (it == peaksBySource.constEnd()) {
					const EditorSource *es = sourceById(c.sourceId);
					it = peaksBySource.insert(
						c.sourceId,
						es ? VoiceoverTrack::loadPeaks(es->path, 600)
						   : QVector<float>());
				}
				c.peaks = it.value();
			}
		}
		s.timeline.tracks.append(t);
	}

	// Post-processing effect stack (restored by restoreSnapshot below). Back-compat:
	// older projects stored a single "shader" object instead of an "effects" array.
	auto readEffect = [](const QJsonObject &fo) {
		ShaderState st;
		st.name = fo.value(QStringLiteral("name")).toString();
		const QJsonObject params = fo.value(QStringLiteral("params")).toObject();
		for (auto it = params.constBegin(); it != params.constEnd(); ++it)
			st.params[it.key()] = it.value().toDouble();
		return st;
	};
	if (root.value(QStringLiteral("effects")).isArray()) {
		for (const QJsonValue &jv : root.value(QStringLiteral("effects")).toArray()) {
			const ShaderState st = readEffect(jv.toObject());
			if (!st.name.isEmpty())
				s.effects.push_back(st);
		}
	} else if (root.value(QStringLiteral("shader")).isObject()) {
		const ShaderState st = readEffect(root.value(QStringLiteral("shader")).toObject());
		if (!st.name.isEmpty())
			s.effects.push_back(st);
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
	for (const EditorEffect &e : effects_)
		s.effects.push_back(ShaderState{e.name, e.params});
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

	// Restore the effect stack: reload each named shader from disk and apply the
	// saved parameter values on top of its defaults.
	effects_.clear();
	for (const ShaderState &st : s.effects) {
		EditorEffect e;
		e.params = st.params;
		QString err;
		if (loadShaderFile(st.name, &e, &err))
			effects_.append(e);
	}
	recompileChain();
	rebuildEffectsUI();
	updateShaderWatch();

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
		playBtn_->setText(QStringLiteral("⏸"));
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
		playBtn_->setText(QStringLiteral("⏸"));
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
	playBtn_->setText(QStringLiteral("⏸"));
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
	playBtn_->setText(QStringLiteral("▶"));
	playTimer_->stop();
	startAudioWhenReady_ = false; // a mix still running must not start on arrival
	if (audioPreview_)
		audioPreview_->stop();
	if (voTrack_ && !voRecording_)
		voTrack_->clearPlayhead();
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

// ---- Post-processing shader effect --------------------------------------

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

QStringList VideoEditorWindow::availableShaders()
{
	QStringList names;
	for (const QFileInfo &fi :
	     QDir(shadersDirPath()).entryInfoList({QStringLiteral("*.frag")}, QDir::Files, QDir::Name))
		names << fi.completeBaseName();
	return names;
}

bool VideoEditorWindow::loadShaderFile(const QString &name, EditorEffect *out, QString *err)
{
	const QString path = shadersDirPath() + QLatin1Char('/') + name + QStringLiteral(".frag");
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		if (err)
			*err = QStringLiteral("Could not read %1.frag").arg(name);
		return false;
	}
	const QString glsl = QString::fromUtf8(f.readAll());
	f.close();
	out->name = name;
	out->defs = parseShaderParams(glsl);
	out->wrapped = wrapShaderToy(glsl, out->defs);
	for (const ShaderParam &p : out->defs) // default any unset params
		if (!out->params.contains(p.uniform))
			out->params[p.uniform] = p.def;
	return true;
}

void VideoEditorWindow::addEffect(const QString &name)
{
	EditorEffect e;
	QString err;
	if (!loadShaderFile(name, &e, &err)) {
		if (shaderError_) {
			shaderError_->setText(err);
			shaderError_->setVisible(true);
		}
		return;
	}
	effects_.append(e);
	recompileChain();
	rebuildEffectsUI();
	updateShaderWatch();
	refreshPreviewFrame();
	scheduleSnapshot();
}

void VideoEditorWindow::removeEffect(int index)
{
	if (index < 0 || index >= effects_.size())
		return;
	effects_.remove(index);
	recompileChain();
	rebuildEffectsUI();
	updateShaderWatch();
	refreshPreviewFrame();
	scheduleSnapshot();
}

void VideoEditorWindow::moveEffect(int index, int delta)
{
	const int to = index + delta;
	if (index < 0 || index >= effects_.size() || to < 0 || to >= effects_.size())
		return;
	effects_.move(index, to);
	recompileChain();
	rebuildEffectsUI();
	refreshPreviewFrame();
	scheduleSnapshot();
}

void VideoEditorWindow::recompileChain()
{
	if (!shaderRenderer_)
		return;
	QVector<ShaderLayerSource> layers;
	layers.reserve(effects_.size());
	for (const EditorEffect &e : effects_)
		layers.append({e.wrapped, e.defs});
	QString err;
	const bool ok = shaderRenderer_->setChain(layers, &err);
	if (shaderError_) {
		shaderError_->setText(ok ? QString() : err);
		shaderError_->setVisible(!ok);
	}
}

void VideoEditorWindow::updateShaderWatch()
{
	if (!shaderWatch_)
		return;
	if (!shaderWatch_->files().isEmpty())
		shaderWatch_->removePaths(shaderWatch_->files());
	for (const EditorEffect &e : effects_) {
		const QString p = shadersDirPath() + QLatin1Char('/') + e.name + QStringLiteral(".frag");
		if (QFile::exists(p))
			shaderWatch_->addPath(p);
	}
}

void VideoEditorWindow::rebuildEffectsUI()
{
	if (!effectsBox_)
		return;
	auto *box = qobject_cast<QVBoxLayout *>(effectsBox_->layout());
	if (!box)
		return;
	// Clear existing children.
	while (QLayoutItem *item = box->takeAt(0)) {
		if (QWidget *w = item->widget())
			w->deleteLater();
		delete item;
	}

	if (effects_.isEmpty()) {
		auto *empty = new QLabel(
			QStringLiteral("No effects yet — click “Add effect” to grade or stylize the video."),
			effectsBox_);
		empty->setWordWrap(true);
		empty->setStyleSheet(QStringLiteral("color:#7f858e;"));
		box->addWidget(empty);
		return;
	}

	for (int i = 0; i < effects_.size(); ++i) {
		const EditorEffect &e = effects_[i];
		auto *card = new QWidget(effectsBox_);
		card->setObjectName(QStringLiteral("fxCard"));
		card->setStyleSheet(QStringLiteral(
			"QWidget#fxCard { background:#1c1e22; border:1px solid #2c2f36; border-radius:4px; }"));
		auto *cv = new QVBoxLayout(card);
		cv->setContentsMargins(8, 6, 8, 8);
		cv->setSpacing(4);

		auto *hdr = new QHBoxLayout;
		auto *title = new QLabel(QStringLiteral("%1. %2").arg(i + 1).arg(e.name), card);
		title->setStyleSheet(QStringLiteral("color:#e8eaed; font-weight:bold;"));
		hdr->addWidget(title, 1);
		auto *up = new QPushButton(QStringLiteral("↑"), card);
		auto *down = new QPushButton(QStringLiteral("↓"), card);
		auto *del = new QPushButton(QStringLiteral("✕"), card);
		up->setFixedWidth(26);
		down->setFixedWidth(26);
		del->setFixedWidth(26);
		up->setEnabled(i > 0);
		down->setEnabled(i < effects_.size() - 1);
		up->setToolTip(QStringLiteral("Move earlier in the chain"));
		down->setToolTip(QStringLiteral("Move later in the chain"));
		del->setToolTip(QStringLiteral("Remove this effect"));
		hdr->addWidget(up);
		hdr->addWidget(down);
		hdr->addWidget(del);
		cv->addLayout(hdr);
		connect(up, &QPushButton::clicked, this, [this, i]() { moveEffect(i, -1); });
		connect(down, &QPushButton::clicked, this, [this, i]() { moveEffect(i, +1); });
		connect(del, &QPushButton::clicked, this, [this, i]() { removeEffect(i); });

		auto *form = new QFormLayout;
		form->setContentsMargins(0, 2, 0, 0);
		form->setHorizontalSpacing(8);
		form->setVerticalSpacing(3);
		for (const ShaderParam &p : e.defs) {
			const double val = e.params.value(p.uniform, p.def);
			auto *key = new QLabel(p.label, card);
			key->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
			if (p.type == ShaderParam::Type::Bool) {
				auto *cb = new QCheckBox(card);
				cb->setChecked(val != 0.0);
				connect(cb, &QCheckBox::toggled, this, [this, i, u = p.uniform](bool on) {
					if (i < effects_.size())
						effects_[i].params[u] = on ? 1.0 : 0.0;
					refreshPreviewFrame();
					scheduleSnapshot();
				});
				form->addRow(key, cb);
			} else {
				auto *roww = new QWidget(card);
				auto *rl = new QHBoxLayout(roww);
				rl->setContentsMargins(0, 0, 0, 0);
				rl->setSpacing(6);
				auto *sl = new QSlider(Qt::Horizontal, roww);
				sl->setRange(0, 1000);
				const double span = (p.max > p.min) ? (p.max - p.min) : 1.0;
				sl->setValue(int(std::clamp((val - p.min) / span, 0.0, 1.0) * 1000.0));
				auto *vlab = new QLabel(QString::number(val, 'g', 3), roww);
				vlab->setMinimumWidth(40);
				vlab->setStyleSheet(QStringLiteral("color:#c8ccd4; font-family:monospace;"));
				rl->addWidget(sl, 1);
				rl->addWidget(vlab);
				connect(sl, &QSlider::valueChanged, this,
					[this, i, u = p.uniform, mn = p.min, sp = span, vlab](int v) {
						const double d = mn + (double(v) / 1000.0) * sp;
						if (i < effects_.size())
							effects_[i].params[u] = d;
						vlab->setText(QString::number(d, 'g', 3));
						refreshPreviewFrame();
						scheduleSnapshot();
					});
				form->addRow(key, roww);
			}
		}
		cv->addLayout(form);
		box->addWidget(card);
	}
}

QImage VideoEditorWindow::runShader(const QImage &img, qint64 ms)
{
	if (effects_.isEmpty() || !shaderRenderer_ || !shaderRenderer_->hasChain() || img.isNull())
		return img;
	QVector<QMap<QString, double>> perLayer;
	perLayer.reserve(effects_.size());
	for (const EditorEffect &e : effects_)
		perLayer.append(e.params);
	const float t = float(ms) / 1000.0f;
	return shaderRenderer_->apply(img, t, int(ms / 33), perLayer);
}

void VideoEditorWindow::setPreviewFrame(const QImage &img, qint64 ms)
{
	lastPreviewRaw_ = img;
	lastPreviewMs_ = ms;
	canvas_->setFrame(runShader(img, ms));
}

void VideoEditorWindow::refreshPreviewFrame()
{
	if (canvas_ && !lastPreviewRaw_.isNull())
		canvas_->setFrame(runShader(lastPreviewRaw_, lastPreviewMs_));
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
	FrameSeeker *fs = seekerFor(sourceId);
	if (!fs)
		fs = seeker_; // fall back to the active source
	if (!fs)
		return;
	// Trim / Multi-Cut show one source frame; the same quality setting applies,
	// against the 720p these modes have always previewed at.
	const QSize dec = previewRenderSize(QSize(1280, 720));
	const QImage img = fs->frameAt(ms, dec.width(), dec.height());
	if (!img.isNull())
		setPreviewFrame(img, ms);
}

void VideoEditorWindow::onCropToggled(bool on)
{
	canvas_->setCropEnabled(on);
}

void VideoEditorWindow::onSave()
{
	if (!valid_ || exporter_) // ignore while an export is already running
		return;
	stopPlayback();

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
	ExportOptionsDialog dlg(base, /*allowGif=*/!cuts, this);
	if (dlg.exec() != QDialog::Accepted)
		return;

	QString name = dlg.fileName();
	if (name.isEmpty())
		name = base;
	const ClipExporter::Format fmt = dlg.format();
	const QString ext = ClipExporter::extensionFor(fmt);
	outPath_ = QFileInfo(inPath_).absolutePath() + QLatin1Char('/') + name + QLatin1Char('.') + ext;

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
	o.gifWidth = 0;     // output = cropped area / full video size (no downscale)
	o.speed = speed_;   // from the editor's speed slider
	o.videoCrf = dlg.videoCrf();
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
		o.timeline = timelineView_->model();
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
		// Match the primary source's frame rate when we know it.
		double fps = 30.0;
		if (!sources_.empty() && sources_.front().seeker && sources_.front().seeker->fps() > 1.0)
			fps = sources_.front().seeker->fps();
		o.timelineFps = fps;
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

	// Bake the post-processing effect stack into the output frames (ignored for
	// GIF, which uses a separate palette-based encoder).
	for (const EditorEffect &e : effects_)
		o.effects.push_back({e.wrapped, e.defs, e.params});

	exporter_ = new ClipExporter(this);
	connect(exporter_, &ClipExporter::progress, this, &VideoEditorWindow::onExportProgress);
	connect(exporter_, &ClipExporter::finished, this, &VideoEditorWindow::onExportFinished);

	progress_ = new QProgressDialog(QStringLiteral("Exporting %1…").arg(ext.toUpper()),
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
