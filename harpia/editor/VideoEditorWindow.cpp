#include "VideoEditorWindow.hpp"

#include "AudioRecorder.hpp"
#include "ClipExporter.hpp"
#include "DevPanel.hpp"
#include "EditorWidgets.hpp"
#include "ExportOptionsDialog.hpp"
#include "FrameSeeker.hpp"
#include "LevelMeter.hpp"
#include "SceneDetector.hpp"
#include "TimelineThumbs.hpp"
#include "TrackEditor.hpp"
#include "VoiceoverMixer.hpp"
#include "VoiceoverTrack.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QButtonGroup>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QIcon>
#include <QListWidget>
#include <QMimeData>
#include <QPixmap>
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
	QProcess::startDetached(QStringLiteral("explorer.exe"),
				{QStringLiteral("/select,") + QDir::toNativeSeparators(path)});
#else
	QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}
} // namespace

VideoEditorWindow::VideoEditorWindow(const QString &inPath, QWidget *parent)
	: QDialog(parent), inPath_(inPath)
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
	auto *modeGroup = new QButtonGroup(this);
	modeGroup->setExclusive(true); // only one mode active; can't un-check both
	modeGroup->addButton(trimModeBtn_);
	modeGroup->addButton(cutModeBtn_);
	// Segmented look: shared fill, joined borders, accent on the active segment.
	const QString segBase = QStringLiteral(
		"QPushButton{background:#2b2f36;color:#c8ccd4;border:1px solid #3a3f47;padding:5px 14px;}"
		"QPushButton:checked{background:#3d7eff;color:#ffffff;border-color:#3d7eff;}");
	trimModeBtn_->setStyleSheet(segBase + QStringLiteral("QPushButton{border-right:none;}"));
	cutModeBtn_->setStyleSheet(segBase);
	auto *segBox = new QHBoxLayout;
	segBox->setSpacing(0); // no gap — the two segments read as one control
	segBox->setContentsMargins(0, 0, 0, 0);
	segBox->addWidget(trimModeBtn_);
	segBox->addWidget(cutModeBtn_);
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

	// Speed: label · slider (stretches) · editable value · per-cut count.
	bar->addWidget(new QLabel(QStringLiteral("Speed"), this));
	speedSlider_ = new QSlider(Qt::Horizontal, this);
	speedSlider_->setRange(0, kSpeedTicks); // exponential 0.1×..50×
	speedSlider_->setPageStep(kSpeedTicks / 20);
	speedSlider_->setValue(speedToSlider(1.0));
	speedSlider_->setMinimumWidth(140);
	speedSlider_->setToolTip(QStringLiteral("Playback speed (0.1×–50×); scaled so low speeds are easy to fine-tune"));
	bar->addWidget(speedSlider_, 1); // takes the slack so Inspector/Dev pin right
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
	speedLabel_->setMinimumWidth(56);
	speedLabel_->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	bar->addWidget(speedLabel_);
	bar->addSpacing(10);

	// Inspector toggle — show/hide the right-side properties panel.
	inspectorBtn_ = new QPushButton(QStringLiteral("Inspector"), this);
	inspectorBtn_->setCheckable(true);
	inspectorBtn_->setChecked(true);
	inspectorBtn_->setToolTip(QStringLiteral(
		"Show/hide the properties panel for the selected cut (uses the space beside portrait previews)"));
	connect(inspectorBtn_, &QPushButton::toggled, this, [this](bool on) {
		if (inspector_)
			inspector_->setVisible(on);
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
			devPanel_ = new DevPanel(timeline_, tracks_, voTrack_, canvas_, this);
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
	auto *bottomLayout = new QVBoxLayout(bottomPane);
	bottomLayout->setContentsMargins(0, 0, 0, 0);

	timeline_ = new Timeline(this);
	tracks_ = new TrackEditor(this);
	stack_ = new QStackedWidget(this);
	stack_->addWidget(timeline_); // index 0 = Simple Trim
	stack_->addWidget(tracks_);   // index 1 = Multi-Cut
	bottomLayout->addWidget(stack_);

	// ---- Voiceover: a collapsible narration section (record over the video).
	// Collapsed by default so detailed cut work keeps the vertical space; the
	// expand/collapse state is remembered across launches.
	auto *audioHeader = new QPushButton(this);
	audioHeader->setFlat(true);
	audioHeader->setCursor(Qt::PointingHandCursor);
	audioHeader->setStyleSheet(QStringLiteral(
		"QPushButton{text-align:left; padding:2px; color:#c8ccd4; font-weight:bold; border:none;}"
		"QPushButton:hover{color:#e8eaed;}"));
	bottomLayout->addWidget(audioHeader);

	auto *audioBody = new QWidget(this);
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
	controls->addWidget(resetCrop);
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

	// ---- Right-side inspector: properties of the selected cut / trim range.
	// Portrait clips leave wide black bars beside the preview; this panel puts
	// that space to work as a live clip inspector.
	inspector_ = new QWidget(this);
	inspector_->setMinimumWidth(180);
	auto *insLayout = new QVBoxLayout(inspector_);
	insLayout->setContentsMargins(10, 8, 10, 8);
	insLayout->setSpacing(6);
	auto *insHeader = new QLabel(QStringLiteral("Inspector"), this);
	insHeader->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8eaed;"));
	insLayout->addWidget(insHeader);
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
	insLayout->addStretch(1);

	// ---- Left sidebar: the list of sources (videos) you can cut from -------
	auto *sidebar = new QWidget(this);
	auto *sideLayout = new QVBoxLayout(sidebar);
	sideLayout->setContentsMargins(6, 4, 6, 6);
	sideLayout->setSpacing(6);
	auto *sideHeader = new QLabel(QStringLiteral("Sources"), this);
	sideHeader->setStyleSheet(QStringLiteral("font-weight:bold; color:#c8ccd4;"));
	sideLayout->addWidget(sideHeader);
	sourceList_ = new QListWidget(this);
	sourceList_->setIconSize(QSize(96, 54));
	sourceList_->setToolTip(QStringLiteral(
		"Videos you can cut from. Drop video files here (or anywhere on the window) "
		"to add more; click one to cut from it."));
	sideLayout->addWidget(sourceList_, 1);
	auto *addSrcBtn = new QPushButton(QStringLiteral("Add video…"), this);
	addSrcBtn->setToolTip(QStringLiteral("Add another video as a source (or drag files onto the window)"));
	connect(addSrcBtn, &QPushButton::clicked, this, &VideoEditorWindow::onAddSource);
	sideLayout->addWidget(addSrcBtn);
	auto *removeSrcBtn = new QPushButton(QStringLiteral("Remove"), this);
	removeSrcBtn->setToolTip(QStringLiteral("Remove the selected source (only if no cut uses it)"));
	connect(removeSrcBtn, &QPushButton::clicked, this, &VideoEditorWindow::onRemoveSource);
	sideLayout->addWidget(removeSrcBtn);
	connect(sourceList_, &QListWidget::itemSelectionChanged, this,
		&VideoEditorWindow::onSourceRowChanged);

	// Left sources | (preview/editing split) | right inspector.
	auto *hsplit = new QSplitter(Qt::Horizontal, this);
	hsplit->setChildrenCollapsible(false);
	hsplit->setHandleWidth(6);
	hsplit->addWidget(sidebar);
	hsplit->addWidget(splitter);
	hsplit->addWidget(inspector_);
	hsplit->setStretchFactor(0, 1); // sources
	hsplit->setStretchFactor(1, 5); // editing area takes the lion's share
	hsplit->setStretchFactor(2, 1); // inspector
	hsplit->setSizes({170, 700, 190});
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
	connect(trimModeBtn_, &QPushButton::clicked, this, [this]() { setEditMode(false); });
	connect(cutModeBtn_, &QPushButton::clicked, this, [this]() { setEditMode(true); });
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
}

void VideoEditorWindow::applyChrome(const EditorChromeParams &p)
{
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

	if (speedSlider_)
		speedSlider_->setMinimumWidth(p.speedSliderMinW);
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
		refreshSourceList();
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
		auto *item = new QListWidgetItem(s.name);
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
	if (activeSourceId_ == id)
		setActiveSource(sources_.front().id);
	else
		refreshSourceList();
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

bool VideoEditorWindow::multiCut() const
{
	return stack_ && stack_->currentIndex() == 1;
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

void VideoEditorWindow::setEditMode(bool cut)
{
	stopPlayback();
	trimModeBtn_->setChecked(!cut);
	cutModeBtn_->setChecked(cut);
	stack_->setCurrentIndex(cut ? 1 : 0);
	if (cut) {
		playBtn_->setToolTip(QStringLiteral("Loop-play the assembled output"));
		onSegmentSelected(tracks_->selectedIndex()); // rebind the speed slider
	} else {
		playBtn_->setToolTip(
			QStringLiteral("Loop-play the trimmed section at the current speed"));
		speedSlider_->setEnabled(valid_);
		speedSpin_->setEnabled(valid_);
		syncSpeedControls(speed_);
		speedLabel_->setText(QString()); // no per-cut count in Simple Trim
	}
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

void VideoEditorWindow::updateInspector()
{
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
	joinExport();
	// Voiceover takes are session-only — clear the temp dir on close.
	if (!voTempDir_.isEmpty())
		QDir(voTempDir_).removeRecursively();
}

qint64 VideoEditorWindow::outputDurationMs() const
{
	if (!valid_)
		return 0;
	if (multiCut())
		return tracks_->totalOutputMs();
	const double sp = speed_ > 0.01 ? speed_ : 1.0;
	return std::max<qint64>(1, qint64((timeline_->end() - timeline_->start()) / sp));
}

qint64 VideoEditorWindow::currentOutputMs() const
{
	if (!playing_)
		return 0;
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
		setEditMode(true); // show the output track
	tracks_->addSegments(segs);
	QMessageBox::information(this, QStringLiteral("Auto-cut"),
				QStringLiteral("Added %1 cuts from scene changes.").arg(segs.size()));
}

void VideoEditorWindow::onSaveProject()
{
	if (!valid_)
		return;
	const QString suggested = QFileInfo(inPath_).absolutePath() + QLatin1Char('/') +
				  QFileInfo(inPath_).completeBaseName() + QStringLiteral("_edit.harpiaproj");
	QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save project"), suggested,
						    QStringLiteral("Harpia project (*.harpiaproj)"));
	if (path.isEmpty())
		return;
	if (!path.endsWith(QStringLiteral(".harpiaproj"), Qt::CaseInsensitive))
		path += QStringLiteral(".harpiaproj");

	const EditorSnapshot s = snapshot();
	const QString projDir = QFileInfo(path).absolutePath();
	const QString assetsRel = QFileInfo(path).completeBaseName() + QStringLiteral("_assets");
	const QString assetsDir = projDir + QLatin1Char('/') + assetsRel;

	QJsonObject root;
	root[QStringLiteral("harpiaProject")] = 1;
	root[QStringLiteral("source")] = QDir::toNativeSeparators(inPath_);
	root[QStringLiteral("sourceName")] = QFileInfo(inPath_).fileName();
	root[QStringLiteral("durationMs")] = double(seeker_->durationMs());
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

	QFile f(path);
	if (!f.open(QIODevice::WriteOnly)) {
		QMessageBox::warning(this, QStringLiteral("Save project"),
				     QStringLiteral("Could not write the project file."));
		return;
	}
	f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	f.close();
	if (!copyOk)
		QMessageBox::warning(
			this, QStringLiteral("Save project"),
			QStringLiteral("Project saved, but some voiceover audio could not be copied."));
	else
		QMessageBox::information(this, QStringLiteral("Save project"),
					QStringLiteral("Project saved."));
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

	const QString projSourceName = root.value(QStringLiteral("sourceName")).toString();
	if (!projSourceName.isEmpty() && projSourceName != QFileInfo(inPath_).fileName()) {
		const auto ret = QMessageBox::question(
			this, QStringLiteral("Open project"),
			QStringLiteral("This project was made for \"%1\", but you're editing \"%2\".\n"
				       "Apply the edits anyway?")
				.arg(projSourceName, QFileInfo(inPath_).fileName()));
		if (ret != QMessageBox::Yes)
			return;
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

	restoreSnapshot(s);
	if (!s.segments.isEmpty() && !multiCut())
		setEditMode(true);
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
	pendingSource_ = multiCut() ? tracks_->scrubSourceId() : activeSourceId_;
	pendingMs_ = ms;
	cursorTimeLabel_->setText(previewTimeText(ms));
	if (!previewTimer_->isActive())
		previewTimer_->start();
}

void VideoEditorWindow::onHoverScrub(qint64 ms)
{
	// Hovering previews the frame under the cursor, but never fights an
	// active playback preview.
	if (!valid_ || playing_)
		return;
	pendingSource_ = multiCut() ? tracks_->scrubSourceId() : activeSourceId_;
	pendingMs_ = ms;
	cursorTimeLabel_->setText(previewTimeText(ms));
	if (!previewTimer_->isActive())
		previewTimer_->start();
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
	return s;
}

void VideoEditorWindow::scheduleSnapshot()
{
	if (restoring_ || !valid_)
		return;
	histTimer_->start(); // (re)start the coalescing timer
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
	if (multiCut()) {
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

void VideoEditorWindow::startPlayback()
{
	if (!valid_)
		return;
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
	if (voTrack_ && !voRecording_)
		voTrack_->clearPlayhead();
}

void VideoEditorWindow::onPlayTick()
{
	if (!playing_ || !valid_)
		return;

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
		canvas_->setFrame(img);
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
	canvas_->setFrame(img);
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

void VideoEditorWindow::onPreviewTick()
{
	if (pendingMs_ >= 0)
		showFrame(pendingSource_, pendingMs_);
	pendingMs_ = -1;
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
	const QImage img = fs->frameAt(ms, 1280, 720);
	if (!img.isNull())
		canvas_->setFrame(img);
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

	const bool cuts = multiCut();
	if (cuts && tracks_->segments().isEmpty()) {
		QMessageBox::information(
			this, QStringLiteral("Multi-Cut"),
			QStringLiteral("Drag on the Source track to create at least one cut first."));
		return;
	}

	// Phase 1: export still consumes a single input. Block exporting a project
	// that mixes more than one source (multi-source export lands next).
	{
		const int primary = sources_.empty() ? -1 : sources_.front().id;
		for (const CutSegment &c : tracks_->segments())
			if (c.sourceId != primary) {
				QMessageBox::information(
					this, QStringLiteral("Export"),
					QStringLiteral("This project mixes more than one source. Exporting "
						       "multiple sources is coming next — for now, export "
						       "projects that use a single video."));
				return;
			}
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
		for (const CutSegment &cs : tracks_->segments())
			o.cuts.push_back({cs.srcStartMs, cs.srcEndMs, cs.speed});
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
