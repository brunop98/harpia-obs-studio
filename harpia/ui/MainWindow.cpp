#include "MainWindow.hpp"

#include "ClipLibraryWindow.hpp"
#include "PresetEditorDialog.hpp"
#include "RecentListWidget.hpp"
#include "RegionOverlay.hpp"
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
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSpinBox>
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
	setWindowTitle(QStringLiteral("Harpia Recorder"));

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

	newPresetButton_ = new QPushButton(QStringLiteral("+ New Preset"), central);
	toolbar->addWidget(newPresetButton_);

	toolbar->addStretch(1);

	regionButton_ = new QPushButton(QStringLiteral("Region…"), central);
	fullScreenButton_ = new QPushButton(QStringLiteral("Full screen"), central);
	toolbar->addWidget(regionButton_);
	toolbar->addWidget(fullScreenButton_);

	idleToggle_ = new QCheckBox(QStringLiteral("Only record while using the computer"), central);
	toolbar->addWidget(idleToggle_);
	idleSpin_ = new QSpinBox(central);
	idleSpin_->setRange(1, 3600);
	idleSpin_->setSuffix(QStringLiteral(" s"));
	idleSpin_->setValue(10);
	idleSpin_->setMaximumWidth(80);
	toolbar->addWidget(idleSpin_);

	openFolderButton_ = new QPushButton(QStringLiteral("Open Preset Folder"), central);
	toolbar->addWidget(openFolderButton_);

	root->addLayout(toolbar);

	// ---- Center controls ------------------------------------------------
	root->addStretch(1);

	auto *controls = new QHBoxLayout;
	controls->setSpacing(18);
	controls->addStretch(1);

	primaryButton_ = new QPushButton(QStringLiteral("●  Record"), central);
	primaryButton_->setObjectName(QStringLiteral("primaryButton"));
	primaryButton_->setMinimumSize(240, 96);
	QFont bigFont = primaryButton_->font();
	bigFont.setPointSize(bigFont.pointSize() + 8);
	bigFont.setBold(true);
	primaryButton_->setFont(bigFont);
	controls->addWidget(primaryButton_);

	stopButton_ = new QPushButton(QStringLiteral("■  Stop"), central);
	stopButton_->setObjectName(QStringLiteral("stopButton"));
	stopButton_->setMinimumSize(130, 96);
	stopButton_->setFont(bigFont);
	controls->addWidget(stopButton_);

	controls->addSpacing(24);

	timerLabel_ = new QLabel(QStringLiteral("00:00:00"), central);
	timerLabel_->setObjectName(QStringLiteral("timerLabel"));
	QFont timerFont(QStringLiteral("monospace"));
	timerFont.setStyleHint(QFont::Monospace);
	timerFont.setPointSize(bigFont.pointSize() + 10);
	timerLabel_->setFont(timerFont);
	controls->addWidget(timerLabel_);

	controls->addStretch(1);
	root->addLayout(controls);

	root->addStretch(1);

	// ---- Recent recordings strip ---------------------------------------
	auto *stripHeader = new QHBoxLayout;
	stripHeader->addWidget(new QLabel(QStringLiteral("Recent recordings"), central));
	stripHeader->addStretch(1);
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

	// Wide, PowerRec-like proportions (~10:4).
	resize(940, 376);

	// ---- Wiring ---------------------------------------------------------
	connect(primaryButton_, &QPushButton::clicked, this, &MainWindow::onPrimaryButton);
	connect(stopButton_, &QPushButton::clicked, this, &MainWindow::onStopButton);
	connect(newPresetButton_, &QPushButton::clicked, this, &MainWindow::onNewPreset);
	connect(openFolderButton_, &QPushButton::clicked, this, &MainWindow::onOpenPresetFolder);
	connect(libraryButton_, &QPushButton::clicked, this, &MainWindow::onOpenClipLibrary);
	connect(regionButton_, &QPushButton::clicked, this, &MainWindow::onSelectRegion);
	connect(fullScreenButton_, &QPushButton::clicked, this, &MainWindow::onClearRegion);
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

	recorder_.onFinished = [this](const std::string &) {
		QMetaObject::invokeMethod(this, "refreshRecentList", Qt::QueuedConnection);
	};

	regionOverlay_ = std::make_unique<RegionOverlay>();

	applyDarkTheme();

	// Bring up the capture source now so the first record is instant.
	obs_.resetVideo(canvasSize_.width(), canvasSize_.height(), activePreset().fps);
	capture_.startCapture(activePreset().monitorIndex);

	stateTimer_ = new QTimer(this);
	stateTimer_->setInterval(250);
	connect(stateTimer_, &QTimer::timeout, this, &MainWindow::tickState);
	stateTimer_->start();

	idleTimer_ = new QTimer(this);
	idleTimer_->setInterval(1000);
	connect(idleTimer_, &QTimer::timeout, this, &MainWindow::tickIdle);
	idleTimer_->start();

	reloadPresetCombo();
	syncIdleControls();
	refreshRecentList();
	updateButtons();
}

MainWindow::~MainWindow()
{
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
	const Preset &preset = activePreset();

	// Size the canvas to the display this preset captures.
	canvasSize_ = canvasForActivePreset();
	uint32_t baseW = canvasSize_.width();
	uint32_t baseH = canvasSize_.height();
	uint32_t outW = baseW, outH = baseH;
	if (preset.resolutionMode != ResolutionMode::Native && preset.width > 0 && preset.height > 0) {
		outW = (uint32_t)preset.width;
		outH = (uint32_t)preset.height;
	}

	// GIF is meant for short clips — cap the frame rate so files stay small.
	int fps = preset.fps;
	if (preset.format == RecordingFormat::GIF)
		fps = qMin(fps, 15);

	obs_.resetVideo(baseW, baseH, fps, outW, outH);

	// Ensure we're capturing this preset's display, then re-apply any region.
	capture_.startCapture(preset.monitorIndex);
	capture_.setRegion(currentRegion_);

	const QString path = buildOutputPath(preset);
	if (!recorder_.start(preset, path.toStdString())) {
		timerLabel_->setText(QStringLiteral("error"));
		return;
	}

	recStartMs_ = QDateTime::currentMSecsSinceEpoch();
	pausedAccumMs_ = 0;
	pauseStartMs_ = 0;
	wasPaused_ = false;
	autoPaused_ = false;
	updateButtons();
}

void MainWindow::onPrimaryButton()
{
	if (!recorder_.isRecording()) {
		startRecording();
		return;
	}
	recorder_.togglePause();
	autoPaused_ = false; // manual action overrides the idle state machine
	updateButtons();
}

void MainWindow::onStopButton()
{
	if (recorder_.isRecording())
		recorder_.stop();
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
		PresetEditorDialog dlg(*cur, this);
		if (dlg.exec() == QDialog::Accepted) {
			presets_.upsert(dlg.result());
			reloadPresetCombo();
			syncIdleControls();
			refreshRecentList();
		}
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

void MainWindow::onSelectRegion()
{
	QScreen *screen = screenForActivePreset();
	RegionSelectDialog dlg(screen, this);
	if (dlg.exec() == QDialog::Accepted) {
		currentRegion_ = dlg.region();
		capture_.setRegion(currentRegion_);
		regionOverlay_->setRegion(currentRegion_, screen);
		updateButtons();
	}
}

void MainWindow::onClearRegion()
{
	currentRegion_ = CaptureRegion{};
	capture_.setRegion(currentRegion_);
	regionOverlay_->setRegion(currentRegion_, screenForActivePreset());
	updateButtons();
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

void MainWindow::onPresetChanged()
{
	const QString id = presetCombo_->currentData().toString();
	if (!id.isEmpty())
		activePresetId_ = id.toStdString();
	syncIdleControls();

	// Switch the live capture to the new preset's display (unless recording).
	// The previous region was chosen on a possibly-different monitor, so reset
	// to full-screen to avoid an out-of-bounds crop.
	if (!recorder_.isRecording()) {
		canvasSize_ = canvasForActivePreset();
		currentRegion_ = CaptureRegion{};
		regionOverlay_->setRegion(currentRegion_, screenForActivePreset());
		capture_.startCapture(activePreset().monitorIndex);
		capture_.setRegion(currentRegion_);
	}
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
	QAction *folderAct = menu.addAction(QStringLiteral("Open containing folder"));
	QAction *copyAct = menu.addAction(QStringLiteral("Copy file path"));

	QAction *chosen = menu.exec(recentStrip_->viewport()->mapToGlobal(pos));
	if (chosen == openAct) {
		QDesktopServices::openUrl(QUrl::fromLocalFile(path));
	} else if (chosen == folderAct) {
		QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
	} else if (chosen == copyAct) {
		auto *mime = new QMimeData();
		mime->setText(path);
		mime->setUrls({QUrl::fromLocalFile(path)});
		QApplication::clipboard()->setMimeData(mime);
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

	if (!recording) {
		primaryButton_->setText(QStringLiteral("●  Record"));
		primaryButton_->setStyleSheet(QString());
	} else if (paused) {
		primaryButton_->setText(QStringLiteral("▶  Resume"));
		primaryButton_->setStyleSheet(
			QStringLiteral("background:#3fb950;border:none;color:white;border-radius:10px;"));
	} else {
		primaryButton_->setText(QStringLiteral("⏸  Pause"));
		primaryButton_->setStyleSheet(
			QStringLiteral("background:#d29922;border:none;color:white;border-radius:10px;"));
	}

	stopButton_->setEnabled(recording);
	presetCombo_->setEnabled(!recording);
	newPresetButton_->setEnabled(!recording);
	regionButton_->setEnabled(!recording);
	fullScreenButton_->setEnabled(!recording);
}

void MainWindow::tickState()
{
	if (!recorder_.isRecording()) {
		if (recStartMs_ != 0) {
			// Recording just ended — reset the timer accounting.
			recStartMs_ = 0;
			pausedAccumMs_ = 0;
			pauseStartMs_ = 0;
			wasPaused_ = false;
		}
		timerLabel_->setText(QStringLiteral("00:00:00"));
		updateButtons();
		return;
	}

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

} // namespace harpia
