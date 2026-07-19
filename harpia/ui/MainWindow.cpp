#include "MainWindow.hpp"

#include "AudioPanel.hpp"
#include "ClipLibraryWindow.hpp"
#include "ErrorLogsPanel.hpp"
#include "MouseFxOverlay.hpp"
#include "Version.hpp"
#include "PresetEditorDialog.hpp"
#include "RecentListWidget.hpp"
#include "CountdownOverlay.hpp"
#include "RegionTool.hpp"
#include "StatusBadge.hpp"
#include "WebcamPreview.hpp"
#include "core/EncoderFactory.hpp"
#include "platform/CameraAccess.hpp"
#include "platform/ForegroundWatcher.hpp"
#include "core/ObsContext.hpp"
#include "library/ClipLibrary.hpp"
#include "model/PresetStore.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QMessageBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QLayoutItem>

#include <algorithm>
#include <functional>
#include <map>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStorageInfo>
#include <QStyle>
#include <QTime>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

namespace harpia {

namespace {
constexpr int kRecentCount = 12;
constexpr QSize kStripThumb(160, 90);
} // namespace

MainWindow::MainWindow(ObsContext &obs, PresetStore &presets, QString defaultFolder, QWidget *parent)
	: QMainWindow(parent),
	  obs_(obs),
	  presets_(presets),
	  idle_(IdleMonitor::create()),
	  defaultFolder_(std::move(defaultFolder))
{
	setWindowTitle(QStringLiteral("Harpia Recorder  v%1").arg(QString::fromUtf8(appVersion())));

	if (!presets_.presets().empty())
		activePresetId_ = presets_.presets().front().id;

	canvasSize_ = canvasForActivePreset();

	auto *central = new QWidget(this);
	auto *root = new QVBoxLayout(central);
	root->setContentsMargins(18, 14, 18, 14);
	root->setSpacing(12);

	// ---- Top toolbar ----------------------------------------------------
	auto *toolbar = new QHBoxLayout;
	toolbar->setSpacing(8);

	toolbar->addWidget(new QLabel(QStringLiteral("Preset"), central));
	presetCombo_ = new QComboBox(central);
	presetCombo_->setMinimumWidth(160);
	presetCombo_->setContextMenuPolicy(Qt::CustomContextMenu);
	presetCombo_->setToolTip(QStringLiteral("Right-click to edit or delete this preset"));
	toolbar->addWidget(presetCombo_);

	editPresetButton_ = new QPushButton(QStringLiteral("Edit"), central);
	editPresetButton_->setToolTip(QStringLiteral("Edit the selected preset"));
	toolbar->addWidget(editPresetButton_);

	newPresetButton_ = new QPushButton(QStringLiteral("New"), central);
	newPresetButton_->setToolTip(QStringLiteral("Create a new preset"));
	toolbar->addWidget(newPresetButton_);

	toolbar->addStretch(1);

	toolbar->addWidget(new QLabel(QStringLiteral("Capture"), central));
	captureModeCombo_ = new QComboBox(central);
	captureModeCombo_->addItem(QStringLiteral("Entire Monitor"), int(CaptureMode::Monitor));
	captureModeCombo_->addItem(QStringLiteral("Custom Region"), int(CaptureMode::Region));
	toolbar->addWidget(captureModeCombo_);

	webcamSettingsButton_ = new QPushButton(QStringLiteral("Webcam"), central);
	webcamSettingsButton_->setToolTip(QStringLiteral("Configure the webcam for this preset"));
	toolbar->addWidget(webcamSettingsButton_);

	idleToggle_ = new QCheckBox(QStringLiteral("Only record while using the computer"), central);
	toolbar->addWidget(idleToggle_);
	idleSpin_ = new QSpinBox(central);
	idleSpin_->setRange(1, 3600);
	idleSpin_->setSuffix(QStringLiteral(" s"));
	idleSpin_->setValue(10);
	idleSpin_->setMaximumWidth(80);
	toolbar->addWidget(idleSpin_);

	openFolderButton_ = new QPushButton(central);
	openFolderButton_->setIcon(style()->standardIcon(QStyle::SP_DirIcon));
	openFolderButton_->setToolTip(QStringLiteral("Open preset folder"));
	openFolderButton_->setFixedWidth(40);
	toolbar->addWidget(openFolderButton_);

	root->addLayout(toolbar);

	// ---- Recording readiness --------------------------------------------
	warningsBox_ = new QWidget(central);
	warningsLayout_ = new QVBoxLayout(warningsBox_);
	warningsLayout_->setContentsMargins(0, 0, 0, 0);
	warningsLayout_->setSpacing(3);
	warningsBox_->setVisible(false);
	root->addWidget(warningsBox_);

	// ---- Center controls: one compact horizontal row ------------------------
	//   ● Ready   |   [ ⬤ Record ]  [ ⏸ Pause ]   |   00:00:00
	// A slim, premium bar: passive status badge, a clear red Record primary, and
	// the timer — vertically centered, minimal padding, subtle separators.
	root->addStretch(1);

	// Button label font — modest, not oversized.
	QFont btnFont;
	btnFont.setPointSize(btnFont.pointSize() + 2);
	btnFont.setBold(true);

	auto makeSep = [central]() {
		auto *line = new QFrame(central);
		line->setFrameShape(QFrame::VLine);
		line->setStyleSheet(QStringLiteral("color:#33373f;"));
		line->setFixedHeight(28);
		return line;
	};

	auto *controls = new QHBoxLayout;
	controls->setSpacing(14);
	controls->addStretch(1);

	// Passive status badge (animated dot + label) — informational, not a button.
	statusBadge_ = new StatusBadge(central);
	controls->addWidget(statusBadge_, 0, Qt::AlignVCenter);

	controls->addWidget(makeSep(), 0, Qt::AlignVCenter);

	// Single Record/Stop toggle: Record when idle, Stop while recording.
	primaryButton_ = new QPushButton(QStringLiteral("\xE2\x97\x8F  Record"), central);
	primaryButton_->setObjectName(QStringLiteral("primaryButton"));
	primaryButton_->setMinimumSize(130, 46);
	primaryButton_->setFont(btnFont);
	controls->addWidget(primaryButton_, 0, Qt::AlignVCenter);

	// Pause/Resume: only shown while recording.
	pauseButton_ = new QPushButton(QStringLiteral("\xE2\x8F\xB8  Pause"), central);
	pauseButton_->setObjectName(QStringLiteral("pauseButton"));
	pauseButton_->setMinimumSize(104, 46);
	pauseButton_->setFont(btnFont);
	pauseButton_->setVisible(false);
	controls->addWidget(pauseButton_, 0, Qt::AlignVCenter);

	controls->addWidget(makeSep(), 0, Qt::AlignVCenter);

	// Elapsed time — monospaced, readable, but not oversized.
	timerLabel_ = new QLabel(QStringLiteral("00:00:00"), central);
	timerLabel_->setObjectName(QStringLiteral("timerLabel"));
	timerLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
	QFont timerFont(QStringLiteral("monospace"));
	timerFont.setStyleHint(QFont::Monospace);
	timerFont.setPointSize(btnFont.pointSize() + 6);
	timerLabel_->setFont(timerFont);
	controls->addWidget(timerLabel_, 0, Qt::AlignVCenter);

	// Inline webcam controls: live preview + device picker, shown only when the
	// active preset records a webcam.
	webcamBox_ = new QWidget(central);
	auto *wcLayout = new QHBoxLayout(webcamBox_);
	wcLayout->setContentsMargins(0, 0, 0, 0);
	wcLayout->setSpacing(8);
	webcamPreview_ = new WebcamPreview(webcamBox_);
	webcamPreview_->setFixedSize(100, 56);
	wcLayout->addWidget(webcamPreview_);
	auto *wcSide = new QVBoxLayout;
	wcSide->setSpacing(4);
	wcSide->addStretch(1);
	webcamCombo_ = new QComboBox(webcamBox_);
	webcamCombo_->setMinimumWidth(160);
	wcSide->addWidget(webcamCombo_);
	webcamWarn_ = new QLabel(QStringLiteral("Webcam not found"), webcamBox_);
	webcamWarn_->setStyleSheet(QStringLiteral("color:#e5484d;"));
	webcamWarn_->setVisible(false);
	wcSide->addWidget(webcamWarn_);
	wcSide->addStretch(1);
	wcLayout->addLayout(wcSide);
	webcamBox_->setVisible(false);
	controls->addWidget(webcamBox_);

	controls->addStretch(1);
	root->addLayout(controls);

	root->addStretch(1);

	// ---- Audio (live levels) -------------------------------------------
	auto *audioGroup = new QGroupBox(QStringLiteral("Audio"), central);
	auto *audioGroupLayout = new QVBoxLayout(audioGroup);
	audioGroupLayout->setContentsMargins(10, 6, 10, 6);
	audioPanel_ = new AudioPanel(audio_, audioGroup);
	audioGroupLayout->addWidget(audioPanel_);
	root->addWidget(audioGroup);

	// ---- Recent recordings strip ---------------------------------------
	auto *stripHeader = new QHBoxLayout;
	stripHeader->addWidget(new QLabel(QStringLiteral("Recent recordings"), central));
	stripHeader->addStretch(1);
	errorLogsButton_ = new QPushButton(QStringLiteral("Error Logs"), central);
	stripHeader->addWidget(errorLogsButton_);
	libraryButton_ = new QPushButton(QStringLiteral("Open Clip Library…"), central);
	stripHeader->addWidget(libraryButton_);
	root->addLayout(stripHeader);

	recentStrip_ = new RecentListWidget(central);
	recentStrip_->setViewMode(QListView::IconMode);
	recentStrip_->setFlow(QListView::LeftToRight);
	recentStrip_->setWrapping(false);
	recentStrip_->setMovement(QListView::Static);
	recentStrip_->setIconSize(kStripThumb);
	recentStrip_->setGridSize(kStripThumb + QSize(24, 44));
	recentStrip_->setFixedHeight(kStripThumb.height() + 60);
	recentStrip_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	recentStrip_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	recentStrip_->setContextMenuPolicy(Qt::CustomContextMenu);
	recentStrip_->setSpacing(6);
	root->addWidget(recentStrip_);

	setCentralWidget(central);

	// Wide, PowerRec-like proportions; a bit taller to fit the audio meters.
	resize(940, 470);

	// ---- Wiring ---------------------------------------------------------
	connect(primaryButton_, &QPushButton::clicked, this, &MainWindow::onPrimaryButton);
	connect(pauseButton_, &QPushButton::clicked, this, &MainWindow::onPauseButton);
	connect(editPresetButton_, &QPushButton::clicked, this, [this]() { editActivePreset(); });
	connect(newPresetButton_, &QPushButton::clicked, this, &MainWindow::onNewPreset);
	connect(webcamSettingsButton_, &QPushButton::clicked, this,
		[this]() { editActivePreset(QStringLiteral("Webcam")); });
	connect(webcamCombo_, &QComboBox::activated, this, &MainWindow::onWebcamDeviceChanged);
	connect(openFolderButton_, &QPushButton::clicked, this, &MainWindow::onOpenPresetFolder);
	connect(libraryButton_, &QPushButton::clicked, this, &MainWindow::onOpenClipLibrary);
	connect(errorLogsButton_, &QPushButton::clicked, this, &MainWindow::onOpenErrorLogs);
	connect(captureModeCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onCaptureModeChanged);
	connect(idleToggle_, &QCheckBox::toggled, this, &MainWindow::onIdleSettingChanged);
	connect(idleSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onIdleSettingChanged);
	connect(presetCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onPresetChanged);
	connect(presetCombo_, &QComboBox::customContextMenuRequested, this, &MainWindow::showPresetMenu);
	connect(recentStrip_, &QListWidget::itemClicked, this, [](QListWidgetItem *item) {
		const QString path = item->data(kClipPathRole).toString();
		if (!path.isEmpty())
			QDesktopServices::openUrl(QUrl::fromLocalFile(path));
	});
	connect(recentStrip_, &QListWidget::customContextMenuRequested, this,
		&MainWindow::showStripContextMenu);
	connect(&thumbnails_, &ThumbnailCache::ready, this, &MainWindow::onThumbnailReady);
	connect(audioPanel_, &AudioPanel::changed, this, &MainWindow::onAudioChanged);

	recorder_.onFinished = [this](const std::string &) {
		QMetaObject::invokeMethod(this, "refreshRecentList", Qt::QueuedConnection);
	};

	mouseFx_ = std::make_unique<MouseFxOverlay>();

	countdownOverlay_ = std::make_unique<CountdownOverlay>();
	connect(countdownOverlay_.get(), &CountdownOverlay::tick, this, [this](int remaining) {
		countdownRemaining_ = remaining;
		updateButtons();
	});
	connect(countdownOverlay_.get(), &CountdownOverlay::finished, this, [this]() {
		countingDown_ = false;
		beginStart();
	});
	connect(countdownOverlay_.get(), &CountdownOverlay::cancelled, this, [this]() {
		countingDown_ = false;
		updateButtons();
	});

	regionTool_ = std::make_unique<RegionTool>();
	connect(regionTool_.get(), &RegionTool::regionChanged, this, &MainWindow::onRegionChanged);
	connect(regionTool_.get(), &RegionTool::cancelled, this, [this]() {
		// Esc: revert to full-monitor capture.
		captureModeCombo_->setCurrentIndex(int(CaptureMode::Monitor));
	});

	applyDarkTheme();

	// Bring up the capture source now so the first record is instant.
	obs_.resetVideo(canvasSize_.width(), canvasSize_.height(), activePreset().fps);
	capture_.startCapture(activePreset().monitorIndex, activePreset().showMouseCursor);

	stateTimer_ = new QTimer(this);
	stateTimer_->setInterval(250);
	connect(stateTimer_, &QTimer::timeout, this, &MainWindow::tickState);
	stateTimer_->start();

	idleTimer_ = new QTimer(this);
	idleTimer_->setInterval(1000);
	connect(idleTimer_, &QTimer::timeout, this, &MainWindow::tickIdle);
	idleTimer_->start();

	// Live audio meters refresh often for a responsive VU bar.
	meterTimer_ = new QTimer(this);
	meterTimer_->setInterval(80);
	connect(meterTimer_, &QTimer::timeout, audioPanel_, &AudioPanel::updateMeters);
	meterTimer_->start();

	// Re-validate readiness periodically to catch hardware changes (a monitor
	// disconnected, a webcam/mic unplugged) without a restart.
	readinessTimer_ = new QTimer(this);
	readinessTimer_->setInterval(1500);
	connect(readinessTimer_, &QTimer::timeout, this, &MainWindow::refreshReadiness);
	readinessTimer_->start();

	reloadPresetCombo();
	syncIdleControls();
	audioPanel_->load(activePreset().recordDesktopAudio, activePreset().micDeviceIds);
	refreshRecentList();
	refreshReadiness();
	refreshWebcamRow();
	updateButtons();
}

MainWindow::~MainWindow()
{
	webcam_.stop();
	if (recorder_.isRecording())
		recorder_.stop();
}

void MainWindow::applyDarkTheme()
{
	// Focused accent styling; the app-wide dark palette is set in main().
	setStyleSheet(QStringLiteral(R"(
		QWidget { color: #e6e6e6; }
		QLabel#timerLabel { color: #f0f0f0; }
		QPushButton { background: #2b2d31; border: 1px solid #3a3d42; border-radius: 6px; padding: 6px 12px; }
		QPushButton:hover { background: #34373c; }
		QPushButton:disabled { color: #6b6f76; }
		QPushButton#primaryButton { background: #e5484d; border: none; color: white; border-radius: 10px; }
		QPushButton#primaryButton:hover { background: #f05a5f; }
		QPushButton#stopButton { border-radius: 10px; }
		QComboBox, QSpinBox { background: #2b2d31; border: 1px solid #3a3d42; border-radius: 6px; padding: 4px 8px; }
		QListWidget { background: #202225; border: 1px solid #303338; border-radius: 8px; }
		QListWidget::item:selected { background: #3a3d42; }
	)"));
}

const Preset &MainWindow::activePreset() const
{
	if (const Preset *p = presets_.find(activePresetId_))
		return *p;
	return presets_.presets().front();
}

QScreen *MainWindow::screenForActivePreset() const
{
	const QList<QScreen *> screens = QGuiApplication::screens();
	if (screens.isEmpty())
		return nullptr;
	// Best-effort: map the preset's monitor index onto Qt's screen order.
	int idx = activePreset().monitorIndex;
	if (idx < 0 || idx >= screens.size())
		idx = 0;
	return screens.at(idx);
}

QSize MainWindow::canvasForActivePreset() const
{
	QScreen *screen = screenForActivePreset();
	if (!screen)
		return QSize(1920, 1080);
	const QSize logical = screen->size();
	const qreal dpr = screen->devicePixelRatio();
	return QSize(int(logical.width() * dpr), int(logical.height() * dpr));
}

QStringList MainWindow::presetFolders() const
{
	QStringList folders;
	for (const Preset &p : presets_.presets()) {
		const QString f = QString::fromStdString(p.outputFolder);
		if (!f.isEmpty() && !folders.contains(f))
			folders << f;
	}
	return folders;
}

ClipLibrary::PresetByFolder MainWindow::presetFolderMap() const
{
	ClipLibrary::PresetByFolder map;
	for (const Preset &p : presets_.presets()) {
		if (!p.outputFolder.empty())
			map.insert(QString::fromStdString(p.outputFolder), QString::fromStdString(p.name));
	}
	return map;
}

QString MainWindow::buildOutputPath(const Preset &preset) const
{
	const std::string name = nameTemplate_.expand(preset.filenameTemplate);
	QDir dir(QString::fromStdString(preset.outputFolder));
	dir.mkpath(QStringLiteral("."));
	return dir.filePath(QStringLiteral("%1.%2").arg(QString::fromStdString(name),
						       QString::fromStdString(preset.extension())));
}

void MainWindow::startRecording()
{
	// Readiness gate: never start while blocking warnings exist.
	refreshReadiness();
	if (recordingBlocked_) {
		starting_ = false;
		updateButtons();
		return;
	}

	const Preset &preset = activePreset();

	// Always record at the native resolution of what's being captured: the exact
	// region size in Region mode, otherwise the full display resolution. No
	// scaling / custom sizes.
	canvasSize_ = canvasForActivePreset();
	uint32_t baseW, baseH;
	if (captureMode_ == CaptureMode::Region && currentRegion_.enabled && currentRegion_.width >= 16 &&
	    currentRegion_.height >= 16) {
		baseW = (uint32_t)currentRegion_.width;
		baseH = (uint32_t)currentRegion_.height;
	} else {
		baseW = canvasSize_.width();
		baseH = canvasSize_.height();
	}

	// GIF is meant for short clips — cap the frame rate so files stay small.
	int fps = preset.fps;
	if (preset.format == RecordingFormat::GIF)
		fps = qMin(fps, 15);

	// Output size == base size (native, no downscale).
	obs_.resetVideo(baseW, baseH, fps, baseW, baseH);

	// Ensure we're capturing this preset's display, then re-apply any region.
	capture_.startCapture(preset.monitorIndex, preset.showMouseCursor);
	capture_.setRegion(currentRegion_);

	// Build the context tokens the clock can't supply, then expand once so the
	// screen and webcam files share a base name.
	std::map<std::string, std::string> vars;
	vars["Preset"] = preset.name;
	vars["Resolution"] = std::to_string(baseW) + "x" + std::to_string(baseH);
	vars["FPS"] = std::to_string(preset.fps) + "fps";
	vars["Codec"] = QString::fromUtf8(codecToString(preset.codec)).toUpper().toStdString();
	vars["Counter"] = QStringLiteral("%1").arg(preset.recordingCounter, 4, 10, QLatin1Char('0')).toStdString();

	const QString baseName = QString::fromStdString(nameTemplate_.expand(preset.filenameTemplate, vars));

	// Advance the persistent counter if the template consumed it.
	if (preset.filenameTemplate.find("{Counter}") != std::string::npos) {
		if (Preset *cur = const_cast<Preset *>(presets_.find(activePresetId_))) {
			cur->recordingCounter += 1;
			presets_.upsert(*cur);
		}
	}

	QDir dir(QString::fromStdString(preset.outputFolder));
	dir.mkpath(QStringLiteral("."));
	const QString screenPath =
		dir.filePath(baseName + QLatin1Char('.') + QString::fromStdString(preset.extension()));
	if (!recorder_.start(preset, screenPath.toStdString())) {
		timerLabel_->setText(QStringLiteral("error"));
		starting_ = false;
		updateButtons();
		return;
	}

	// Webcam as a separate synchronized file (never composited).
	if (preset.webcamEnabled) {
		QString wcFolder = (preset.webcamUseCustomFolder && !preset.webcamFolder.empty())
					   ? QString::fromStdString(preset.webcamFolder)
					   : QString::fromStdString(preset.outputFolder);
		QDir wdir(wcFolder);
		wdir.mkpath(QStringLiteral("."));
		const QString webcamPath = wdir.filePath(baseName + QStringLiteral("_webcam.mp4"));
		// Reuse the live preview's already-open camera so the device isn't
		// opened twice (DirectShow cameras are usually exclusive).
		obs_source_t *shared = webcamPreview_ ? webcamPreview_->source() : nullptr;
		if (!webcam_.start(preset.webcamDeviceId, preset.webcamWidth, preset.webcamHeight, preset.webcamFps,
				   webcamPath.toStdString(), shared)) {
			// No camera / failed to start — keep the screen recording going.
			QMessageBox::warning(
				this, QStringLiteral("Webcam"),
				QStringLiteral("Webcam recording could not start (no camera available?).\n"
					       "The screen recording continues without it."));
		}
	}

	// Focus auto-pause: remember the app that's in the foreground now as the
	// target, and prepare a sidecar file for Auto Paused/Resumed markers.
	focusPaused_ = false;
	targetPid_ = 0;
	markersPath_.clear();
	if (preset.pauseOnFocusLoss) {
		targetPid_ = ForegroundWatcher::foregroundProcessId();
		markersPath_ = screenPath + QStringLiteral(".markers.txt");
	}

	recStartMs_ = QDateTime::currentMSecsSinceEpoch();
	pausedAccumMs_ = 0;
	pauseStartMs_ = 0;
	wasPaused_ = false;
	autoPaused_ = false;
	updateButtons();
	updateRegionToolVisibility(); // dim the region tool into recording mode

	// Mouse effects overlay (highlight + click ripples) — captured by the screen.
	if (preset.showMouseArea || preset.recordMouseClicks) {
		MouseFxOverlay::Config cfg;
		cfg.showArea = preset.showMouseArea;
		cfg.areaColor = QColor(QString::fromStdString(preset.mouseHighlightColor));
		cfg.areaSize = preset.mouseHighlightSize;
		cfg.showClicks = preset.recordMouseClicks;
		cfg.leftColor = QColor(QString::fromStdString(preset.leftClickColor));
		cfg.rightColor = QColor(QString::fromStdString(preset.rightClickColor));
		mouseFx_->configure(cfg);
		mouseFx_->setScreen(screenForActivePreset());
		mouseFx_->start();
	}
}

void MainWindow::onPrimaryButton()
{
	// Ignore clicks while a transition is in flight (the button is disabled then
	// anyway, but guard against races).
	if (starting_ || stopping_ || countingDown_)
		return;

	if (recorder_.isRecording())
		beginStop();
	else
		beginRecordFlow();
}

void MainWindow::beginRecordFlow()
{
	// Gate on readiness before showing any countdown.
	refreshReadiness();
	if (recordingBlocked_)
		return;

	const int cd = activePreset().countdownSeconds;
	if (cd > 0 && countdownOverlay_) {
		countingDown_ = true;
		countdownRemaining_ = cd;
		countdownOverlay_->start(cd, screenForActivePreset());
		updateButtons();
		return;
	}
	beginStart();
}

void MainWindow::beginStart()
{
	starting_ = true;
	updateButtons();
	startRecording(); // clears starting_ itself on failure; success clears in tickState
}

void MainWindow::beginStop()
{
	stopping_ = true;
	updateButtons();
	recorder_.stop(); // async; tickState clears stopping_ once finalized
}

void MainWindow::onPauseButton()
{
	if (!recorder_.isRecording())
		return;
	recorder_.togglePause();
	autoPaused_ = false;  // manual action overrides the idle state machine
	focusPaused_ = false; // and the focus state machine
	updateButtons();
}

void MainWindow::onNewPreset()
{
	Preset base = Preset::makeDefault(defaultFolder_.toStdString());
	base.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
	base.name = "New preset";

	PresetEditorDialog dlg(base, this);
	if (dlg.exec() == QDialog::Accepted) {
		Preset created = dlg.result();
		presets_.upsert(created);
		activePresetId_ = created.id;
		reloadPresetCombo();
		syncIdleControls();
		refreshRecentList();
	}
}

void MainWindow::showPresetMenu(const QPoint &pos)
{
	if (recorder_.isRecording())
		return;

	QMenu menu(this);
	QAction *editAct = menu.addAction(QStringLiteral("Edit preset…"));
	QAction *dupAct = menu.addAction(QStringLiteral("Duplicate preset"));
	menu.addSeparator();
	QAction *delAct = menu.addAction(QStringLiteral("Delete preset"));
	delAct->setEnabled(presets_.presets().size() > 1);

	QAction *chosen = menu.exec(presetCombo_->mapToGlobal(pos));
	if (!chosen)
		return;

	const Preset *cur = presets_.find(activePresetId_);
	if (!cur)
		return;

	if (chosen == editAct) {
		editActivePreset();
	} else if (chosen == dupAct) {
		Preset copy = *cur;
		copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
		copy.name = cur->name + " (copy)";
		presets_.upsert(copy);
		activePresetId_ = copy.id;
		reloadPresetCombo();
		syncIdleControls();
	} else if (chosen == delAct) {
		if (presets_.presets().size() <= 1)
			return;
		if (QMessageBox::question(this, QStringLiteral("Delete preset"),
					  QStringLiteral("Delete preset \"%1\"?")
						  .arg(QString::fromStdString(cur->name))) != QMessageBox::Yes)
			return;
		presets_.remove(activePresetId_);
		activePresetId_ = presets_.presets().front().id;
		reloadPresetCombo();
		syncIdleControls();
		refreshRecentList();
	}
}

void MainWindow::editActivePreset(const QString &initialPage)
{
	const Preset *cur = presets_.find(activePresetId_);
	if (!cur)
		return;
	PresetEditorDialog dlg(*cur, this);
	if (!initialPage.isEmpty())
		dlg.showPage(initialPage);
	if (dlg.exec() == QDialog::Accepted) {
		presets_.upsert(dlg.result());
		reloadPresetCombo();
		syncIdleControls();
		audioPanel_->load(activePreset().recordDesktopAudio, activePreset().micDeviceIds);
		refreshRecentList();
		refreshReadiness();
		refreshWebcamRow();
	}
}

void MainWindow::refreshWebcamRow()
{
	const Preset &p = activePreset();

	if (!p.webcamEnabled) {
		webcamBox_->setVisible(false);
		if (webcamPreview_)
			webcamPreview_->clearDevice();
		return;
	}

	// Populate the device list from currently available cameras.
	const std::vector<AudioDevice> cams = WebcamRecorder::cameras();
	{
		QSignalBlocker block(webcamCombo_);
		webcamCombo_->clear();
		for (const AudioDevice &c : cams)
			webcamCombo_->addItem(QString::fromStdString(c.name), QString::fromStdString(c.id));
	}

	const QString wantId = QString::fromStdString(p.webcamDeviceId);
	int idx = wantId.isEmpty() ? 0 : webcamCombo_->findData(wantId);
	const bool wantedMissing = !wantId.isEmpty() && idx < 0;
	if (idx < 0)
		idx = 0; // fall back to the first available camera

	webcamBox_->setVisible(true);
	if (cams.empty()) {
		// Explain *why* there are no cameras: plugin missing / privacy / none.
		QString warn;
		QString tip;
		if (!WebcamRecorder::supported()) {
			warn = QStringLiteral("Webcam capture unavailable in this build");
			tip = QStringLiteral("The camera plugin (win-dshow) isn't loaded. Install the "
					     "Visual Studio 'C++ ATL' component and rebuild.");
		} else if (cameraAccessStatus() == CameraAccess::DeniedByPrivacy) {
			warn = QStringLiteral("Camera access blocked by Windows");
			tip = QStringLiteral("Enable Windows Privacy & Security > Camera > "
					     "'Let desktop apps access your camera'.");
		} else {
			warn = QStringLiteral("Webcam not found");
			tip = QStringLiteral("Connect a camera that isn't already in use by another app. "
					     "If it still doesn't appear, check Windows camera privacy settings.");
		}
		webcamWarn_->setText(warn);
		webcamWarn_->setToolTip(tip);
		webcamWarn_->setVisible(true);
		webcamPreview_->clearDevice();
		return;
	}

	{
		QSignalBlocker block(webcamCombo_);
		webcamCombo_->setCurrentIndex(idx);
	}
	webcamWarn_->setText(QStringLiteral("Saved camera not found — using %1")
				     .arg(webcamCombo_->currentText()));
	webcamWarn_->setVisible(wantedMissing);

	const std::string dev = webcamCombo_->currentData().toString().toStdString();
	webcamPreview_->setDevice(dev, p.webcamWidth, p.webcamHeight, p.webcamFps);
}

void MainWindow::onWebcamDeviceChanged()
{
	const QString id = webcamCombo_->currentData().toString();
	if (id.isEmpty())
		return;

	// Switch the live preview immediately, and remember the choice on the preset
	// so it persists without opening the editor.
	const Preset &p = activePreset();
	webcamPreview_->setDevice(id.toStdString(), p.webcamWidth, p.webcamHeight, p.webcamFps);
	webcamWarn_->setVisible(false);

	if (Preset *cur = const_cast<Preset *>(presets_.find(activePresetId_))) {
		cur->webcamDeviceId = id.toStdString();
		presets_.upsert(*cur);
	}
}

void MainWindow::refreshReadiness()
{
	struct Warning {
		QString message;
		std::function<void()> fix;
		QString fixLabel;
		bool blocking = true; // false = caution only, recording still allowed
	};
	std::vector<Warning> warnings;

	const Preset &p = activePreset();

	// --- Output folder ---
	const QString folder = QString::fromStdString(p.outputFolder);
	if (folder.isEmpty()) {
		warnings.push_back({QStringLiteral("No output folder is set."), [this]() { editActivePreset(); },
				    QStringLiteral("Set folder")});
	} else {
		QDir dir(folder);
		if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
			warnings.push_back({QStringLiteral("Output folder does not exist and can't be created."),
					    [this]() { editActivePreset(); }, QStringLiteral("Fix folder")});
		} else if (dir.exists() && !QFileInfo(folder).isWritable()) {
			warnings.push_back({QStringLiteral("Output folder is not writable."),
					    [this]() { editActivePreset(); }, QStringLiteral("Fix folder")});
		} else {
			const QStorageInfo storage(folder);
			if (storage.isValid() && storage.bytesAvailable() > 0 &&
			    storage.bytesAvailable() < 500LL * 1024 * 1024) {
				warnings.push_back({QStringLiteral("Low disk space (< 500 MB) on the output drive."),
						    nullptr, QString(), /*blocking=*/false});
			}
		}
	}

	// --- Display / region ---
	const int monitorCount = (int)CaptureManager::enumerateMonitors().size();
	if (monitorCount > 0 && p.monitorIndex >= monitorCount) {
		warnings.push_back({QStringLiteral("The selected monitor is no longer available."),
				    [this]() { editActivePreset(); }, QStringLiteral("Choose display")});
	}
	if (captureMode_ == CaptureMode::Region) {
		if (!currentRegion_.enabled) {
			warnings.push_back({QStringLiteral("No capture region is selected."),
					    [this]() { captureModeCombo_->setCurrentIndex(int(CaptureMode::Region)); },
					    QStringLiteral("Select region")});
		} else if (currentRegion_.width < 16 || currentRegion_.height < 16) {
			warnings.push_back({QStringLiteral("The capture region is too small."), nullptr, QString()});
		}
	}

	// --- Microphones ---
	if (!p.micDeviceIds.empty()) {
		std::vector<std::string> available;
		for (const AudioDevice &d : AudioManager::inputDevices())
			available.push_back(d.id);
		for (const std::string &id : p.micDeviceIds) {
			const bool present = id == "default" ||
					     std::find(available.begin(), available.end(), id) != available.end();
			if (!present) {
				warnings.push_back(
					{QStringLiteral("A selected microphone is no longer available."), nullptr,
					 QString()});
				break;
			}
		}
	}

	// --- Webcam --- (non-blocking: the screen recording proceeds without it)
	if (p.webcamEnabled && WebcamRecorder::cameras().empty()) {
		warnings.push_back({QStringLiteral("Webcam is enabled but no camera is available — it will be "
						   "skipped for this recording."),
				    [this]() { editActivePreset(QStringLiteral("Webcam")); },
				    QStringLiteral("Fix webcam"),
				    /*blocking=*/false});
	}

	// --- Codec / video settings ---
	if (p.format != RecordingFormat::GIF && EncoderFactory::videoEncoderId(p).empty()) {
		warnings.push_back({QStringLiteral("The selected codec has no available encoder."),
				    [this]() { editActivePreset(); }, QStringLiteral("Change codec")});
	}
	if (p.fps <= 0) {
		warnings.push_back({QStringLiteral("Invalid frame rate."), [this]() { editActivePreset(); },
				    QStringLiteral("Fix video")});
	}

	// --- Rebuild the warnings UI ---
	QLayoutItem *item;
	while ((item = warningsLayout_->takeAt(0)) != nullptr) {
		if (item->widget())
			item->widget()->deleteLater();
		delete item;
	}

	for (const Warning &w : warnings) {
		auto *row = new QWidget(warningsBox_);
		auto *rl = new QHBoxLayout(row);
		rl->setContentsMargins(0, 0, 0, 0);
		auto *icon = new QLabel(QStringLiteral("⚠"), row);
		icon->setStyleSheet(QStringLiteral("color:#d29922;"));
		auto *msg = new QLabel(w.message, row);
		msg->setWordWrap(true);
		rl->addWidget(icon);
		rl->addWidget(msg, 1);
		if (w.fix) {
			auto *fix = new QPushButton(w.fixLabel, row);
			auto action = w.fix;
			connect(fix, &QPushButton::clicked, this, [action]() { action(); });
			rl->addWidget(fix);
		}
		warningsLayout_->addWidget(row);
	}

	bool anyBlocking = false;
	for (const Warning &w : warnings) {
		if (w.blocking)
			anyBlocking = true;
	}
	recordingBlocked_ = anyBlocking;

	// Headline for the status chip: prefer the first blocking issue, else the
	// first caution.
	firstIssue_.clear();
	for (const Warning &w : warnings) {
		if (w.blocking) {
			firstIssue_ = w.message;
			break;
		}
	}
	if (firstIssue_.isEmpty() && !warnings.empty())
		firstIssue_ = warnings.front().message;

	warningsBox_->setVisible(!warnings.empty());
	updateButtons();
}

void MainWindow::onOpenPresetFolder()
{
	const QString dir = QString::fromStdString(presets_.configDir());
	if (!dir.isEmpty())
		QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::onOpenClipLibrary()
{
	if (!clipWindow_)
		clipWindow_ = std::make_unique<ClipLibraryWindow>(presets_);
	clipWindow_->show();
	clipWindow_->raise();
	clipWindow_->activateWindow();
	clipWindow_->refresh();
}

void MainWindow::onOpenErrorLogs()
{
	if (!errorLogsPanel_)
		errorLogsPanel_ = std::make_unique<ErrorLogsPanel>();
	errorLogsPanel_->show();
	errorLogsPanel_->raise();
	errorLogsPanel_->activateWindow();
	errorLogsPanel_->refresh();
}

void MainWindow::onCaptureModeChanged()
{
	captureMode_ = CaptureMode(captureModeCombo_->currentData().toInt());
	QScreen *screen = screenForActivePreset();

	if (captureMode_ == CaptureMode::Region) {
		// Seed a default region (centered, ~2/3 of the screen) the first time.
		if (!currentRegion_.enabled || currentRegion_.width <= 0) {
			const QSize canvas = canvasForActivePreset();
			CaptureRegion r;
			r.enabled = true;
			r.width = canvas.width() * 2 / 3;
			r.height = canvas.height() * 2 / 3;
			r.x = (canvas.width() - r.width) / 2;
			r.y = (canvas.height() - r.height) / 2;
			currentRegion_ = r;
		}
		regionTool_->setScreen(screen);
		regionTool_->setRegionDevicePx(
			QRect(currentRegion_.x, currentRegion_.y, currentRegion_.width, currentRegion_.height));
		capture_.setRegion(currentRegion_);
	} else {
		currentRegion_ = CaptureRegion{};
		capture_.setRegion(currentRegion_);
	}
	updateRegionToolVisibility();
	updateButtons();
	refreshReadiness();
}

void MainWindow::onRegionChanged(const CaptureRegion &region)
{
	currentRegion_ = region;
	// Live update — crop_filter applies immediately, even while recording.
	capture_.setRegion(region);
	refreshReadiness();
}

void MainWindow::updateRegionToolVisibility()
{
	if (!regionTool_)
		return;
	if (captureMode_ != CaptureMode::Region) {
		regionTool_->hide();
		return;
	}
	const bool recording = recorder_.isRecording();
	regionTool_->setRecordingMode(recording);
	// Visible while recording (subtle indicator) or while the app (main window or
	// the tool itself) is focused; hidden when unfocused and not recording.
	const bool focused = isActiveWindow() || regionTool_->isActiveWindow();
	if (recording || focused)
		regionTool_->show();
	else
		regionTool_->hide();
}

void MainWindow::changeEvent(QEvent *event)
{
	QMainWindow::changeEvent(event);
	if (event->type() == QEvent::ActivationChange)
		updateRegionToolVisibility();
}

void MainWindow::onIdleSettingChanged()
{
	idleSpin_->setEnabled(idleToggle_->isChecked());

	// Persist the idle setting onto the active preset so it survives restarts
	// and drives tickIdle().
	const Preset *cur = presets_.find(activePresetId_);
	if (!cur)
		return;
	Preset updated = *cur;
	updated.idleTimeoutSeconds = idleToggle_->isChecked() ? idleSpin_->value() : 0;
	if (updated.idleTimeoutSeconds != cur->idleTimeoutSeconds)
		presets_.upsert(updated);
}

void MainWindow::onAudioChanged()
{
	const Preset *cur = presets_.find(activePresetId_);
	if (!cur)
		return;
	Preset updated = *cur;
	updated.recordDesktopAudio = audioPanel_->desktopOn();
	updated.micDeviceIds = audioPanel_->enabledMicIds();
	presets_.upsert(updated);
	refreshReadiness();
}

void MainWindow::onPresetChanged()
{
	const QString id = presetCombo_->currentData().toString();
	if (!id.isEmpty())
		activePresetId_ = id.toStdString();
	syncIdleControls();
	audioPanel_->load(activePreset().recordDesktopAudio, activePreset().micDeviceIds);

	// Switch the live capture to the new preset's display (unless recording).
	// The previous region was chosen on a possibly-different monitor, so reset
	// to full-monitor capture to avoid an out-of-bounds crop.
	if (!recorder_.isRecording()) {
		canvasSize_ = canvasForActivePreset();
		currentRegion_ = CaptureRegion{};
		captureModeCombo_->setCurrentIndex(int(CaptureMode::Monitor));
		capture_.startCapture(activePreset().monitorIndex, activePreset().showMouseCursor);
		capture_.setRegion(currentRegion_);
		updateRegionToolVisibility();
	}
	refreshReadiness();
	refreshWebcamRow();
}

void MainWindow::syncIdleControls()
{
	const Preset &p = activePreset();
	QSignalBlocker b1(idleToggle_);
	QSignalBlocker b2(idleSpin_);
	const bool on = p.idleTimeoutSeconds > 0;
	idleToggle_->setChecked(on);
	idleSpin_->setValue(on ? p.idleTimeoutSeconds : 10);
	idleSpin_->setEnabled(on);
}

void MainWindow::reloadPresetCombo()
{
	QSignalBlocker block(presetCombo_);
	presetCombo_->clear();
	int activeIndex = 0;
	int i = 0;
	for (const Preset &p : presets_.presets()) {
		presetCombo_->addItem(QString::fromStdString(p.name), QString::fromStdString(p.id));
		if (p.id == activePresetId_)
			activeIndex = i;
		++i;
	}
	if (presetCombo_->count() > 0) {
		presetCombo_->setCurrentIndex(activeIndex);
		activePresetId_ = presetCombo_->currentData().toString().toStdString();
	}
}

void MainWindow::refreshRecentList()
{
	recentStrip_->clear();
	itemByPath_.clear();
	const QVector<ClipInfo> clips = ClipLibrary::recent(presetFolders(), kRecentCount, presetFolderMap());
	for (const ClipInfo &clip : clips) {
		auto *item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(clip.relativeAge(), clip.humanSize()));
		item->setData(kClipPathRole, clip.filePath);
		item->setToolTip(clip.fileName + QStringLiteral("\n") + clip.filePath);
		item->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);

		const QImage thumb = thumbnails_.cached(clip.filePath, kStripThumb);
		if (!thumb.isNull())
			item->setIcon(QIcon(QPixmap::fromImage(thumb)));
		else
			thumbnails_.ensure(clip.filePath, kStripThumb);

		itemByPath_.insert(clip.filePath, item);
		recentStrip_->addItem(item);
	}
}

void MainWindow::onThumbnailReady(const QString &path)
{
	QListWidgetItem *item = itemByPath_.value(path, nullptr);
	if (!item)
		return;
	const QImage thumb = thumbnails_.cached(path, kStripThumb);
	if (!thumb.isNull())
		item->setIcon(QIcon(QPixmap::fromImage(thumb)));
}

void MainWindow::showStripContextMenu(const QPoint &pos)
{
	QListWidgetItem *item = recentStrip_->itemAt(pos);
	if (!item)
		return;
	const QString path = item->data(kClipPathRole).toString();
	if (path.isEmpty())
		return;

	QMenu menu(this);
	QAction *openAct = menu.addAction(QStringLiteral("Open"));
	QAction *folderAct = menu.addAction(QStringLiteral("Open Folder"));
	menu.addSeparator();
	QAction *copyAct = menu.addAction(QStringLiteral("Copy"));
	QAction *copyPathAct = menu.addAction(QStringLiteral("Copy Path"));
	menu.addSeparator();
	QAction *renameAct = menu.addAction(QStringLiteral("Rename…"));
	QAction *deleteAct = menu.addAction(QStringLiteral("Delete"));

	QAction *chosen = menu.exec(recentStrip_->viewport()->mapToGlobal(pos));
	if (!chosen)
		return;

	if (chosen == openAct) {
		QDesktopServices::openUrl(QUrl::fromLocalFile(path));
	} else if (chosen == folderAct) {
		// On Windows, reveal the file selected in Explorer; elsewhere open the
		// containing folder.
#ifdef Q_OS_WIN
		QProcess::startDetached(
			QStringLiteral("explorer.exe"),
			{QStringLiteral("/select,") + QDir::toNativeSeparators(path)});
#else
		QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
	} else if (chosen == copyAct) {
		// Put the file itself on the clipboard so it pastes into Explorer/Finder.
		auto *mime = new QMimeData();
		mime->setUrls({QUrl::fromLocalFile(path)});
		QApplication::clipboard()->setMimeData(mime);
	} else if (chosen == copyPathAct) {
		QApplication::clipboard()->setText(path);
	} else if (chosen == renameAct) {
		const QFileInfo fi(path);
		bool ok = false;
		const QString newBase = QInputDialog::getText(
			this, QStringLiteral("Rename recording"), QStringLiteral("New name:"),
			QLineEdit::Normal, fi.completeBaseName(), &ok);
		if (!ok || newBase.trimmed().isEmpty() || newBase == fi.completeBaseName())
			return;
		QString suffix = fi.suffix();
		QString target = fi.absolutePath() + QLatin1Char('/') + newBase.trimmed();
		if (!suffix.isEmpty())
			target += QLatin1Char('.') + suffix;
		if (QFileInfo::exists(target)) {
			QMessageBox::warning(this, QStringLiteral("Rename recording"),
					     QStringLiteral("A file with that name already exists."));
			return;
		}
		if (!QFile::rename(path, target))
			QMessageBox::warning(this, QStringLiteral("Rename recording"),
					     QStringLiteral("Could not rename the file."));
		refreshRecentList();
	} else if (chosen == deleteAct) {
		if (QMessageBox::question(
			    this, QStringLiteral("Delete recording"),
			    QStringLiteral("Delete \"%1\"?\nThis cannot be undone.")
				    .arg(QFileInfo(path).fileName())) != QMessageBox::Yes)
			return;
		if (!QFile::remove(path))
			QMessageBox::warning(this, QStringLiteral("Delete recording"),
					     QStringLiteral("Could not delete the file."));
		refreshRecentList();
	}
}

QString MainWindow::elapsedString() const
{
	if (recStartMs_ == 0)
		return QStringLiteral("00:00:00");

	qint64 paused = pausedAccumMs_;
	if (recorder_.isPaused() && pauseStartMs_ > 0)
		paused += QDateTime::currentMSecsSinceEpoch() - pauseStartMs_;

	qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - recStartMs_ - paused;
	if (elapsedMs < 0)
		elapsedMs = 0;
	const int totalSecs = int(elapsedMs / 1000);
	return QStringLiteral("%1:%2:%3")
		.arg(totalSecs / 3600, 2, 10, QLatin1Char('0'))
		.arg((totalSecs % 3600) / 60, 2, 10, QLatin1Char('0'))
		.arg(totalSecs % 60, 2, 10, QLatin1Char('0'));
}

void MainWindow::updateButtons()
{
	const bool recording = recorder_.isRecording();
	const bool paused = recorder_.isPaused();
	const bool transitioning = starting_ || stopping_ || countingDown_;

	// Braille spinner frames for the in-progress states.
	static const char *kSpin[] = {"\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8",
				      "\xE2\xA0\xBC", "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7",
				      "\xE2\xA0\x87", "\xE2\xA0\x8F"};
	const QString spin = QString::fromUtf8(kSpin[((spinPhase_ % 10) + 10) % 10]);

	// Single Record/Stop toggle, with transitional Starting…/Stopping… states.
	if (countingDown_) {
		primaryButton_->setText(QStringLiteral("Starting in %1…").arg(countdownRemaining_));
		primaryButton_->setEnabled(false);
		primaryButton_->setToolTip(QStringLiteral("Press Esc on the countdown to cancel"));
	} else if (starting_) {
		primaryButton_->setText(QStringLiteral("%1  Starting…").arg(spin));
		primaryButton_->setEnabled(false);
		primaryButton_->setToolTip(QString());
	} else if (stopping_) {
		primaryButton_->setText(QStringLiteral("%1  Stopping…").arg(spin));
		primaryButton_->setEnabled(false);
		primaryButton_->setToolTip(QString());
	} else if (recording) {
		primaryButton_->setText(QStringLiteral("■  Stop"));
		primaryButton_->setEnabled(true);
		primaryButton_->setToolTip(QString());
	} else {
		primaryButton_->setText(QStringLiteral("●  Record"));
		primaryButton_->setEnabled(!recordingBlocked_);
		primaryButton_->setToolTip(recordingBlocked_
						   ? QStringLiteral("Resolve the warnings above before recording")
						   : QString());
	}

	// Pause/Resume: only present while actively recording (hidden while stopping).
	pauseButton_->setVisible(recording && !stopping_);
	if (recording) {
		if (paused) {
			pauseButton_->setText(QStringLiteral("▶  Resume"));
			pauseButton_->setStyleSheet(QStringLiteral(
				"background:#3fb950;border:none;color:white;border-radius:10px;"));
		} else {
			pauseButton_->setText(QStringLiteral("⏸  Pause"));
			pauseButton_->setStyleSheet(QStringLiteral(
				"background:#d29922;border:none;color:white;border-radius:10px;"));
		}
	}

	const bool locked = recording || transitioning;
	presetCombo_->setEnabled(!locked);
	editPresetButton_->setEnabled(!locked);
	newPresetButton_->setEnabled(!locked);
	if (webcamSettingsButton_)
		webcamSettingsButton_->setEnabled(!locked);
	captureModeCombo_->setEnabled(!locked);

	updateStatusChip();
}

void MainWindow::updateStatusChip()
{
	if (!statusBadge_)
		return;

	QString text;
	QColor color;
	bool pulse = false;
	QString tip;

	if (countingDown_) {
		text = QStringLiteral("Starting in %1s").arg(countdownRemaining_);
		color = QColor(0xd2, 0x99, 0x22);
		pulse = true;
	} else if (starting_) {
		text = QStringLiteral("Starting");
		color = QColor(0xd2, 0x99, 0x22);
		pulse = true;
	} else if (stopping_) {
		text = QStringLiteral("Stopping");
		color = QColor(0xd2, 0x99, 0x22);
		pulse = true;
	} else if (recorder_.isRecording()) {
		if (recorder_.isPaused()) {
			text = QStringLiteral("Paused");
			color = QColor(0xd2, 0x99, 0x22);
			if (focusPaused_)
				tip = QStringLiteral("Paused — target app not focused");
		} else {
			text = QStringLiteral("Recording");
			color = QColor(0xe5, 0x48, 0x4d);
			pulse = true;
		}
	} else if (recordingBlocked_) {
		text = QStringLiteral("Error");
		color = QColor(0xe5, 0x48, 0x4d);
		tip = firstIssue_.isEmpty() ? QStringLiteral("Not ready to record") : firstIssue_;
	} else {
		text = QStringLiteral("Ready");
		color = QColor(0x3f, 0xb9, 0x50);
		pulse = true;
		if (!firstIssue_.isEmpty())
			tip = firstIssue_; // non-blocking caution shown on hover
	}

	statusBadge_->setStatus(text, color, pulse);
	statusBadge_->setToolTip(tip);
}

void MainWindow::tickState()
{
	++spinPhase_; // drive the Starting…/Stopping… spinner

	if (!recorder_.isRecording()) {
		if (recStartMs_ != 0) {
			// Recording just ended — reset the timer accounting.
			recStartMs_ = 0;
			pausedAccumMs_ = 0;
			pauseStartMs_ = 0;
			wasPaused_ = false;
			updateRegionToolVisibility(); // leave recording mode
			if (mouseFx_)
				mouseFx_->stop();
			webcam_.stop();
			focusPaused_ = false;
			targetPid_ = 0;
			markersPath_.clear();
			stopping_ = false; // finalize complete
		}
		timerLabel_->setText(QStringLiteral("00:00:00"));
		updateButtons();
		return;
	}

	starting_ = false; // output is now active

	tickFocus(); // pause/resume on target-app focus changes

	// Track pause transitions to keep the timer accurate regardless of what
	// triggered the pause (button or idle monitor).
	const bool paused = recorder_.isPaused();
	if (paused && !wasPaused_)
		pauseStartMs_ = QDateTime::currentMSecsSinceEpoch();
	else if (!paused && wasPaused_ && pauseStartMs_ > 0) {
		pausedAccumMs_ += QDateTime::currentMSecsSinceEpoch() - pauseStartMs_;
		pauseStartMs_ = 0;
	}
	wasPaused_ = paused;

	timerLabel_->setText(elapsedString());
	updateButtons();
}

void MainWindow::tickIdle()
{
	if (!recorder_.isRecording() || !idle_)
		return;

	const int timeout = activePreset().idleTimeoutSeconds;
	if (timeout <= 0)
		return;

	const double idleSecs = idle_->currentIdleSeconds();

	if (!recorder_.isPaused()) {
		if (idleSecs >= timeout && recorder_.pause(true)) {
			autoPaused_ = true;
			updateButtons();
		}
	} else if (autoPaused_ && idleSecs < timeout) {
		recorder_.pause(false);
		autoPaused_ = false;
		updateButtons();
	}
}

void MainWindow::tickFocus()
{
	if (!recorder_.isRecording() || !activePreset().pauseOnFocusLoss || targetPid_ == 0)
		return;

	const uint64_t fg = ForegroundWatcher::foregroundProcessId();
	if (fg == 0)
		return; // unknown/unsupported — don't change state

	// Child windows, dialogs, and file pickers of the target belong to the same
	// process, so a plain process-id match treats them as still focused.
	const bool focused = (fg == targetPid_);

	if (!focused && !recorder_.isPaused()) {
		if (recorder_.pause(true)) {
			focusPaused_ = true;
			writeMarker(QStringLiteral("Auto Paused (Application Lost Focus)"));
			updateButtons();
		}
	} else if (focused && focusPaused_ && recorder_.isPaused()) {
		recorder_.pause(false);
		focusPaused_ = false;
		writeMarker(QStringLiteral("Auto Resumed (Application Regained Focus)"));
		updateButtons();
	}
}

void MainWindow::writeMarker(const QString &label)
{
	if (markersPath_.isEmpty())
		return;
	QFile f(markersPath_);
	if (f.open(QIODevice::Append | QIODevice::Text)) {
		const QString line = QStringLiteral("%1  %2\n").arg(elapsedString(), label);
		f.write(line.toUtf8());
	}
}

} // namespace harpia
