#include "MainWindow.hpp"

#include "core/Logger.hpp"

#include "UiIcons.hpp"
#include "UiText.hpp"

#include "AudioPanel.hpp"
#include "ClipLibraryWindow.hpp"
#include "ErrorLogsPanel.hpp"
#include "MainDevPanel.hpp"
#include "MouseFxOverlay.hpp"
#include "Version.hpp"
#include "PresetEditorDialog.hpp"
#include "RecentListWidget.hpp"
#include "RecorderControlsOverlay.hpp"
#include "CountdownOverlay.hpp"
#include "RegionDialogs.hpp"
#include "MonitorMatch.hpp"
#include "RegionTool.hpp"
#include "ScreenBorderOverlay.hpp"
#include "ShareExportDialog.hpp"
#include "StatusBadge.hpp"
#include "editor/VideoEditorWindow.hpp"
#include "WebcamPreview.hpp"
#include "core/EncoderFactory.hpp"
#include "core/Remuxer.hpp"
#include "platform/CameraAccess.hpp"
#include "platform/ForegroundWatcher.hpp"
#include "core/ObsContext.hpp"
#include "library/ClipLibrary.hpp"
#include "model/PresetStore.hpp"

#include <util/base.h> // blog

#if defined(_WIN32)
#include <windows.h> // EnumDisplayDevices — correlate OBS monitor_id → QScreen
#endif

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
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
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRunnable>
#include <QCursor>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QResizeEvent>
#include <QStatusBar>
#include <QToolButton>
#include <QStorageInfo>
#include <QStyle>
#include <QThreadPool>
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

// Scale a thumbnail to FILL the card slot (cover + center crop), so every
// recent-recordings card shows a uniform full-bleed image no matter the
// recording's aspect ratio (portrait region captures included).
QIcon cardIcon(const QImage &img)
{
	if (img.isNull())
		return QIcon();
	const QImage scaled =
		img.scaled(kStripThumb, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
	const QRect crop((scaled.width() - kStripThumb.width()) / 2,
			 (scaled.height() - kStripThumb.height()) / 2, kStripThumb.width(),
			 kStripThumb.height());
	return QIcon(QPixmap::fromImage(scaled.copy(crop)));
}
} // namespace

MainWindow::MainWindow(ObsContext &obs, PresetStore &presets, QString defaultFolder, QWidget *parent)
	: QMainWindow(parent),
	  obs_(obs),
	  presets_(presets),
	  idle_(IdleMonitor::create()),
	  defaultFolder_(std::move(defaultFolder))
{
	setWindowTitle(QStringLiteral("Harpia Recorder  v%1").arg(QString::fromUtf8(appVersion())));

	ownPid_ = (uint64_t)QCoreApplication::applicationPid();

	// Saved capture regions (global; shared across presets).
	regionStore_ = std::make_unique<RegionStore>(presets_.configDir());
	regionStore_->load();

	if (!presets_.presets().empty())
		activePresetId_ = presets_.presets().front().id;

	canvasSize_ = canvasForActivePreset();

	auto *central = new QWidget(this);
	auto *root = new QVBoxLayout(central);
	rootLayout_ = root;
	root->setContentsMargins(18, 14, 18, 14);
	root->setSpacing(12);

	// Muted field labels: quiet hierarchy so control groups scan instantly. An
	// optional tooltip mirrors the control's, so hovering the label helps too.
	auto fieldLabel = [central](const QString &text, const QString &tip = QString()) {
		auto *l = new QLabel(text, central);
		l->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
		if (!tip.isEmpty())
			l->setToolTip(tip);
		return l;
	};
	// One spacing rule everywhere: 8px inside a group, 18px between groups.
	constexpr int kGroupGap = 18;

	// ---- Toolbar row 1: preset + capture + idle -------------------------
	auto *row1 = new QHBoxLayout;
	row1Layout_ = row1;
	row1->setSpacing(8);

	row1->addWidget(fieldLabel(
		QStringLiteral("Preset"),
		QStringLiteral("Recording preset: output folder, format, quality and filename pattern.")));
	presetCombo_ = new QComboBox(central);
	presetCombo_->setMinimumWidth(160);
	presetCombo_->setContextMenuPolicy(Qt::CustomContextMenu);
	presetCombo_->setToolTip(QStringLiteral("Right-click to edit or delete this preset"));
	row1->addWidget(presetCombo_);

	newPresetButton_ = new QPushButton(QStringLiteral("New"), central);
	newPresetButton_->setToolTip(QStringLiteral("Create a new preset"));
	row1->addWidget(newPresetButton_);

	editPresetButton_ = new QPushButton(QStringLiteral("Edit"), central);
	editPresetButton_->setToolTip(QStringLiteral("Edit the selected preset"));
	row1->addWidget(editPresetButton_);

	row1->addSpacing(kGroupGap);
	row1->addWidget(fieldLabel(
		QStringLiteral("Capture"),
		QStringLiteral("What to record: the entire monitor, a custom on-screen region, or a saved region.")));
	captureModeCombo_ = new QComboBox(central);
	// Items carry a string tag in their data: "monitor", "region", "saved:<id>",
	// or "manage". Saved regions are appended by reloadCaptureModeCombo().
	captureModeCombo_->addItem(QStringLiteral("Entire Monitor"), QStringLiteral("monitor"));
	captureModeCombo_->addItem(QStringLiteral("Custom Region"), QStringLiteral("region"));
	captureModeCombo_->setToolTip(
		QStringLiteral("What to record: the whole monitor, a custom region you drag on screen, "
			       "or a saved region. Right-click a region to save it."));
	row1->addWidget(captureModeCombo_);

	row1->addSpacing(kGroupGap);
	row1->addWidget(fieldLabel(
		QStringLiteral("Display"),
		QStringLiteral("Which monitor to record — also where the region overlay opens.")));
	monitorCombo_ = new QComboBox(central);
	monitorCombo_->setMinimumWidth(140); // without this the adjust policy collapses it
	monitorCombo_->setMaximumWidth(200);
	monitorCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	monitorCombo_->setToolTip(QStringLiteral(
		"Which display to record — applies to Entire Monitor and to Custom Region "
		"(the region overlay opens on this display)."));
	// The display list refreshes right before the popup opens (eventFilter).
	monitorCombo_->installEventFilter(this);
	row1->addWidget(monitorCombo_);

	row1->addSpacing(kGroupGap);
	// Countdown label + combo as one unit, so the whole thing can be hidden when
	// the window is too narrow (it's a nice-to-have, not an essential control).
	countdownGroup_ = new QWidget(central);
	auto *countdownLayout = new QHBoxLayout(countdownGroup_);
	countdownLayout->setContentsMargins(0, 0, 0, 0);
	countdownLayout->setSpacing(8);
	countdownLayout->addWidget(fieldLabel(
		QStringLiteral("Countdown"),
		QStringLiteral("On-screen countdown before recording starts, so you can get ready.")));
	countdownCombo_ = new QComboBox(countdownGroup_);
	countdownCombo_->addItem(QStringLiteral("Off"), 0);
	for (int s = 1; s <= 10; ++s)
		countdownCombo_->addItem(QStringLiteral("%1s").arg(s), s);
	countdownCombo_->setMaximumWidth(80);
	countdownCombo_->setToolTip(QStringLiteral(
		"Show an on-screen countdown before recording starts, so you can get ready. "
		"Saved on the active preset; never part of the recording."));
	countdownLayout->addWidget(countdownCombo_);
	row1->addWidget(countdownGroup_);
	row1->addStretch(1);
	root->addLayout(row1);

	// The behavior dropdowns (Focus app / Webcam / Pause when idle) live in the
	// middle section's LEFT column — created there, below.

	// ---- Recording readiness --------------------------------------------
	warningsBox_ = new QWidget(central);
	warningsLayout_ = new QVBoxLayout(warningsBox_);
	warningsLayout_->setContentsMargins(0, 0, 0, 0);
	warningsLayout_->setSpacing(3);
	warningsBox_->setVisible(false);
	root->addWidget(warningsBox_);

	// ---- Middle: two columns --------------------------------------------
	//   Focus app       [combo]      |
	//   Webcam          [combo]      |   [ ⬤ Record ] [ ⏸ Pause ]  00:00:00
	//   Pause when idle [combo]      |
	// Left: one labelled behavior dropdown per row (first item = off; labels
	// right-aligned so the controls line up). Right: the record controls,
	// centered in the remaining space. Weighted stretches (2 above / 3 below)
	// keep the section at a natural height when the Audio foldout is collapsed.
	root->addStretch(2);

	auto *middle = new QHBoxLayout;
	middleLayout_ = middle;
	middle->setSpacing(kGroupGap);

	auto *behaviorCol = new QVBoxLayout;
	behaviorColLayout_ = behaviorCol;
	behaviorCol->setSpacing(10);

	// All three behavior dropdowns share one fixed width so the column reads
	// as a justified block; long names elide (full text in the tooltip/popup).
	constexpr int kBehaviorComboW = 200;

	appCombo_ = new QComboBox(central);
	appCombo_->setFixedWidth(kBehaviorComboW);
	appCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	appCombo_->setToolTip(QStringLiteral(
		"Pick an application to auto-pause recording whenever it isn't focused "
		"(resumes when it is). Does not change what's captured — the capture mode "
		"still applies. First entry disables this."));
	appCombo_->addItem(QStringLiteral("Off"), QString());
	// The window list is refreshed just before the popup opens (eventFilter).
	appCombo_->installEventFilter(this);

	webcamCombo_ = new QComboBox(central);
	webcamCombo_->setFixedWidth(kBehaviorComboW);
	webcamCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	webcamCombo_->setToolTip(QStringLiteral(
		"Record this camera to its own file alongside the screen recording. "
		"First entry disables the webcam."));
	webcamCombo_->addItem(QStringLiteral("No webcam"), QString());
	// The camera list is filled just before the popup opens; see eventFilter.
	webcamCombo_->installEventFilter(this);

	idleCombo_ = new QComboBox(central);
	idleCombo_->addItem(QStringLiteral("Off"), 0);
	for (int s : {1, 2, 3, 5, 10})
		idleCombo_->addItem(QStringLiteral("%1 s").arg(s), s);
	idleCombo_->setFixedWidth(kBehaviorComboW);
	idleCombo_->setToolTip(QStringLiteral(
		"Auto-pause the recording after this many seconds without mouse/keyboard "
		"input, and resume on input. Off records regardless of activity."));

	// Region recording only: pause once the pointer has been outside the region
	// long enough, and resume when it returns. "Off" is -1 rather than 0 because
	// 0 is a real choice here meaning "the moment it leaves".
	regionLeaveCombo_ = new QComboBox(central);
	regionLeaveCombo_->addItem(QStringLiteral("Off"), kRegionWatchOff);
	for (int s : kRegionWatchSeconds)
		regionLeaveCombo_->addItem(QStringLiteral("%1 s").arg(s), s);
	regionLeaveCombo_->setFixedWidth(kBehaviorComboW);
	regionLeaveCombo_->setToolTip(QStringLiteral(
		"Pause the recording once the mouse pointer has been outside the "
		"recording region for this long, and resume it when the pointer comes "
		"back. 0 s pauses as soon as it leaves. Only applies to Custom Region "
		"capture."));

	auto behaviorRow = [&](const QString &text, QWidget *control) -> QWidget * {
		auto *roww = new QWidget(central);
		auto *h = new QHBoxLayout(roww);
		h->setContentsMargins(0, 0, 0, 0);
		h->setSpacing(8);
		auto *l = fieldLabel(text);
		l->setToolTip(control->toolTip()); // label mirrors the control's help
		l->setFixedWidth(110);
		l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		h->addWidget(l);
		h->addWidget(control);
		h->addStretch(1);
		behaviorCol->addWidget(roww);
		// Tracked so the Developer Panel can retune the label width + row spacing.
		behaviorLabels_.push_back(l);
		behaviorRows_.push_back(roww);
		return roww;
	};
	behaviorRow(QStringLiteral("Focus app"), appCombo_);
	behaviorRow(QStringLiteral("Webcam"), webcamCombo_);
	idleGroup_ = behaviorRow(QStringLiteral("Pause when idle"), idleCombo_);
	regionLeaveGroup_ = behaviorRow(QStringLiteral("Pause off-region"), regionLeaveCombo_);
	behaviorCol->addStretch(1);
	middle->addLayout(behaviorCol);

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
	controlsLayout_ = controls;
	controls->setSpacing(14);
	controls->addStretch(1);

	// Single Record/Stop toggle: Record when idle, Stop while recording.
	primaryButton_ = new QPushButton(QStringLiteral("\xE2\x97\x8F  Record"), central);
	primaryButton_->setObjectName(QStringLiteral("primaryButton"));
	primaryButton_->setMinimumSize(130, 46);
	primaryButton_->setFont(btnFont);
	primaryButton_->setToolTip(QStringLiteral("Start/stop recording (F9)"));
	controls->addWidget(primaryButton_, 0, Qt::AlignVCenter);

	// Pause/Resume: only shown while recording.
	pauseButton_ = new QPushButton(QStringLiteral("\xE2\x8F\xB8  Pause"), central);
	pauseButton_->setObjectName(QStringLiteral("pauseButton"));
	pauseButton_->setMinimumSize(104, 46);
	pauseButton_->setFont(btnFont);
	// Always visible, enabled only while recording — the row never rearranges
	// and there's no reserved hole where the button would be.
	pauseButton_->setEnabled(false);
	pauseButton_->setToolTip(QStringLiteral("Pause/resume the recording (F10)"));
	controls->addWidget(pauseButton_, 0, Qt::AlignVCenter);

	ctrlSeparator_ = makeSep();
	controls->addWidget(ctrlSeparator_, 0, Qt::AlignVCenter);

	// Elapsed time — monospaced, readable, but not oversized.
	timerLabel_ = new QLabel(QStringLiteral("00:00:00"), central);
	timerLabel_->setObjectName(QStringLiteral("timerLabel"));
	timerLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
	timerLabel_->setToolTip(QStringLiteral("Elapsed recording time (paused spans are not counted)."));
	QFont timerFont(QStringLiteral("monospace"));
	timerFont.setStyleHint(QFont::Monospace);
	timerFont.setPointSize(btnFont.pointSize() + 6);
	timerLabel_->setFont(timerFont);
	controls->addWidget(timerLabel_, 0, Qt::AlignVCenter);

	// Inline live webcam preview, shown only when the active preset records a
	// webcam. The enable toggle + device picker live in toolbar row 2.
	webcamBox_ = new QWidget(central);
	auto *wcLayout = new QHBoxLayout(webcamBox_);
	wcLayout->setContentsMargins(0, 0, 0, 0);
	wcLayout->setSpacing(8);
	webcamPreview_ = new WebcamPreview(webcamBox_);
	webcamPreview_->setFixedSize(100, 56);
	webcamPreview_->setToolTip(QStringLiteral("Live preview of the webcam being recorded to its own file."));
	wcLayout->addWidget(webcamPreview_);
	webcamWarn_ = new QLabel(QStringLiteral("Webcam not found"), webcamBox_);
	webcamWarn_->setStyleSheet(QStringLiteral("color:#e5484d;"));
	webcamWarn_->setVisible(false);
	webcamWarn_->setWordWrap(true);
	wcLayout->addWidget(webcamWarn_, 1);
	webcamBox_->setVisible(false);
	controls->addWidget(webcamBox_);

	controls->addStretch(1);
	middle->addLayout(controls, 1); // the record column takes the remaining width
	root->addLayout(middle);

	root->addStretch(3);

	// ---- Audio (collapsible foldout) -----------------------------------
	// A header toggle expands/collapses the live-levels panel so it can be
	// tucked away when not needed.
	auto *audioSection = new QWidget(central);
	auto *audioSectionLayout = new QVBoxLayout(audioSection);
	audioSectionLayout->setContentsMargins(0, 0, 0, 0);
	audioSectionLayout->setSpacing(4);

	audioToggleButton_ = new QToolButton(audioSection);
	audioToggleButton_->setText(QStringLiteral("Audio"));
	audioToggleButton_->setCheckable(true);
	audioToggleButton_->setChecked(true);
	audioToggleButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	audioToggleButton_->setArrowType(Qt::DownArrow);
	audioToggleButton_->setAutoRaise(true);
	audioToggleButton_->setCursor(Qt::PointingHandCursor);
	audioToggleButton_->setToolTip(QStringLiteral("Show or hide the audio sources and levels"));
	audioToggleButton_->setStyleSheet(
		QStringLiteral("QToolButton { border: none; font-weight: bold; padding: 2px 0; }"));
	audioSectionLayout->addWidget(audioToggleButton_, 0, Qt::AlignLeft);

	audioBody_ = new QFrame(audioSection);
	audioBody_->setObjectName(QStringLiteral("audioBody"));
	auto *audioBodyLayout = new QVBoxLayout(audioBody_);
	audioBodyLayout->setContentsMargins(10, 6, 10, 6);
	audioPanel_ = new AudioPanel(audio_, audioBody_);
	audioBodyLayout->addWidget(audioPanel_);
	audioSectionLayout->addWidget(audioBody_);

	connect(audioToggleButton_, &QToolButton::toggled, this, [this](bool on) {
		audioBody_->setVisible(on);
		audioToggleButton_->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
	});

	root->addWidget(audioSection);

	// ---- Recent recordings strip (hidden when the window is narrow) -----
	recentSection_ = new QWidget(central);
	auto *recentLayout = new QVBoxLayout(recentSection_);
	recentLayout->setContentsMargins(0, 0, 0, 0);
	recentLayout->setSpacing(6);

	auto *stripHeader = new QHBoxLayout;
	auto *recentHeaderLabel = new QLabel(QStringLiteral("Recent recordings"), recentSection_);
	recentHeaderLabel->setToolTip(
		QStringLiteral("Your latest recordings — double-click to play, right-click for more."));
	stripHeader->addWidget(recentHeaderLabel);
	stripHeader->addStretch(1);
	libraryButton_ = new QPushButton(QStringLiteral("Open Clip Library…"), recentSection_);
	libraryButton_->setToolTip(
		QStringLiteral("Browse, play, rename, edit and export all of your recordings."));
	stripHeader->addWidget(libraryButton_);
	recentLayout->addLayout(stripHeader);

	recentStrip_ = new RecentListWidget(recentSection_);
	recentStrip_->setViewMode(QListView::IconMode);
	recentStrip_->setFlow(QListView::LeftToRight);
	recentStrip_->setWrapping(false);
	recentStrip_->setMovement(QListView::Static);
	recentStrip_->setIconSize(kStripThumb);
	// Card height derived from the REAL font metrics (two caption lines: name,
	// then date · size) instead of hand-tuned pixels — survives DPI/font
	// scaling without clipping the text.
	const int captionH = 2 * recentStrip_->fontMetrics().height() + 8;
	recentStrip_->setGridSize(QSize(kStripThumb.width() + 24,
					kStripThumb.height() + captionH + 12));
	recentStrip_->setUniformItemSizes(true); // every card is one grid cell
	recentStrip_->setResizeMode(QListView::Adjust);
	// Widget height = one full card + frame + PERMANENTLY reserved scrollbar
	// space; when the scrollbar appeared on demand it stole viewport height
	// and clipped the bottom caption line.
	recentStrip_->setFixedHeight(recentStrip_->gridSize().height() +
				     recentStrip_->style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 8);
	recentStrip_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	recentStrip_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	recentStrip_->setContextMenuPolicy(Qt::CustomContextMenu);
	recentStrip_->setSpacing(6);
	recentLayout->addWidget(recentStrip_);

	root->addWidget(recentSection_);

	setCentralWidget(central);

	// Bottom status bar:  [● status]  ————————  [Error Logs] [version]
	// The passive status badge (Ready/Recording/Paused/Error) anchors the left;
	// diagnostics + version sit quietly on the right.
	statusBadge_ = new StatusBadge(this);
	statusBar()->addWidget(statusBadge_);
	// Quick "Google Drive" shortcut: shown only when the active preset carries a
	// share link; clicking it opens that link in the browser.
	driveLinkButton_ = new QPushButton(QStringLiteral("Google Drive"), this);
	driveLinkButton_->setFlat(true);
	driveLinkButton_->setCursor(Qt::PointingHandCursor);
	driveLinkButton_->setStyleSheet(QStringLiteral(
		"QPushButton{color:#3d84b8; background:transparent; border:none; font-size:%1px; padding:2px 8px;}"
		"QPushButton:hover{color:#5aa9e6; text-decoration:underline;}")
							.arg(uiCaptionPx()));
	driveLinkButton_->setVisible(false);
	statusBar()->addWidget(driveLinkButton_);
	connect(driveLinkButton_, &QPushButton::clicked, this, [this]() {
		const QString link = QString::fromStdString(activePreset().googleDriveLink);
		if (!link.isEmpty())
			QDesktopServices::openUrl(QUrl::fromUserInput(link));
	});
	// Developer Panel launcher: a quiet button that opens the live layout tuner.
	auto *devButton = new QPushButton(QStringLiteral("Dev"), this);
	devButton->setFlat(true);
	devButton->setCursor(Qt::PointingHandCursor);
	devButton->setStyleSheet(QStringLiteral(
		"QPushButton{color:#9a9fa8; background:transparent; border:none; font-size:%1px; padding:2px 8px;}"
		"QPushButton:hover{color:#e6e6e6;}")
							.arg(uiCaptionPx()));
	devButton->setToolTip(QStringLiteral("Developer Panel — live-tweak the window layout sizes (auto-saved)"));
	statusBar()->addPermanentWidget(devButton);
	connect(devButton, &QPushButton::clicked, this, &MainWindow::openDevPanel);
	errorLogsButton_ = new QPushButton(QStringLiteral("Error Logs"), this);
	errorLogsButton_->setToolTip(QStringLiteral("View recent warnings and errors written by the recorder."));
	errorLogsButton_->setFlat(true);
	errorLogsButton_->setCursor(Qt::PointingHandCursor);
	errorLogsButton_->setStyleSheet(QStringLiteral(
		"QPushButton{color:#9a9fa8; background:transparent; border:none; font-size:%1px; padding:2px 8px;}"
		"QPushButton:hover{color:#e6e6e6;}")
							.arg(uiCaptionPx()));
	statusBar()->addPermanentWidget(errorLogsButton_);
	auto *versionLabel = new QLabel(QStringLiteral("v%1").arg(QString::fromUtf8(appVersion())), this);
	versionLabel->setStyleSheet(QStringLiteral("color:#6b6f76; padding-right:6px; font-size:%1px;").arg(uiCaptionPx()));
	statusBar()->addPermanentWidget(versionLabel);
	statusBar()->setSizeGripEnabled(false);
	statusBar()->setStyleSheet(QStringLiteral("QStatusBar{background:transparent;} QStatusBar::item{border:none;}"));

	// Wide, PowerRec-like proportions; a bit taller to fit the audio meters.
	// A modest minimum lets the window shrink far enough that the responsive
	// layout can drop non-essential sections while the record controls stay
	// usable (see applyResponsiveLayout).
	setMinimumSize(560, 400);
	resize(940, 470);

	// ---- Wiring ---------------------------------------------------------
	connect(primaryButton_, &QPushButton::clicked, this, &MainWindow::onPrimaryButton);
	connect(pauseButton_, &QPushButton::clicked, this, &MainWindow::onPauseButton);

	// Keyboard shortcuts (also shown in the buttons' tooltips).
	auto *recordShortcut = new QShortcut(QKeySequence(Qt::Key_F9), this);
	connect(recordShortcut, &QShortcut::activated, this, &MainWindow::onPrimaryButton);
	auto *pauseShortcut = new QShortcut(QKeySequence(Qt::Key_F10), this);
	connect(pauseShortcut, &QShortcut::activated, this, &MainWindow::onPauseButton);
	connect(editPresetButton_, &QPushButton::clicked, this, [this]() { editActivePreset(); });
	connect(newPresetButton_, &QPushButton::clicked, this, &MainWindow::onNewPreset);
	connect(webcamCombo_, &QComboBox::activated, this, &MainWindow::onWebcamDeviceChanged);
	connect(appCombo_, &QComboBox::activated, this, &MainWindow::onAppWindowChanged);
	connect(monitorCombo_, &QComboBox::activated, this, &MainWindow::onMonitorChanged);
	connect(libraryButton_, &QPushButton::clicked, this, &MainWindow::onOpenClipLibrary);
	connect(errorLogsButton_, &QPushButton::clicked, this, &MainWindow::onOpenErrorLogs);
	connect(captureModeCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onCaptureModeChanged);
	connect(idleCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onIdleSettingChanged);
	connect(regionLeaveCombo_, &QComboBox::currentIndexChanged, this,
		&MainWindow::onRegionLeaveSettingChanged);
	connect(countdownCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onCountdownSettingChanged);
	connect(presetCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onPresetChanged);
	connect(presetCombo_, &QComboBox::customContextMenuRequested, this, &MainWindow::showPresetMenu);
	// Single click only selects; double-click opens the recording.
	connect(recentStrip_, &QListWidget::itemDoubleClicked, this, [](QListWidgetItem *item) {
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
	screenBorder_ = std::make_unique<ScreenBorderOverlay>();

	// Floating desktop Pause/Stop HUD: its buttons reuse the exact same handlers
	// as the main window's, so behavior stays identical wherever it's clicked.
	floatingControls_ = std::make_unique<RecorderControlsOverlay>();
	connect(floatingControls_.get(), &RecorderControlsOverlay::pauseClicked, this,
		&MainWindow::onPauseButton);
	connect(floatingControls_.get(), &RecorderControlsOverlay::stopClicked, this,
		&MainWindow::onPrimaryButton);

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
	// Re-evaluate once a drag ends: mode changes are deferred while one is in
	// flight, and grabbing the overlay changes which window is active.
	connect(regionTool_.get(), &RegionTool::interactionFinished, this,
		&MainWindow::updateRegionToolVisibility);
	connect(regionTool_.get(), &RegionTool::saveRegionRequested, this, &MainWindow::onSaveRegionRequested);
	connect(regionTool_.get(), &RegionTool::manageRegionsRequested, this,
		&MainWindow::openSavedRegionsManager);

	// Populate the capture dropdown with any saved regions, and the display list.
	reloadCaptureModeCombo();
	reloadMonitorCombo();

	applyDarkTheme();

	stateTimer_ = new QTimer(this);
	stateTimer_->setInterval(250);
	connect(stateTimer_, &QTimer::timeout, this, &MainWindow::tickState);
	stateTimer_->start();

	idleTimer_ = new QTimer(this);
	idleTimer_->setInterval(1000);
	connect(idleTimer_, &QTimer::timeout, this, &MainWindow::tickIdle);
	idleTimer_->start();

	// Faster than the idle tick: with the timeout set to 0 s the stop should
	// feel like it happened when the pointer crossed the edge, and a one-second
	// poll would be up to a second late.
	regionWatchTimer_ = new QTimer(this);
	regionWatchTimer_->setInterval(250);
	connect(regionWatchTimer_, &QTimer::timeout, this, &MainWindow::tickRegionWatch);
	regionWatchTimer_->start();

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

	// Coalesces readiness refreshes fired by high-frequency events (a region
	// drag emits per mouse-move) into one run shortly after they settle.
	readinessDebounce_ = new QTimer(this);
	readinessDebounce_->setSingleShot(true);
	readinessDebounce_->setInterval(250);
	connect(readinessDebounce_, &QTimer::timeout, this, &MainWindow::refreshReadiness);

	reloadPresetCombo();
	syncIdleControls();
	audioPanel_->load(activePreset().recordDesktopAudio, activePreset().micDeviceIds,
			  activePreset().desktopVolume, activePreset().micVolumes);
	refreshDriveLink();
	updateButtons();
	applyResponsiveLayout(width()); // set initial section visibility

	// Restore any layout metrics saved from a previous Developer Panel session
	// and apply them over the freshly built (default-sized) layout.
	MainDevPanel::loadInto(layout_);
	setLayoutParams(layout_);

	// Start keyboard focus on the primary action instead of a random combo.
	primaryButton_->setFocus();

	// Everything expensive is in finishStartup(), which main() calls once this
	// window has actually been painted.
}

void MainWindow::finishStartup()
{
	// The slow half of coming up, deliberately after the window is on screen.
	//
	// None of this is needed to DRAW the window, and all of it talks to
	// hardware or the filesystem: probing cameras, listing recordings, sizing
	// the output drive, creating the capture source. Doing it in the
	// constructor meant the app was an empty taskbar entry for the whole of it.
	// Doing it here costs the same milliseconds but spends them with something
	// on screen, which is most of what "faster" means to whoever is waiting.

	// Bring the capture source up now so the first record is instant. This is
	// the biggest single item here and the reason the window comes first.
	obs_.resetVideo(canvasSize_.width(), canvasSize_.height(), activePreset().fps);
	capture_.startCapture(activePreset().monitorIndex, activePreset().showMouseCursor);

	refreshRecentList();
	refreshReadiness();
	refreshWebcamRow();
	updateButtons(); // readiness may have changed what is allowed

	// Surface (but never list) recordings orphaned by a crash/kill — they stay
	// in each output folder's hidden .harpia_tmp for manual salvage instead of
	// showing up in Recent Recordings as broken clips.
	for (const QString &folder : presetFolders()) {
		const QDir tmpDir(folder + QStringLiteral("/.harpia_tmp"));
		const QStringList orphans = tmpDir.entryList(QDir::Files);
		for (const QString &f : orphans)
			blog(LOG_WARNING, "[harpia] orphaned partial recording (crash/kill?): %s",
			     tmpDir.filePath(f).toUtf8().constData());
	}
}

MainWindow::~MainWindow()
{
	webcam_.stop();
	if (recorder_.isRecording())
		recorder_.stop();
}

void MainWindow::applyStripMetrics()
{
	if (!recentStrip_)
		return;
	const QSize thumb(layout_.stripThumbW, layout_.stripThumbH);
	recentStrip_->setIconSize(thumb);
	// Two caption lines (name, then date · size) sized from the real font
	// metrics, plus the tweakable card padding.
	const int captionH = 2 * recentStrip_->fontMetrics().height() + 8;
	recentStrip_->setGridSize(QSize(thumb.width() + layout_.stripCardExtraW,
					thumb.height() + captionH + layout_.stripCardExtraH));
	recentStrip_->setFixedHeight(recentStrip_->gridSize().height() +
				     recentStrip_->style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 8);
	recentStrip_->setSpacing(layout_.stripSpacing);
}

void MainWindow::setLayoutParams(const MainLayoutParams &p)
{
	layout_ = p;
	if (rootLayout_) {
		rootLayout_->setContentsMargins(p.rootMarginH, p.rootMarginV, p.rootMarginH, p.rootMarginV);
		rootLayout_->setSpacing(p.rootSpacing);
	}
	if (row1Layout_)
		row1Layout_->setSpacing(p.row1Spacing);
	if (middleLayout_)
		middleLayout_->setSpacing(p.middleSpacing);
	if (behaviorColLayout_)
		behaviorColLayout_->setSpacing(p.behaviorColSpacing);
	if (controlsLayout_)
		controlsLayout_->setSpacing(p.controlsSpacing);
	if (presetCombo_)
		presetCombo_->setMinimumWidth(p.presetComboW);
	if (monitorCombo_) {
		monitorCombo_->setMinimumWidth(p.monitorComboMinW);
		monitorCombo_->setMaximumWidth(p.monitorComboMaxW);
	}
	for (QComboBox *c : {appCombo_, webcamCombo_, idleCombo_, regionLeaveCombo_}) {
		if (c)
			c->setFixedWidth(p.behaviorComboW);
	}
	for (QLabel *l : behaviorLabels_) {
		if (l)
			l->setFixedWidth(p.behaviorLabelW);
	}
	for (QWidget *row : behaviorRows_) {
		if (row && row->layout())
			row->layout()->setSpacing(p.behaviorRowSpacing);
	}
	if (primaryButton_)
		primaryButton_->setMinimumSize(p.recordBtnW, p.recordBtnH);
	if (pauseButton_)
		pauseButton_->setMinimumSize(p.pauseBtnW, p.pauseBtnH);
	if (ctrlSeparator_)
		ctrlSeparator_->setFixedHeight(p.separatorH);
	if (webcamPreview_)
		webcamPreview_->setFixedSize(p.webcamPreviewW, p.webcamPreviewH);
	applyStripMetrics();
}

void MainWindow::refreshDriveLink()
{
	if (!driveLinkButton_)
		return;
	const QString link = QString::fromStdString(activePreset().googleDriveLink);
	driveLinkButton_->setVisible(!link.isEmpty());
	if (!link.isEmpty())
		driveLinkButton_->setToolTip(QStringLiteral("Open the preset's Google Drive share link:\n%1").arg(link));
}

void MainWindow::openDevPanel()
{
	if (!devPanel_)
		devPanel_ = new MainDevPanel(this, this);
	devPanel_->show();
	devPanel_->raise();
	devPanel_->activateWindow();
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
		QPushButton:focus { border: 1px solid #00aeef; }
		QPushButton#primaryButton { background: #e5484d; border: 1px solid transparent; color: white; border-radius: 10px; }
		QPushButton#primaryButton:hover { background: #f05a5f; }
		QPushButton#primaryButton:focus { border: 1px solid #ffd9da; }
		QPushButton#primaryButton:disabled { background: #5a3a3b; color: #9a7a7b; }
		QPushButton#pauseButton { border-radius: 10px; }
		QPushButton#pauseButton:disabled { background: #24262b; color: #565b63; border: 1px solid #2b2f35; }
		QComboBox, QSpinBox { background: #2b2d31; border: 1px solid #3a3d42; border-radius: 6px; padding: 4px 8px; }
		QComboBox:focus, QSpinBox:focus { border: 1px solid #00aeef; }
		QListWidget { background: #202225; border: 1px solid #303338; border-radius: 8px; }
		QListWidget::item:selected { background: #3a3d42; }
		QFrame#audioBody { background: #202225; border: 1px solid #303338; border-radius: 8px; }
	)"));
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
	QMainWindow::resizeEvent(event);
	applyResponsiveLayout(event->size().width());
}

void MainWindow::applyResponsiveLayout(int width)
{
	// Progressive disclosure: hide the least essential sections first as the
	// window narrows, so controls never overlap and the record button, timer
	// and pause always stay visible and aligned.
	//   >= 880 : everything
	//   >= 700 : hide the recent-recordings gallery
	//   <  640 : also hide the countdown picker and the idle auto-pause group
	// The status badge lives in the bottom status bar and always stays visible.
	if (recentSection_)
		recentSection_->setVisible(width >= 880);
	if (countdownGroup_)
		countdownGroup_->setVisible(width >= 700);
	if (idleGroup_)
		idleGroup_->setVisible(width >= 640);
	// This row has two reasons to be hidden — the window is narrow, or we are
	// not in Region mode — and they arrive from different places. Record the
	// width verdict and let one function combine them, so whichever fires last
	// cannot undo the other.
	regionLeaveNarrow_ = width < 640;
	updateRegionLeaveVisibility();
}

const Preset &MainWindow::activePreset() const
{
	if (const Preset *p = presets_.find(activePresetId_))
		return *p;
	// Guard the (theoretical) empty store — .front() on an empty vector is UB.
	if (!presets_.presets().empty())
		return presets_.presets().front();
	static const Preset fallback = Preset::makeDefault("");
	return fallback;
}

#if defined(_WIN32)
// The GDI device name (e.g. "\\.\DISPLAY2", matching QScreen::name()) for the
// display whose monitor device-interface path equals `monId` — the value OBS's
// monitor_capture stores in "monitor_id". Empty if it can't be resolved.
static QString gdiNameForMonitorId(const std::string &monId)
{
	if (monId.empty())
		return QString();
	const QString want = QString::fromStdString(monId);
	for (DWORD ai = 0;; ++ai) {
		DISPLAY_DEVICEW adapter{};
		adapter.cb = sizeof(adapter);
		if (!EnumDisplayDevicesW(nullptr, ai, &adapter, 0))
			break;
		if (!(adapter.StateFlags & DISPLAY_DEVICE_ACTIVE))
			continue;
		DISPLAY_DEVICEW mon{};
		mon.cb = sizeof(mon);
		for (DWORD mi = 0;
		     EnumDisplayDevicesW(adapter.DeviceName, mi, &mon, EDD_GET_DEVICE_INTERFACE_NAME); ++mi) {
			if (want.compare(QString::fromWCharArray(mon.DeviceID), Qt::CaseInsensitive) == 0)
				return QString::fromWCharArray(adapter.DeviceName);
		}
	}
	return QString();
}

// The monitor's human-readable name (what Qt 6's QScreen::name() returns on
// Windows, e.g. "SMT22A550") for a GDI display like "\\.\DISPLAY1", via the
// DisplayConfig API. Empty if unresolved.
static QString friendlyNameForGdi(const QString &gdiName)
{
	UINT32 nPath = 0, nMode = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPath, &nMode) != ERROR_SUCCESS)
		return QString();
	std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPath);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(nMode);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPath, paths.data(), &nMode, modes.data(),
			       nullptr) != ERROR_SUCCESS)
		return QString();
	for (UINT32 i = 0; i < nPath; ++i) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
		src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		src.header.size = sizeof(src);
		src.header.adapterId = paths[i].sourceInfo.adapterId;
		src.header.id = paths[i].sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS)
			continue;
		if (gdiName.compare(QString::fromWCharArray(src.viewGdiDeviceName),
				    Qt::CaseInsensitive) != 0)
			continue;
		DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
		tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
		tgt.header.size = sizeof(tgt);
		tgt.header.adapterId = paths[i].targetInfo.adapterId;
		tgt.header.id = paths[i].targetInfo.id;
		if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS)
			continue;
		return QString::fromWCharArray(tgt.monitorFriendlyDeviceName);
	}
	return QString();
}

// Native desktop rect (device pixels) of a GDI display.
static bool nativeRectForGdi(const QString &gdiName, QRect *out)
{
	DEVMODEW dm{};
	dm.dmSize = sizeof(dm);
	if (!EnumDisplaySettingsW(reinterpret_cast<const wchar_t *>(gdiName.utf16()),
				  ENUM_CURRENT_SETTINGS, &dm))
		return false;
	*out = QRect(dm.dmPosition.x, dm.dmPosition.y, int(dm.dmPelsWidth), int(dm.dmPelsHeight));
	return true;
}

// All active GDI displays with their native rects (for arrangement matching).
static std::vector<std::pair<QString, QRect>> activeGdiDisplays()
{
	std::vector<std::pair<QString, QRect>> out;
	for (DWORD ai = 0;; ++ai) {
		DISPLAY_DEVICEW adapter{};
		adapter.cb = sizeof(adapter);
		if (!EnumDisplayDevicesW(nullptr, ai, &adapter, 0))
			break;
		if (!(adapter.StateFlags & DISPLAY_DEVICE_ACTIVE))
			continue;
		const QString name = QString::fromWCharArray(adapter.DeviceName);
		QRect r;
		if (nativeRectForGdi(name, &r))
			out.emplace_back(name, r);
	}
	return out;
}
#endif

QScreen *MainWindow::screenForActivePreset() const
{
	const QList<QScreen *> screens = QGuiApplication::screens();
	if (screens.isEmpty())
		return nullptr;

#if defined(_WIN32)
	// The overlay/crop origin must sit on the SAME physical monitor OBS captures.
	// Qt's screen order and OBS's monitor list order are independent on Windows,
	// so map by device identity (OBS monitor_id → GDI name → QScreen) rather
	// than trusting the bare index. The choosing is in MonitorMatch, where it
	// can be tested; this end only gathers the facts Win32 has to answer for.
	const std::vector<MonitorOption> mons = CaptureManager::enumerateMonitors();
	// Against the OBS count, because the index IS an OBS index. Checking it
	// against Qt's count used to send a perfectly valid choice to 0 whenever
	// OBS saw more displays than Qt did -- and only for the overlay, since the
	// recording indexes the OBS list directly. That is the two disagreeing.
	const int idx = clampMonitorIndex(activePreset().monitorIndex, int(mons.size()));
	if (idx < (int)mons.size() && mons[idx].isString) {
		const QString gdi = gdiNameForMonitorId(mons[idx].strValue);
		if (!gdi.isEmpty()) {
			QVector<QtScreenDesc> qtScreens;
			qtScreens.reserve(screens.size());
			for (QScreen *s : screens)
				qtScreens.append({s->name(), s->geometry().topLeft()});

			QVector<GdiDisplayDesc> all;
			for (const auto &d : activeGdiDisplays())
				all.append({d.first, friendlyNameForGdi(d.first), d.second.topLeft()});

			// Take the target's own descriptor out of the list rather
			// than rebuilding a half-filled one: the arrangement step
			// ranks it against its neighbours, so it has to be the same
			// record they are.
			GdiDisplayDesc target{gdi, friendlyNameForGdi(gdi), QPoint()};
			for (const GdiDisplayDesc &d : all)
				if (d.gdiName.compare(gdi, Qt::CaseInsensitive) == 0)
					target = d;
			const int hit = matchQtScreen(qtScreens, target, all);
			if (hit >= 0 && hit < screens.size()) {
				blog(LOG_INFO,
				     "[harpia] monitor map: OBS #%d (%s) -> Qt screen '%s' via %s",
				     idx, gdi.toUtf8().constData(),
				     screens.at(hit)->name().toUtf8().constData(), lastMatchMethod());
				return screens.at(hit);
			}
			blog(LOG_WARNING,
			     "[harpia] monitor map: no Qt screen matches OBS #%d (%s) — falling back to index",
			     idx, gdi.toUtf8().constData());
		}
	}
#endif
	// No identity to go on: the bare index against Qt's list, which is a guess,
	// so it is range-checked against the list it is actually indexing.
	return screens.at(clampMonitorIndex(activePreset().monitorIndex, screens.size()));
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
	// scaling / custom sizes. ("Record only one application" doesn't affect the
	// captured area — it only drives focus auto-pause.)
	canvasSize_ = canvasForActivePreset();
	uint32_t baseW, baseH;

	if (captureMode_ == CaptureMode::Region && currentRegion_.enabled &&
	    currentRegion_.width >= 16 && currentRegion_.height >= 16) {
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

	// Capture the selected display; a custom region (if any) crops it. The
	// application chosen for "Record only one application" never changes what's
	// captured — only whether recording is auto-paused when it loses focus.
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

	// EVERY format records into a hidden temp folder first and is moved (or,
	// for MP4, losslessly remuxed from a crash-safe temp .mkv) into place on a
	// clean stop. A crash or Task Manager kill therefore can never leave a
	// partial recording where the library would list it — orphans stay in
	// .harpia_tmp (see the startup sweep log).
	QDir tmp(dir.filePath(QStringLiteral(".harpia_tmp")));
	tmp.mkpath(QStringLiteral("."));
	const QString recordPath =
		tmp.filePath(baseName + QLatin1Char('.') +
			     (preset.format == RecordingFormat::MP4
				      ? QStringLiteral("mkv")
				      : QString::fromStdString(preset.extension())));

	if (!recorder_.start(preset, recordPath.toStdString())) {
		// The timer label gets overwritten by the next tick — the status bar
		// keeps the failure visible (details are in the Error Logs).
		statusBar()->showMessage(
			QStringLiteral("Could not start the recording — see Error Logs for details"), 10000);
		starting_ = false;
		updateButtons();
		return;
	}

	// Remember this recording's files + minimum length for the short-clip check
	// and the post-stop remux.
	lastRecordedPath_ = recordPath;
	lastScreenPath_ = screenPath;
	lastWebcamPath_.clear();
	lastMinSeconds_ = preset.minRecordingSeconds;
	lastContentMs_ = 0; // set at Stop; stays 0 if it ends without a user Stop

	// Webcam as a separate synchronized file (never composited).
	if (preset.webcamEnabled) {
		QString wcFolder = (preset.webcamUseCustomFolder && !preset.webcamFolder.empty())
					   ? QString::fromStdString(preset.webcamFolder)
					   : QString::fromStdString(preset.outputFolder);
		QDir wdir(wcFolder);
		wdir.mkpath(QStringLiteral("."));
		const QString webcamPath = wdir.filePath(baseName + QStringLiteral("_webcam.mp4"));
		lastWebcamPath_ = webcamPath;
		// Reuse the live preview's already-open camera so the device isn't
		// opened twice (DirectShow cameras are usually exclusive).
		obs_source_t *shared = webcamPreview_ ? webcamPreview_->source() : nullptr;
		if (!webcam_.start(preset, preset.webcamDeviceId, preset.webcamWidth, preset.webcamHeight,
				   preset.webcamFps, webcamPath.toStdString(), shared)) {
			// No camera / failed to start — keep the screen recording going.
			QMessageBox::warning(
				this, QStringLiteral("Webcam"),
				QStringLiteral("Webcam recording could not start (no camera available?).\n"
					       "The screen recording continues without it."));
		}
	}

	logRecordingStart(preset, recordPath, screenPath, lastWebcamPath_, baseW, baseH, fps);

	// Focus auto-pause: "Record only one application" doesn't change the capture
	// source (that stays the selected mode — region/monitor); it only names the
	// application to watch, pausing the recording whenever that app isn't the
	// foreground window and resuming when it is. The target is the app chosen in
	// the dropdown, matched by executable so every window/process of a
	// multi-process app (browsers, Electron) counts as focused.
	focusPaused_ = false;
	targetExe_.clear();
	markersPath_.clear();
	if (appCaptureEnabled_ && !appWindowValue_.isEmpty()) {
		// Window values are "title:class:executable" with ':' inside fields
		// encoded as "#3A".
		targetExe_ = appWindowValue_.section(QLatin1Char(':'), -1)
				     .replace(QLatin1String("#3A"), QLatin1String(":"))
				     .trimmed();
		if (!targetExe_.isEmpty()) {
			markersPath_ = screenPath + QStringLiteral(".markers.txt");
			blog(LOG_INFO, "[harpia] focus auto-pause target: %s",
			     targetExe_.toUtf8().constData());
		}
	}

	recStartMs_ = QDateTime::currentMSecsSinceEpoch();
	pausedAccumMs_ = 0;
	pauseStartMs_ = 0;
	wasPaused_ = false;
	autoPaused_ = false;
	regionAutoPaused_ = false;
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

	// Recording border around the monitor (Full Screen mode only). Excluded from
	// the capture on Windows, so it isn't part of the video.
	if (captureMode_ == CaptureMode::Monitor && preset.showScreenBorder && screenBorder_) {
		QColor c(QString::fromStdString(preset.screenBorderColor));
		if (!c.isValid())
			c = QColor(0xe5, 0x48, 0x4d);
		screenBorder_->showBorder(screenForActivePreset(), c, preset.screenBorderThickness);
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
	// Capture the final content length now (at the user's Stop), before the async
	// finalize adds any lag, for the short-recording check.
	lastContentMs_ = contentElapsedMs();
	stopping_ = true;
	stopRequestMs_ = QDateTime::currentMSecsSinceEpoch(); // arms the watchdog
	forcedStop_ = false;
	updateButtons();
	recorder_.stop(); // async; tickState clears stopping_ once finalized
}

void MainWindow::finalizeStopped(const QString &recordedPath, const QString &finalPath,
				 const QString &webcamPath, const QString &markersPath, qint64 contentMs,
				 int minSeconds, bool needsRemux)
{
	// 1) Optional short-recording discard (operates on the file OBS wrote).
	if (minSeconds > 0 &&
	    discardShortRecording(recordedPath, webcamPath, markersPath, contentMs, minSeconds)) {
		if (closePending_)
			close(); // discarded — nothing left in flight
		return;
	}

	// 2) Move the temp recording into place — MP4 goes through the lossless
	// background remux (.mkv -> .mp4); every other format keeps its container
	// and is simply renamed out of the hidden temp folder.
	if (needsRemux) {
		const bool sameContainer = QFileInfo(recordedPath)
						   .suffix()
						   .compare(QFileInfo(finalPath).suffix(),
							    Qt::CaseInsensitive) == 0;
		if (!sameContainer) {
			remuxInBackground(recordedPath, finalPath);
			return;
		}
		QString target = finalPath;
		if (QFile::exists(target)) {
			// Same template name already exists — never overwrite a clip.
			target = QFileInfo(finalPath).absolutePath() + QLatin1Char('/') +
				 QFileInfo(finalPath).completeBaseName() + QStringLiteral("_%1.").arg(QDateTime::currentMSecsSinceEpoch() % 100000) +
				 QFileInfo(finalPath).suffix();
		}
		if (!QFile::rename(recordedPath, target)) {
			blog(LOG_WARNING, "[harpia] could not move the recording into place: %s",
			     recordedPath.toUtf8().constData());
			statusBar()->showMessage(
				QStringLiteral("Could not move the recording into the output folder — "
					       "it was kept in .harpia_tmp"),
				10000);
		} else {
			QDir().rmdir(QFileInfo(recordedPath).absolutePath()); // temp dir if empty
		}
		refreshClipViews();
		if (closePending_)
			close();
	} else {
		refreshClipViews();
		if (closePending_)
			close();
	}
}

bool MainWindow::discardShortRecording(const QString &recordedPath, const QString &webcamPath,
				       const QString &markersPath, qint64 contentMs, int minSeconds)
{
	if (recordedPath.isEmpty() || !QFileInfo::exists(recordedPath))
		return false;

	const int secs = int((contentMs + 500) / 1000);

	QMessageBox box(this);
	box.setWindowTitle(QStringLiteral("Short recording"));
	box.setIcon(QMessageBox::Question);
	box.setText(QStringLiteral("This recording is only %1 s — shorter than the %2 s minimum set for "
				   "this preset.\n\nDiscard it?")
			    .arg(secs)
			    .arg(minSeconds));
	QPushButton *keepBtn = box.addButton(QStringLiteral("Keep"), QMessageBox::RejectRole);
	QPushButton *discardBtn = box.addButton(QStringLiteral("Discard"), QMessageBox::DestructiveRole);
	box.setDefaultButton(keepBtn); // safest default
	box.exec();

	if (box.clickedButton() != discardBtn)
		return false; // keep it

	QFile::remove(recordedPath);
	if (!webcamPath.isEmpty())
		QFile::remove(webcamPath);
	if (!markersPath.isEmpty())
		QFile::remove(markersPath);
	// Tidy the temp dir if this was a to-be-remuxed .mkv.
	QDir().rmdir(QFileInfo(recordedPath).absolutePath());
	blog(LOG_INFO, "[harpia] discarded short recording (%d s < %d s minimum)", secs, minSeconds);
	refreshClipViews();
	return true;
}

void MainWindow::remuxInBackground(const QString &mkvPath, const QString &mp4Path)
{
	const qint64 srcSize = QFileInfo(mkvPath).size();
	blog(LOG_INFO, "[harpia] remux: %s -> %s (%lld MB, lossless stream copy)",
	     mkvPath.toUtf8().constData(), mp4Path.toUtf8().constData(), (long long)(srcSize / (1024 * 1024)));

	// Visible feedback while the file is being finalized — otherwise the clip
	// silently appears "late" in the recent strip and saving looks stuck.
	statusBar()->showMessage(QStringLiteral("Saving recording… (finalizing MP4, %1 MB)")
					 .arg(srcSize / (1024 * 1024)));
	remuxActive_ = true; // a pending close must wait for this to finish

	QPointer<MainWindow> guard(this);
	const qint64 startMs = QDateTime::currentMSecsSinceEpoch();

	QThreadPool::globalInstance()->start(QRunnable::create([guard, mkvPath, mp4Path, srcSize, startMs]() {
		const bool ok = Remuxer::remux(mkvPath.toStdString(), mp4Path.toStdString());
		const qint64 outSize = ok ? QFileInfo(mp4Path).size() : 0;
		QMetaObject::invokeMethod(
			qApp,
			[guard, mkvPath, mp4Path, ok, srcSize, outSize, startMs]() {
				const qint64 secs =
					(QDateTime::currentMSecsSinceEpoch() - startMs + 500) / 1000;
				if (ok) {
					QFile::remove(mkvPath);
					QDir().rmdir(QFileInfo(mkvPath).absolutePath()); // temp dir if empty
					blog(LOG_INFO,
					     "[harpia] remux complete: %lld MB -> %lld MB in %llds",
					     (long long)(srcSize / (1024 * 1024)),
					     (long long)(outSize / (1024 * 1024)), (long long)secs);
				} else {
					// A failed remux may have written a partial header/trailer —
					// remove the broken .mp4 so the library doesn't list it.
					QFile::remove(mp4Path);
					// Don't lose the recording: keep the .mkv beside the target.
					const QString fallback =
						QFileInfo(mp4Path).absolutePath() + QLatin1Char('/') +
						QFileInfo(mp4Path).completeBaseName() + QStringLiteral(".mkv");
					QFile::rename(mkvPath, fallback);
					blog(LOG_WARNING,
					     "[harpia] remux FAILED — kept the recording as %s",
					     fallback.toUtf8().constData());
				}
				if (guard) {
					guard->remuxActive_ = false;
					if (ok)
						guard->statusBar()->showMessage(
							QStringLiteral("Recording saved (%1 MB, %2 s)")
								.arg(outSize / (1024 * 1024))
								.arg(secs),
							6000);
					else
						guard->statusBar()->showMessage(
							QStringLiteral("Could not finalize MP4 — recording "
								       "kept as MKV"),
							10000);
					guard->refreshClipViews();
					if (guard->closePending_)
						guard->close();
				}
			},
			Qt::QueuedConnection);
	}));
}

void MainWindow::logRecordingStart(const Preset &p, const QString &recordedPath, const QString &finalPath,
				   const QString &webcamPath, uint32_t baseW, uint32_t baseH, int fps)
{
	const char *fmt = formatToString(p.format);
	const char *codec = codecToString(p.codec);
	const char *frMode = p.frameRateMode == FrameRateMode::VFR ? "VFR" : "CFR";

	blog(LOG_INFO, "======== Harpia recording start ========");
	blog(LOG_INFO, "[harpia] preset: '%s'", p.name.c_str());
	blog(LOG_INFO, "[harpia] format: %s  codec: %s  %ux%u @ %d fps (%s)", fmt, codec, baseW, baseH, fps,
	     frMode);
	const std::string bitrate = p.videoBitrateKbps > 0 ? (std::to_string(p.videoBitrateKbps) + " kbps")
							   : std::string("auto");
	blog(LOG_INFO, "[harpia] bitrate: %s  gpu-encode: %s", bitrate.c_str(), p.gpuCompression ? "yes" : "no");
	const char *captureKind = captureMode_ == CaptureMode::Region ? "region" : "monitor";
	blog(LOG_INFO, "[harpia] capture: %s  monitor#%d", captureKind, p.monitorIndex);
	if (appCaptureEnabled_ && !appWindowValue_.isEmpty())
		blog(LOG_INFO, "[harpia] focus-pause app: %s", appWindowValue_.toUtf8().constData());
	if (captureMode_ == CaptureMode::Region && currentRegion_.enabled)
		blog(LOG_INFO, "[harpia] region: %dx%d at (%d,%d)", currentRegion_.width, currentRegion_.height,
		     currentRegion_.x, currentRegion_.y);
	blog(LOG_INFO, "[harpia] output folder: %s", p.outputFolder.c_str());
	blog(LOG_INFO, "[harpia] recording to: %s", recordedPath.toUtf8().constData());
	if (recordedPath != finalPath)
		blog(LOG_INFO, "[harpia] will remux to: %s (fast-stop MKV->MP4)",
		     finalPath.toUtf8().constData());
	blog(LOG_INFO, "[harpia] audio: desktop=%s  mics=%zu", p.recordDesktopAudio ? "on" : "off",
	     p.micDeviceIds.size());
	blog(LOG_INFO, "[harpia] mouse: cursor=%s area=%s clicks=%s", p.showMouseCursor ? "on" : "off",
	     p.showMouseArea ? "on" : "off", p.recordMouseClicks ? "on" : "off");
	if (p.webcamEnabled)
		blog(LOG_INFO, "[harpia] webcam: '%s' %dx%d @ %d fps -> %s", p.webcamDeviceId.c_str(),
		     p.webcamWidth, p.webcamHeight, p.webcamFps,
		     webcamPath.isEmpty() ? "(failed)" : webcamPath.toUtf8().constData());
	else
		blog(LOG_INFO, "[harpia] webcam: off");
	blog(LOG_INFO, "[harpia] countdown: %ds  min-length: %ds  focus-pause: %s  screen-border: %s",
	     p.countdownSeconds, p.minRecordingSeconds,
	     (appCaptureEnabled_ && !appWindowValue_.isEmpty()) ? "on" : "off",
	     p.showScreenBorder ? "on" : "off");
	blog(LOG_INFO, "========================================");
}

void MainWindow::onPauseButton()
{
	if (!recorder_.isRecording())
		return;
	recorder_.togglePause();
	webcam_.pause(recorder_.isPaused()); // keep the companion file in sync
	notePauseTransition(recorder_.isPaused());
	autoPaused_ = false;  // manual action overrides the idle state machine
	regionAutoPaused_ = false; // ...and the off-region one
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
		created.name = uniquePresetName(created.name, created.id);
		presets_.upsert(created);
		activePresetId_ = created.id;
		reloadPresetCombo();
		syncIdleControls();
		refreshRecentList();
	}
}

std::string MainWindow::uniquePresetName(const std::string &wanted, const std::string &selfId) const
{
	// Two presets with the same name are indistinguishable in the preset combo
	// (and collide in the library's folder→preset map) — suffix "(2)", "(3)"…
	auto taken = [this, &selfId](const std::string &name) {
		for (const Preset &p : presets_.presets()) {
			if (p.id != selfId && p.name == name)
				return true;
		}
		return false;
	};
	if (!taken(wanted))
		return wanted;
	for (int i = 2;; ++i) {
		const std::string candidate = wanted + " (" + std::to_string(i) + ")";
		if (!taken(candidate))
			return candidate;
	}
}

void MainWindow::showPresetMenu(const QPoint &pos)
{
	if (recorder_.isRecording())
		return;

	QMenu menu(this);
	QAction *editAct = menu.addAction(QStringLiteral("Edit preset…"));
	QAction *dupAct = menu.addAction(QStringLiteral("Duplicate preset"));
	QAction *folderAct = menu.addAction(QStringLiteral("Open recordings folder"));
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
	} else if (chosen == folderAct) {
		QString folder = QString::fromStdString(cur->outputFolder);
		if (folder.isEmpty())
			folder = defaultFolder_;
		QDir().mkpath(folder);
		QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
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
		Preset updated = dlg.result();
		updated.name = uniquePresetName(updated.name, updated.id);
		presets_.upsert(updated);
		reloadPresetCombo();
		syncIdleControls();
		audioPanel_->load(activePreset().recordDesktopAudio, activePreset().micDeviceIds,
			  activePreset().desktopVolume, activePreset().micVolumes);
		refreshRecentList();
		hwProbeMs_ = 0; // settings may have changed monitor/mic/webcam use
		refreshReadiness();
		refreshWebcamRow();
		refreshDriveLink();
	}
}

void MainWindow::reloadWebcamCombo()
{
	// Fill the camera list, keeping whatever is currently chosen selected. Only
	// called when someone is actually looking at the list, because this is the
	// call that probes DirectShow.
	const QString had = webcamCombo_->currentData().toString();
	const std::vector<AudioDevice> cams = WebcamRecorder::cameras();
	QSignalBlocker block(webcamCombo_);
	webcamCombo_->clear();
	webcamCombo_->addItem(QStringLiteral("No webcam"), QString());
	for (const AudioDevice &c : cams)
		webcamCombo_->addItem(QString::fromStdString(c.name), QString::fromStdString(c.id));
	const int idx = had.isEmpty() ? 0 : webcamCombo_->findData(had);
	webcamCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
}

void MainWindow::refreshWebcamRow()
{
	const Preset &p = activePreset();

	webcamCombo_->setEnabled(!recorder_.isRecording());

	if (!p.webcamEnabled) {
		// Off: the combo rests on its first item; no preview, no warning row.
		//
		// And NO camera enumeration. WebcamRecorder::cameras() creates a
		// private DirectShow source and builds its properties, which walks
		// every capture filter registered on the machine -- including the
		// virtual cameras that OBS, Teams and Zoom leave installed, some of
		// which are slow to answer. This used to run on every startup whether
		// or not the webcam was switched on, which is the one unconditional
		// hardware probe here that nothing caches. The list is filled in when
		// the dropdown is opened (see eventFilter) or when the webcam is
		// turned on, both of which are moments the user is already waiting on
		// a camera.
		{
			QSignalBlocker block(webcamCombo_);
			webcamCombo_->clear();
			webcamCombo_->addItem(QStringLiteral("No webcam"), QString());
			webcamCombo_->setCurrentIndex(0);
		}
		webcamBox_->setVisible(false);
		if (webcamPreview_)
			webcamPreview_->clearDevice();
		return;
	}

	// Populate the device list: first item = "No webcam" (off), then cameras.
	const std::vector<AudioDevice> cams = WebcamRecorder::cameras();
	{
		QSignalBlocker block(webcamCombo_);
		webcamCombo_->clear();
		webcamCombo_->addItem(QStringLiteral("No webcam"), QString());
		for (const AudioDevice &c : cams)
			webcamCombo_->addItem(QString::fromStdString(c.name), QString::fromStdString(c.id));
	}

	const QString wantId = QString::fromStdString(p.webcamDeviceId);
	int idx = wantId.isEmpty() ? (cams.empty() ? 0 : 1) : webcamCombo_->findData(wantId);
	const bool wantedMissing = !wantId.isEmpty() && idx < 0;
	if (idx < 0)
		idx = cams.empty() ? 0 : 1; // fall back to the first available camera

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
	// First item ("No webcam") disables the webcam; any device enables it.
	// Persisted on the preset so it survives without opening the editor.
	const QString id = webcamCombo_->currentData().toString();
	if (Preset *cur = const_cast<Preset *>(presets_.find(activePresetId_))) {
		cur->webcamEnabled = !id.isEmpty();
		if (!id.isEmpty())
			cur->webcamDeviceId = id.toStdString();
		presets_.upsert(*cur);
	}
	hwProbeMs_ = 0; // re-probe hardware now that webcam use changed
	refreshWebcamRow();
	refreshReadiness();
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

	// Hardware enumeration (monitors / mics / cameras) is expensive — each call
	// builds obs source properties, and the camera probe even creates a source.
	// Re-probe at most every few seconds instead of on every 1.5s readiness tick.
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (hwProbeMs_ == 0 || now - hwProbeMs_ > 4000) {
		hwProbeMs_ = now;
		hwMonitorCount_ = (int)CaptureManager::enumerateMonitors().size();
		hwInputIds_.clear();
		if (!p.micDeviceIds.empty())
			for (const AudioDevice &d : AudioManager::inputDevices())
				hwInputIds_.push_back(d.id);
		hwCameraPresent_ = p.webcamEnabled ? !WebcamRecorder::cameras().empty() : true;

		// Folder + encoder checks share the TTL: filesystem stats (slow on
		// network drives) and encoder enumeration don't belong on every
		// 1.5s tick — much less on every region-drag mouse move.
		hwFolderIssue_ = 0;
		const QString folder = QString::fromStdString(p.outputFolder);
		if (!folder.isEmpty()) {
			QDir dir(folder);
			if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
				hwFolderIssue_ = 1;
			} else if (dir.exists() && !QFileInfo(folder).isWritable()) {
				hwFolderIssue_ = 2;
			} else {
				const QStorageInfo storage(folder);
				if (storage.isValid() && storage.bytesAvailable() > 0 &&
				    storage.bytesAvailable() < 500LL * 1024 * 1024)
					hwFolderIssue_ = 3;
			}
		}
		hwEncoderOk_ = p.format == RecordingFormat::GIF ||
			       !EncoderFactory::videoEncoderId(p).empty();
	}

	// --- Output folder --- (state cached above)
	const QString folder = QString::fromStdString(p.outputFolder);
	if (folder.isEmpty()) {
		warnings.push_back({QStringLiteral("No output folder is set."), [this]() { editActivePreset(); },
				    QStringLiteral("Set folder")});
	} else if (hwFolderIssue_ == 1) {
		warnings.push_back({QStringLiteral("Output folder does not exist and can't be created."),
				    [this]() { editActivePreset(); }, QStringLiteral("Fix folder")});
	} else if (hwFolderIssue_ == 2) {
		warnings.push_back({QStringLiteral("Output folder is not writable."),
				    [this]() { editActivePreset(); }, QStringLiteral("Fix folder")});
	} else if (hwFolderIssue_ == 3) {
		warnings.push_back({QStringLiteral("Low disk space (< 500 MB) on the output drive."), nullptr,
				    QString(), /*blocking=*/false});
	}

	// --- Display / region ---
	const int monitorCount = hwMonitorCount_;
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
		const std::vector<std::string> &available = hwInputIds_;
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
	if (p.webcamEnabled && !hwCameraPresent_) {
		warnings.push_back({QStringLiteral("Webcam is enabled but no camera is available — it will be "
						   "skipped for this recording."),
				    [this]() { editActivePreset(QStringLiteral("Webcam")); },
				    QStringLiteral("Fix webcam"),
				    /*blocking=*/false});
	}

	// --- Codec / video settings --- (availability cached above)
	if (!hwEncoderOk_) {
		warnings.push_back({QStringLiteral("The selected codec has no available encoder."),
				    [this]() { editActivePreset(); }, QStringLiteral("Change codec")});
	}
	if (p.fps <= 0) {
		warnings.push_back({QStringLiteral("Invalid frame rate."), [this]() { editActivePreset(); },
				    QStringLiteral("Fix video")});
	}

	// Recompute the blocking state + status headline (cheap, always).
	bool anyBlocking = false;
	for (const Warning &w : warnings) {
		if (w.blocking)
			anyBlocking = true;
	}
	recordingBlocked_ = anyBlocking;

	firstIssue_.clear();
	for (const Warning &w : warnings) {
		if (w.blocking) {
			firstIssue_ = w.message;
			break;
		}
	}
	if (firstIssue_.isEmpty() && !warnings.empty())
		firstIssue_ = warnings.front().message;

	// Only tear down and rebuild the warnings widgets when the set actually
	// changed — otherwise this churned QWidgets every 1.5s for no visible change.
	QStringList sig;
	sig.reserve((int)warnings.size());
	for (const Warning &w : warnings)
		sig << (w.blocking ? QLatin1Char('!') : QLatin1Char('-')) + w.message + w.fixLabel;

	if (sig != lastWarningSig_) {
		lastWarningSig_ = sig;

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
			auto *icon = new QLabel(row);
			icon->setPixmap(uiIcon(Glyph::Warning, 14, QColor(0xe2, 0xa0, 0x3f)).pixmap(14, 14));
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
		warningsBox_->setVisible(!warnings.empty());
	}

	updateButtons();
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
	const QString sel = captureModeCombo_->currentData().toString();

	// "Manage saved regions…" is an action, not a selectable mode — restore the
	// previous selection and open the manager.
	if (sel == QStringLiteral("manage")) {
		QSignalBlocker block(captureModeCombo_);
		captureModeCombo_->setCurrentIndex(prevCaptureIndex_);
		openSavedRegionsManager();
		return;
	}
	prevCaptureIndex_ = captureModeCombo_->currentIndex();

	if (sel == QStringLiteral("monitor")) {
		captureMode_ = CaptureMode::Monitor;
		currentRegion_ = CaptureRegion{};
		capture_.setRegion(currentRegion_);
	} else {
		// "region" (fresh custom) or "saved:<id>" (restore its position/size).
		captureMode_ = CaptureMode::Region;
		if (sel.startsWith(QStringLiteral("saved:"))) {
			const std::string id = sel.mid(6).toStdString();
			if (const SavedRegion *r = regionStore_->find(id)) {
				// The region lives on a specific display — switch to it.
				applyMonitorIndex(r->monitorIndex);
				currentRegion_ = CaptureRegion{true, r->x, r->y, r->width, r->height};
			}
		}
		// Seed a default region (centered, ~2/3 of the screen) if none yet.
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
		// Resolved after any saved-region monitor switch above.
		regionTool_->setScreen(screenForActivePreset());
		regionTool_->setRegionDevicePx(
			QRect(currentRegion_.x, currentRegion_.y, currentRegion_.width, currentRegion_.height));
		capture_.setRegion(currentRegion_);
	}
	updateRegionToolVisibility();
	updateRegionLeaveVisibility();
	updateButtons();
	refreshReadiness();
}

void MainWindow::reloadCaptureModeCombo()
{
	QSignalBlocker block(captureModeCombo_);
	const QString prev = captureModeCombo_->currentData().toString();

	captureModeCombo_->clear();
	captureModeCombo_->addItem(QStringLiteral("Entire Monitor"), QStringLiteral("monitor"));
	captureModeCombo_->addItem(QStringLiteral("Custom Region"), QStringLiteral("region"));

	const auto &regions = regionStore_->regions();
	if (!regions.empty()) {
		captureModeCombo_->insertSeparator(captureModeCombo_->count());
		for (const SavedRegion &r : regions)
			captureModeCombo_->addItem(QString::fromStdString(r.name),
						   QStringLiteral("saved:%1").arg(QString::fromStdString(r.id)));
		captureModeCombo_->insertSeparator(captureModeCombo_->count());
		captureModeCombo_->addItem(QStringLiteral("Manage saved regions…"), QStringLiteral("manage"));
	}

	// Keep the previous selection if it still exists, else reflect the mode.
	int idx = captureModeCombo_->findData(prev.isEmpty() ? QStringLiteral("monitor") : prev);
	if (idx < 0)
		idx = (captureMode_ == CaptureMode::Region) ? 1 : 0;
	captureModeCombo_->setCurrentIndex(idx);
	prevCaptureIndex_ = captureModeCombo_->currentIndex();
}

void MainWindow::onSaveRegionRequested()
{
	if (!regionTool_ || captureMode_ != CaptureMode::Region || !currentRegion_.enabled)
		return;

	SavedRegion r;
	r.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
	r.name = QStringLiteral("Region %1").arg(int(regionStore_->regions().size()) + 1).toStdString();
	r.x = currentRegion_.x;
	r.y = currentRegion_.y;
	r.width = currentRegion_.width;
	r.height = currentRegion_.height;
	r.monitorIndex = activePreset().monitorIndex; // the display it was drawn on

	RegionEditDialog dlg(r, this);
	if (dlg.exec() != QDialog::Accepted)
		return;
	const SavedRegion saved = dlg.result();
	regionStore_->upsert(saved);
	reloadCaptureModeCombo();
	// Select (and apply) the newly saved region.
	const int idx =
		captureModeCombo_->findData(QStringLiteral("saved:%1").arg(QString::fromStdString(saved.id)));
	if (idx >= 0)
		captureModeCombo_->setCurrentIndex(idx); // triggers onCaptureModeChanged -> applies it
}

void MainWindow::openSavedRegionsManager()
{
	SavedRegionsDialog dlg(*regionStore_, this);
	dlg.exec();
	reloadCaptureModeCombo();
}

void MainWindow::applyLiveCapture()
{
	if (recorder_.isRecording())
		return; // don't disturb an in-progress capture

	const Preset &p = activePreset();
	// The capture source is always the selected display (+ optional region crop);
	// "Record only one application" only selects the focus-pause target and never
	// changes what is captured.
	capture_.startCapture(p.monitorIndex, p.showMouseCursor);
	capture_.setRegion(currentRegion_);
	updateRegionToolVisibility();
}

void MainWindow::reloadMonitorCombo()
{
	const int want = activePreset().monitorIndex;
	QSignalBlocker block(monitorCombo_);
	monitorCombo_->clear();
	const std::vector<MonitorOption> monitors = CaptureManager::enumerateMonitors();
	if (monitors.empty()) {
		monitorCombo_->addItem(QStringLiteral("Primary display"), 0);
	} else {
		int idx = 0;
		for (const MonitorOption &m : monitors)
			monitorCombo_->addItem(QString::fromStdString(m.name), idx++);
	}
	monitorCombo_->setCurrentIndex(want >= 0 && want < monitorCombo_->count() ? want : 0);
}

void MainWindow::applyMonitorIndex(int index)
{
	const Preset *cur = presets_.find(activePresetId_);
	if (!cur || cur->monitorIndex == index)
		return;
	Preset updated = *cur;
	updated.monitorIndex = index;
	presets_.upsert(updated);

	// Re-anchor everything that depends on the display: canvas size, the live
	// capture source, and the region overlay (it opens on the new display).
	canvasSize_ = canvasForActivePreset();
	obs_.resetVideo(canvasSize_.width(), canvasSize_.height(), activePreset().fps);
	reloadMonitorCombo(); // keep the selector in sync when invoked indirectly

	// Custom Region: the region is canvas-relative, so it must MOVE with the
	// display — clamp it into the new canvas and re-anchor the overlay there
	// (previously the overlay stayed stranded on the old monitor).
	if (captureMode_ == CaptureMode::Region && currentRegion_.enabled && regionTool_) {
		CaptureRegion r = currentRegion_;
		r.width = std::min(r.width, canvasSize_.width());
		r.height = std::min(r.height, canvasSize_.height());
		r.x = std::clamp(r.x, 0, canvasSize_.width() - r.width);
		r.y = std::clamp(r.y, 0, canvasSize_.height() - r.height);
		currentRegion_ = r;
		regionTool_->setScreen(screenForActivePreset());
		regionTool_->setRegionDevicePx(QRect(r.x, r.y, r.width, r.height));
	}

	applyLiveCapture();
}

void MainWindow::onMonitorChanged()
{
	applyMonitorIndex(monitorCombo_->currentData().toInt());
	refreshReadiness();
}

void MainWindow::reloadAppCombo()
{
	// Fresh window list; keep the current selection when the app still runs.
	const QString want = appWindowValue_;
	QSignalBlocker block(appCombo_);
	appCombo_->clear();
	appCombo_->addItem(QStringLiteral("Off"), QString());
	for (const WindowOption &w : CaptureManager::enumerateWindows())
		appCombo_->addItem(QString::fromStdString(w.name), QString::fromStdString(w.value));
	int idx = want.isEmpty() ? 0 : appCombo_->findData(want);
	if (idx < 0) {
		// The watched app isn't running right now — keep the choice visible.
		appCombo_->addItem(QStringLiteral("(not running)"), want);
		idx = appCombo_->count() - 1;
	}
	appCombo_->setCurrentIndex(idx);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
	if (obj == appCombo_ && event->type() == QEvent::MouseButtonPress)
		reloadAppCombo(); // refresh the list right before the popup opens
	if (obj == monitorCombo_ && event->type() == QEvent::MouseButtonPress)
		reloadMonitorCombo(); // catch displays plugged/unplugged since launch
	// With the webcam off the camera list is left empty at startup (the probe is
	// slow and nothing was waiting on it), so it has to be filled the moment
	// someone goes looking for a camera -- otherwise there would be no way to
	// turn one on.
	if (obj == webcamCombo_ && event->type() == QEvent::MouseButtonPress)
		reloadWebcamCombo();
	return QMainWindow::eventFilter(obj, event);
}

void MainWindow::onAppWindowChanged()
{
	// First item ("Off") disables focus auto-pause. No applyLiveCapture():
	// the focus target only arms the auto-pause and never changes what is
	// captured — recreating the capture source here caused a pointless
	// restart (and a visible flash) on every selection.
	appWindowValue_ = appCombo_->currentData().toString();
	appCaptureEnabled_ = !appWindowValue_.isEmpty();
	updateButtons();
	refreshReadiness();
}

void MainWindow::onRegionChanged(const CaptureRegion &region)
{
	currentRegion_ = region;
	// Live update — crop_filter applies immediately, even while recording.
	capture_.setRegion(region);
	// Fired per mouse-move while dragging the region — debounce the readiness
	// pass instead of running filesystem checks dozens of times a second.
	readinessDebounce_->start();
}

void MainWindow::updateRegionToolVisibility()
{
	if (!regionTool_)
		return;
	// Mid-drag the overlay owns the interaction: re-masking under a move that
	// started in the interior would drop it halfway. Activation changes fire
	// exactly then -- clicking the overlay deactivates the main window -- so
	// this is checked before anything is decided.
	if (regionTool_->isInteracting())
		return;

	// The policy itself lives in regionOverlayState(), so it can be stated once
	// and tested without a window: whether the overlay is on screen at all, and
	// whether it takes the mouse. It used to disappear the moment focus left
	// Harpia, which made the one job it exists for -- lining the frame up
	// against the app you are about to record -- impossible.
	const bool focused = isActiveWindow() || regionTool_->isActiveWindow();
	const RegionOverlayState st = regionOverlayState(captureMode_ == CaptureMode::Region,
							 recorder_.isRecording(), focused,
							 VideoEditorWindow::anyOpen());
	regionTool_->setMode(st.mode);
	// Never steals the foreground: showing an always-on-top window normally
	// activates it, which would yank focus off whatever the user just clicked
	// -- and then bounce it straight back here on the next activation change.
	regionTool_->setAttribute(Qt::WA_ShowWithoutActivating, !focused);
	regionTool_->setVisible(st.visible);
}

void MainWindow::changeEvent(QEvent *event)
{
	QMainWindow::changeEvent(event);
	if (event->type() == QEvent::ActivationChange)
		updateRegionToolVisibility();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
	// A countdown hasn't recorded anything yet — cancel it and close normally.
	if (countingDown_) {
		if (countdownOverlay_)
			countdownOverlay_->stop();
		countingDown_ = false;
	}

	if (recorder_.isRecording() || starting_ || stopping_) {
		if (!closePending_) {
			const auto btn = QMessageBox::question(
				this, QStringLiteral("Recording in progress"),
				QStringLiteral("A recording is still in progress.\n\n"
					       "Stop the recording and close?"),
				QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
			if (btn != QMessageBox::Yes) {
				event->ignore();
				return;
			}
			closePending_ = true;
			if (!stopping_)
				beginStop();
			statusBar()->showMessage(QStringLiteral("Stopping the recording before closing…"));
		}
		// Wait for the async stop + finalize; the finalize/remux completion
		// paths call close() again once nothing is in flight.
		event->ignore();
		return;
	}

	if (remuxActive_) {
		// The recording stopped but its MP4 is still being finalized in the
		// background — closing now would corrupt/lose it.
		closePending_ = true;
		statusBar()->showMessage(QStringLiteral("Finishing the recording file before closing…"));
		event->ignore();
		return;
	}

	QMainWindow::closeEvent(event);
}

void MainWindow::onIdleSettingChanged()
{
	// Persist the idle setting onto the active preset so it survives restarts
	// and drives tickIdle(). First item ("Off") stores 0 = disabled.
	const Preset *cur = presets_.find(activePresetId_);
	if (!cur)
		return;
	Preset updated = *cur;
	updated.idleTimeoutSeconds = idleCombo_->currentData().toInt();
	if (updated.idleTimeoutSeconds != cur->idleTimeoutSeconds)
		presets_.upsert(updated);
}

// Polled rather than event-driven: the pointer spends most of this feature's
// life over OTHER applications' windows, where no Qt event of ours fires. A
// quarter second keeps the 0 s setting feeling immediate without the cost
// mattering — it is one cursor-position query.
void MainWindow::tickRegionWatch()
{
	regionWatch_.setTimeoutSeconds(activePreset().regionLeavePauseSeconds);

	// A region recording that is actually under way. Not while starting or
	// stopping, when the region and the recorder are mid-change.
	//
	// Deliberately still true while PAUSED, unlike when this used to stop the
	// recording: the pointer coming back is what resumes, so its position has to
	// keep being checked. What does not run while paused is the countdown —
	// hence the separate `armed` below.
	const bool regionRec = captureMode_ == CaptureMode::Region && currentRegion_.enabled &&
			       recorder_.isRecording() && !starting_ && !stopping_;
	const bool armed = regionRec && !recorder_.isPaused();

	bool inside = true;
	if (regionRec) {
		// Resolve the screen once per recording, not four times a second: on
		// Windows screenForActivePreset() enumerates monitors and does GDI
		// lookups to match OBS's display order to Qt's, which is far too much
		// to repeat on a poll. The monitor cannot change under a running
		// recording, so caching it while armed is safe.
		if (!regionWatchArmed_) {
			const QScreen *scr = screenForActivePreset();
			regionWatchOrigin_ = scr ? scr->geometry().topLeft() : QPoint(0, 0);
			regionWatchDpr_ = scr ? scr->devicePixelRatio() : 1.0;
			regionWatchArmed_ = true;
		}
		inside = RegionWatch::contains(currentRegion_,
					       RegionWatch::toRegionSpace(QCursor::pos(),
									  regionWatchOrigin_,
									  regionWatchDpr_));
	} else {
		regionWatchArmed_ = false;
	}

	if (regionWatch_.tick(armed, inside, QDateTime::currentMSecsSinceEpoch())) {
		if (recorder_.pause(true)) {
			webcam_.pause(true); // the companion file pauses in lockstep
			notePauseTransition(true);
			regionAutoPaused_ = true;
			updateButtons();
			Logger::instance().log(
				LogLevel::Info,
				"Auto-pause: pointer left the recording region for " +
					std::to_string(regionWatch_.timeoutSeconds()) + "s");
		}
	} else if (regionRec && recorder_.isPaused() && regionAutoPaused_ && inside) {
		// Back inside: pick up where it left off. Only when WE paused it --
		// a recording the user paused by hand stays paused, however much the
		// pointer wanders, which is the same rule the idle pause follows.
		recorder_.pause(false);
		webcam_.pause(false);
		notePauseTransition(false);
		regionAutoPaused_ = false;
		updateButtons();
		Logger::instance().log(LogLevel::Info,
				       "Auto-pause: pointer is back in the region — resuming");
	}
}

void MainWindow::onRegionLeaveSettingChanged()
{
	const Preset *cur = presets_.find(activePresetId_);
	if (!cur)
		return;
	Preset updated = *cur;
	updated.regionLeavePauseSeconds = regionLeaveCombo_->currentData().toInt();
	if (updated.regionLeavePauseSeconds != cur->regionLeavePauseSeconds)
		presets_.upsert(updated);
}

// The control only means anything for Custom Region capture, so it is hidden
// rather than disabled in the other modes — a greyed-out row invites you to
// wonder what would enable it.
void MainWindow::updateRegionLeaveVisibility()
{
	if (regionLeaveGroup_)
		regionLeaveGroup_->setVisible(captureMode_ == CaptureMode::Region &&
					      (!regionLeaveNarrow_));
}

void MainWindow::onCountdownSettingChanged()
{
	// Persist the countdown onto the active preset (read at record start).
	const Preset *cur = presets_.find(activePresetId_);
	if (!cur)
		return;
	Preset updated = *cur;
	updated.countdownSeconds = countdownCombo_->currentData().toInt();
	if (updated.countdownSeconds != cur->countdownSeconds)
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
	updated.desktopVolume = audioPanel_->desktopVolume();
	updated.micVolumes = audioPanel_->micVolumes();
	presets_.upsert(updated);
	refreshReadiness();
}

void MainWindow::onPresetChanged()
{
	const QString id = presetCombo_->currentData().toString();
	if (!id.isEmpty())
		activePresetId_ = id.toStdString();
	syncIdleControls();
	audioPanel_->load(activePreset().recordDesktopAudio, activePreset().micDeviceIds,
			  activePreset().desktopVolume, activePreset().micVolumes);

	// Switch the live capture to the new preset's display (unless recording).
	// The previous region was chosen on a possibly-different monitor, so reset
	// to full-monitor capture to avoid an out-of-bounds crop.
	if (!recorder_.isRecording()) {
		canvasSize_ = canvasForActivePreset();
		if (!appCaptureEnabled_) {
			currentRegion_ = CaptureRegion{};
			captureModeCombo_->setCurrentIndex(int(CaptureMode::Monitor));
		}
		applyLiveCapture();
	}
	hwProbeMs_ = 0; // preset changed — re-probe hardware for accurate readiness
	refreshReadiness();
	refreshWebcamRow();
	refreshDriveLink();
}

void MainWindow::syncIdleControls()
{
	const Preset &p = activePreset();

	// The display selector reflects the active preset's monitor.
	reloadMonitorCombo();

	QSignalBlocker b1(idleCombo_);
	int idx = idleCombo_->findData(p.idleTimeoutSeconds);
	if (idx < 0 && p.idleTimeoutSeconds > 0) {
		// Preset saved with a timeout outside the preset list (e.g. from an
		// older version's spinbox) — keep the value selectable.
		idleCombo_->addItem(QStringLiteral("%1 s").arg(p.idleTimeoutSeconds),
				    p.idleTimeoutSeconds);
		idx = idleCombo_->count() - 1;
	}
	idleCombo_->setCurrentIndex(idx >= 0 ? idx : 0);

	QSignalBlocker b2(regionLeaveCombo_);
	const int ri = regionLeaveCombo_->findData(p.regionLeavePauseSeconds);
	regionLeaveCombo_->setCurrentIndex(ri >= 0 ? ri : 0); // unknown value reads as Off
	updateRegionLeaveVisibility();

	// The countdown control lives on the toolbar too; keep it in sync with the
	// active preset.
	QSignalBlocker b3(countdownCombo_);
	const int ci = countdownCombo_->findData(p.countdownSeconds);
	countdownCombo_->setCurrentIndex(ci >= 0 ? ci : 0);
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
	const QVector<ClipInfo> clips = ClipLibrary::recent(presetFolders(), kRecentCount, presetFolderMap());

	// Skip the clear-and-rebuild (and the thumbnail churn it causes) when the
	// clip set is unchanged — refresh is called on every finished recording,
	// preset edit, and editor export.
	QStringList sig;
	sig.reserve(clips.size());
	for (const ClipInfo &clip : clips)
		sig << QStringLiteral("%1|%2|%3")
			       .arg(clip.filePath)
			       .arg(clip.modified.toSecsSinceEpoch())
			       .arg(clip.sizeBytes);
	if (sig == lastRecentSig_)
		return;
	lastRecentSig_ = sig;

	recentStrip_->clear();
	itemByPath_.clear();
	const QFontMetrics fm = recentStrip_->fontMetrics();
	const int captionW = recentStrip_->gridSize().width() - 12;
	for (const ClipInfo &clip : clips) {
		// Two caption lines, both always visible: the clip's name (middle-
		// elided when long) and "date · size".
		const QString name = fm.elidedText(clip.fileName, Qt::ElideMiddle, captionW);
		auto *item = new QListWidgetItem(QStringLiteral("%1\n%2 · %3")
							 .arg(name, clip.relativeAge(), clip.humanSize()));
		item->setData(kClipPathRole, clip.filePath);
		item->setToolTip(clip.fileName + QStringLiteral("\n") + clip.filePath);
		item->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);
		// Pin each card to the full grid cell so its footprint is fixed even before
		// the thumbnail loads — otherwise a late-arriving icon grows the item and
		// the icon-mode layout leaves it overlapping its neighbor.
		item->setSizeHint(recentStrip_->gridSize());

		const QImage thumb = thumbnails_.cached(clip.filePath, kStripThumb);
		if (!thumb.isNull())
			item->setIcon(cardIcon(thumb));
		else
			thumbnails_.ensure(clip.filePath, kStripThumb);

		itemByPath_.insert(clip.filePath, item);
		recentStrip_->addItem(item);
	}
}

void MainWindow::refreshClipViews()
{
	// Refresh the recent strip and, if the Clip Library window is open, its grid
	// too — so newly finished recordings and optimized (_shared) copies appear in
	// both places live, without reopening the library.
	refreshRecentList();
	if (clipWindow_)
		clipWindow_->refresh();
}

void MainWindow::onThumbnailReady(const QString &path)
{
	QListWidgetItem *item = itemByPath_.value(path, nullptr);
	if (!item)
		return;
	const QImage thumb = thumbnails_.cached(path, kStripThumb);
	if (!thumb.isNull()) {
		// Geometry is pinned (uniform sizes + fixed sizeHint), so setting the
		// icon can't change the layout — no doItemsLayout() storm needed as
		// the 12 thumbnails stream in.
		item->setIcon(cardIcon(thumb));
	}
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

	// Trim / crop / export (built-in editor) + internet-sharing copy — videos only.
	QAction *trimAct = nullptr;
	QAction *optLowAct = nullptr;
	QAction *optBalAct = nullptr;
	QAction *optHighAct = nullptr;
	if (!path.endsWith(QStringLiteral(".gif"), Qt::CaseInsensitive)) {
		menu.addSeparator();
		trimAct = menu.addAction(QStringLiteral("Trim / Crop…"));
		QMenu *opt = menu.addMenu(QStringLiteral("Optimize for sharing"));
		optLowAct = opt->addAction(QStringLiteral("Low — smallest file"));
		optBalAct = opt->addAction(QStringLiteral("Balanced (Default)"));
		optHighAct = opt->addAction(QStringLiteral("High — best quality"));
	}

	menu.addSeparator();
	QAction *renameAct = menu.addAction(QStringLiteral("Rename…"));
	QAction *deleteAct = menu.addAction(QStringLiteral("Delete"));

	QAction *chosen = menu.exec(recentStrip_->viewport()->mapToGlobal(pos));
	if (!chosen)
		return;

	if (chosen == trimAct) {
		auto *editor = new VideoEditorWindow(path, presetFolders(), this);
		if (!editor->isValid()) {
			QMessageBox::warning(this, QStringLiteral("Trim"),
					     QStringLiteral("Could not open this video for editing."));
			editor->deleteLater();
			return;
		}
		connect(editor, &VideoEditorWindow::exported, this, [this]() { refreshClipViews(); });
		editor->setAttribute(Qt::WA_DeleteOnClose);
		editor->exec();
	} else if (chosen == optLowAct) {
		ShareExportDialog::runModal(path, ShareExporter::Level::Low, this);
		refreshClipViews();
	} else if (chosen == optBalAct) {
		ShareExportDialog::runModal(path, ShareExporter::Level::Balanced, this);
		refreshClipViews();
	} else if (chosen == optHighAct) {
		ShareExportDialog::runModal(path, ShareExporter::Level::High, this);
		refreshClipViews();
	} else if (chosen == openAct) {
		QDesktopServices::openUrl(QUrl::fromLocalFile(path));
	} else if (chosen == folderAct) {
		// On Windows, reveal the file selected in Explorer; elsewhere open the
		// containing folder.
#ifdef Q_OS_WIN
		// "/select,<path>" must reach Explorer verbatim — Qt's argument quoting
		// mangles it (opens a default location), so pass native arguments.
		QProcess p;
		p.setProgram(QStringLiteral("explorer.exe"));
		p.setNativeArguments(
			QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(path)));
		if (!p.startDetached())
			QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
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

void MainWindow::notePauseTransition(bool paused)
{
	// Stamp the pause clock at the moment of the transition instead of waiting
	// for the next tickState() tick — otherwise every pause counts up to one
	// tick (~250 ms) of paused time as recorded content.
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (paused) {
		if (pauseStartMs_ == 0)
			pauseStartMs_ = now;
	} else if (pauseStartMs_ > 0) {
		pausedAccumMs_ += now - pauseStartMs_;
		pauseStartMs_ = 0;
	}
}

qint64 MainWindow::contentElapsedMs() const
{
	if (recStartMs_ == 0)
		return 0;
	qint64 paused = pausedAccumMs_;
	if (recorder_.isPaused() && pauseStartMs_ > 0)
		paused += QDateTime::currentMSecsSinceEpoch() - pauseStartMs_;
	qint64 ms = QDateTime::currentMSecsSinceEpoch() - recStartMs_ - paused;
	return ms < 0 ? 0 : ms;
}

QString MainWindow::elapsedString() const
{
	if (recStartMs_ == 0)
		return QStringLiteral("00:00:00");

	const int totalSecs = int(contentElapsedMs() / 1000);
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

	// Region overlay border color follows the state (green/red/yellow).
	if (regionTool_)
		regionTool_->setPaused(recording && paused);

	// Pause-dependent styling (pause button + monitor border color) runs every
	// tick — only re-apply the stylesheets/colors when the state actually flips.
	const int pauseUiState = !recording ? 0 : (paused ? 2 : 1);
	const bool pauseUiChanged = (pauseUiState != lastPauseUiState_);
	lastPauseUiState_ = pauseUiState;

	// The full-screen monitor border follows the same convention: it turns yellow
	// while paused and back to the preset's recording color when resumed. (It's
	// only shown during recording, so there's no idle/green state here.)
	if (pauseUiChanged && screenBorder_ && recording && captureMode_ == CaptureMode::Monitor &&
	    activePreset().showScreenBorder) {
		if (paused) {
			screenBorder_->setColor(QColor(0xd2, 0x99, 0x22)); // yellow — paused
		} else {
			QColor c(QString::fromStdString(activePreset().screenBorderColor));
			if (!c.isValid())
				c = QColor(0xe5, 0x48, 0x4d); // red — recording
			screenBorder_->setColor(c);
		}
	}

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
		primaryButton_->setText(QStringLiteral("Stop"));
		primaryButton_->setIcon(uiIcon(Glyph::Stop, 14));
		primaryButton_->setEnabled(true);
		primaryButton_->setToolTip(QString());
	} else {
		primaryButton_->setText(QStringLiteral("Record"));
		primaryButton_->setIcon(uiIcon(Glyph::Record, 14, QColor(0xe5, 0x48, 0x4d)));
		primaryButton_->setEnabled(!recordingBlocked_);
		primaryButton_->setToolTip(recordingBlocked_
						   ? QStringLiteral("Resolve the warnings above before recording")
						   : QString());
	}

	// Pause/Resume: always visible; enabled only while actively recording.
	// The moment Stop is pressed (stopping_), the button disables AND drops
	// its green Resume styling so it can't read as clickable.
	pauseButton_->setEnabled(recording && !stopping_);
	if (stopping_ && !pauseButton_->styleSheet().isEmpty()) {
		pauseButton_->setText(QStringLiteral("Pause"));
		pauseButton_->setIcon(uiIcon(Glyph::Pause, 14));
		pauseButton_->setStyleSheet(QString());
	} else if (recording && pauseUiChanged) {
		if (paused) {
			pauseButton_->setText(QStringLiteral("Resume"));
		pauseButton_->setIcon(uiIcon(Glyph::Play, 14));
			pauseButton_->setStyleSheet(QStringLiteral(
				"background:#3fb950;border:none;color:white;border-radius:10px;"));
		} else {
			pauseButton_->setText(QStringLiteral("Pause"));
		pauseButton_->setIcon(uiIcon(Glyph::Pause, 14));
			pauseButton_->setStyleSheet(QStringLiteral(
				"background:#d29922;border:none;color:white;border-radius:10px;"));
		}
	} else if (!recording && !pauseButton_->styleSheet().isEmpty()) {
		// Back to the idle look (theme's default disabled button).
		pauseButton_->setText(QStringLiteral("Pause"));
		pauseButton_->setIcon(uiIcon(Glyph::Pause, 14));
		pauseButton_->setStyleSheet(QString());
	}

	const bool locked = recording || transitioning;
	presetCombo_->setEnabled(!locked);
	editPresetButton_->setEnabled(!locked);
	newPresetButton_->setEnabled(!locked);
	// Custom Region can be combined with single-application capture, so the mode
	// selector stays enabled regardless of the focus-app selection.
	captureModeCombo_->setEnabled(!locked);
	monitorCombo_->setEnabled(!locked); // display can't change mid-file
	// Dropdown-only controls (first item = off): always visible, locked while
	// recording so the auto-pause target / camera can't change mid-file.
	appCombo_->setEnabled(!locked);
	webcamCombo_->setEnabled(!locked);
	// Locking the idle control too: switching it to Off while the recording is
	// auto-paused would strand it paused forever (tickIdle bails on timeout <= 0
	// and never resumes).
	idleCombo_->setEnabled(!locked);
	regionLeaveCombo_->setEnabled(!locked);
	countdownCombo_->setEnabled(!locked);

	updateFloatingControls();
	updateStatusChip();
}

void MainWindow::updateFloatingControls()
{
	if (!floatingControls_)
		return;
	const bool recording = recorder_.isRecording();
	const bool paused = recorder_.isPaused();
	// Present from the moment a start is kicked off until the file is finalized,
	// so it's a continuous "recording in progress" indicator.
	const bool active = recording || starting_ || stopping_;
	if (active) {
		floatingControls_->setState(paused, recording && !stopping_ && recorder_.canPause(),
					    recording && !stopping_);
		floatingControls_->showControls();
	} else {
		floatingControls_->hideControls();
	}
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
		// No pulse at rest: an idle badge shouldn't repaint 20x/sec forever.
		if (!firstIssue_.isEmpty())
			tip = firstIssue_; // non-blocking caution shown on hover
	}

	statusBadge_->setStatus(text, color, pulse);
	if (statusBadge_->toolTip() != tip)
		statusBadge_->setToolTip(tip);
}

void MainWindow::tickState()
{
	++spinPhase_; // drive the Starting…/Stopping… spinner

	// The editor opens with exec(), which spins its own event loop -- this timer
	// keeps running inside it, and nothing else here does. So the overlay's
	// "hide while the editor is up" rule is re-checked from the tick rather than
	// from either of the two places an editor can be opened. setMode() and
	// setVisible() are both no-ops when nothing has changed.
	updateRegionToolVisibility();

	// Stop watchdog: obs_output_stop is async and a stuck muxer/encoder can
	// hang it indefinitely (previously: kill via Task Manager). After 10s,
	// force-stop the output — the file may lose its tail, but the app lives.
	if (stopping_ && !forcedStop_ && stopRequestMs_ > 0 &&
	    QDateTime::currentMSecsSinceEpoch() - stopRequestMs_ > 10000) {
		forcedStop_ = true;
		blog(LOG_WARNING, "[harpia] stop timed out after 10s — forcing the output to stop");
		statusBar()->showMessage(
			QStringLiteral("Stopping took too long — the recording was force-closed "
				       "(the file may be incomplete)"),
			10000);
		recorder_.forceStop();
	}

	// The foreground query is a real OS call — only pay for it when the focus
	// auto-pause is actually armed (recording with a target app selected).
	const bool focusArmed =
		recorder_.isRecording() && appCaptureEnabled_ && !targetExe_.isEmpty();
	const uint64_t fg = focusArmed ? ForegroundWatcher::foregroundProcessId() : 0;

	// Release the webcam output once its async stop finished writing the file.
	// (reap() is internally a no-op unless a stop is pending, but the
	// obs_output_active poll is skipped entirely when idle.)
	if (webcam_.stopPending())
		webcam_.reap();

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
			if (screenBorder_)
				screenBorder_->hideBorder();
			webcam_.stop();
			focusPaused_ = false;
			targetExe_.clear();
			stopping_ = false; // finalize complete
			stopRequestMs_ = 0; // disarm the stop watchdog

			// Surface an unclean stop (disk full, write error…) instead of
			// letting it look like a normal save.
			if (recorder_.lastStopCode() != 0) {
				const QString detail = QString::fromStdString(recorder_.lastStopError());
				statusBar()->showMessage(
					QStringLiteral("Recording stopped with an error%1")
						.arg(detail.isEmpty() ? QString()
								      : QStringLiteral(": ") + detail),
					10000);
			}

			// Post-stop handling — deferred briefly so the muxer finishes
			// flushing before we read/delete files: (1) offer to discard a
			// too-short recording, then (2) remux the temp .mkv to the final
			// .mp4. Only scheduled when there's actually work to do.
			const bool needMinCheck = (lastMinSeconds_ > 0 && lastContentMs_ > 0 &&
						   lastContentMs_ < (qint64)lastMinSeconds_ * 1000);
			const bool needsRemux =
				(!lastRecordedPath_.isEmpty() && lastRecordedPath_ != lastScreenPath_);
			if (needMinCheck || needsRemux) {
				const QString recorded = lastRecordedPath_;
				const QString finalP = lastScreenPath_;
				const QString webcam = lastWebcamPath_;
				const QString markers = markersPath_;
				const int minS = needMinCheck ? lastMinSeconds_ : 0;
				const qint64 contentMs = lastContentMs_;
				QTimer::singleShot(700, this, [this, recorded, finalP, webcam, markers, minS,
							       contentMs, needsRemux]() {
					finalizeStopped(recorded, finalP, webcam, markers, contentMs, minS,
							needsRemux);
				});
			} else if (closePending_) {
				// The user asked to close mid-recording and there is no
				// finalize work — close once the muxer has flushed.
				QTimer::singleShot(750, this, [this]() { close(); });
			}
			markersPath_.clear();
		}
		timerLabel_->setText(QStringLiteral("00:00:00"));
		updateButtons();
		return;
	}

	starting_ = false; // output is now active

	tickFocus(fg); // pause/resume on target-app focus changes

	// Track pause transitions as a fallback (the pause sites stamp the clock
	// precisely via notePauseTransition; this only catches transitions that
	// happened outside those sites, without clobbering an existing stamp).
	const bool paused = recorder_.isPaused();
	if (paused && !wasPaused_ && pauseStartMs_ == 0)
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
			webcam_.pause(true); // companion file pauses in lockstep
			notePauseTransition(true);
			autoPaused_ = true;
			updateButtons();
		}
	} else if (autoPaused_ && idleSecs < timeout) {
		recorder_.pause(false);
		webcam_.pause(false);
		notePauseTransition(false);
		autoPaused_ = false;
		updateButtons();
	}
}

void MainWindow::tickFocus(uint64_t foregroundPid)
{
	// Focus-driven pause is enabled by "Record only one application"; targetExe_
	// is set (to the chosen app) only then, so this is inactive for a plain
	// monitor/region recording.
	if (!recorder_.isRecording() || !appCaptureEnabled_ || targetExe_.isEmpty())
		return;

	const uint64_t fg = foregroundPid;
	if (fg == 0)
		return; // unknown/unsupported — don't change state

	// Harpia's own windows are neutral: clicking Pause/Resume/Stop (or just
	// glancing at the timer) must not count as the target losing focus —
	// otherwise a manual Resume would instantly re-pause, since Harpia is the
	// foreground app while you're clicking its buttons.
	if (fg == ownPid_)
		return;

	// Match by executable name, so ANY window of ANY process of the selected app
	// counts as focused (child windows, dialogs, file pickers, and the extra
	// processes of multi-process apps like browsers/Electron).
	const QString fgExe = QString::fromStdString(ForegroundWatcher::foregroundExecutable());
	if (fgExe.isEmpty())
		return; // can't identify the app — don't change state
	const bool focused = (fgExe.compare(targetExe_, Qt::CaseInsensitive) == 0);

	if (!focused && !recorder_.isPaused()) {
		if (recorder_.pause(true)) {
			webcam_.pause(true); // companion file pauses in lockstep
			notePauseTransition(true);
			focusPaused_ = true;
			writeMarker(QStringLiteral("Auto Paused (Application Lost Focus)"));
			updateButtons();
		}
	} else if (focused && focusPaused_ && recorder_.isPaused()) {
		recorder_.pause(false);
		webcam_.pause(false);
		notePauseTransition(false);
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
