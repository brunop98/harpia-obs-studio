#include "MainWindow.hpp"

#include "ClipLibraryWindow.hpp"
#include "PresetManagerDialog.hpp"
#include "RecentListWidget.hpp"
#include "RegionOverlay.hpp"
#include "core/ObsContext.hpp"
#include "library/ClipLibrary.hpp"
#include "model/PresetStore.hpp"

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

namespace harpia {

namespace {
constexpr int kRecentCount = 10;
}

MainWindow::MainWindow(ObsContext &obs, PresetStore &presets, QString defaultFolder, QWidget *parent)
	: QMainWindow(parent),
	  obs_(obs),
	  presets_(presets),
	  idle_(IdleMonitor::create()),
	  defaultFolder_(std::move(defaultFolder))
{
	setWindowTitle(QStringLiteral("Harpia Recorder"));

	// Capture canvas = primary screen resolution (device pixels).
	if (QScreen *screen = QGuiApplication::primaryScreen()) {
		const QSize logical = screen->size();
		const qreal dpr = screen->devicePixelRatio();
		canvasSize_ = QSize(int(logical.width() * dpr), int(logical.height() * dpr));
	} else {
		canvasSize_ = QSize(1920, 1080);
	}

	if (!presets_.presets().empty())
		activePresetId_ = presets_.presets().front().id;

	auto *central = new QWidget(this);
	auto *layout = new QVBoxLayout(central);

	// Preset selector.
	auto *presetRow = new QHBoxLayout;
	presetRow->addWidget(new QLabel(QStringLiteral("Preset:"), central));
	presetCombo_ = new QComboBox(central);
	presetsButton_ = new QPushButton(QStringLiteral("Manage…"), central);
	presetRow->addWidget(presetCombo_, 1);
	presetRow->addWidget(presetsButton_);
	layout->addLayout(presetRow);

	// Primary record/pause control.
	primaryButton_ = new QPushButton(QStringLiteral("● Record"), central);
	primaryButton_->setMinimumHeight(72);
	QFont f = primaryButton_->font();
	f.setPointSize(f.pointSize() + 6);
	primaryButton_->setFont(f);
	layout->addWidget(primaryButton_);

	stopButton_ = new QPushButton(QStringLiteral("■ Stop"), central);
	stopButton_->setMinimumHeight(40);
	layout->addWidget(stopButton_);

	// Region controls.
	auto *regionRow = new QHBoxLayout;
	regionButton_ = new QPushButton(QStringLiteral("Select region…"), central);
	fullScreenButton_ = new QPushButton(QStringLiteral("Full screen"), central);
	regionRow->addWidget(regionButton_);
	regionRow->addWidget(fullScreenButton_);
	layout->addLayout(regionRow);

	statusLabel_ = new QLabel(QStringLiteral("Ready"), central);
	statusLabel_->setAlignment(Qt::AlignCenter);
	layout->addWidget(statusLabel_);

	layout->addSpacing(8);
	layout->addWidget(new QLabel(QStringLiteral("Recent recordings (drag to another app)"), central));
	recentList_ = new RecentListWidget(central);
	recentList_->setMinimumHeight(200);
	layout->addWidget(recentList_, 1);

	libraryButton_ = new QPushButton(QStringLiteral("Open Clip Library…"), central);
	layout->addWidget(libraryButton_);

	setCentralWidget(central);
	resize(440, 620);

	connect(primaryButton_, &QPushButton::clicked, this, &MainWindow::onPrimaryButton);
	connect(stopButton_, &QPushButton::clicked, this, &MainWindow::onStopButton);
	connect(libraryButton_, &QPushButton::clicked, this, &MainWindow::onOpenClipLibrary);
	connect(presetsButton_, &QPushButton::clicked, this, &MainWindow::onManagePresets);
	connect(regionButton_, &QPushButton::clicked, this, &MainWindow::onSelectRegion);
	connect(fullScreenButton_, &QPushButton::clicked, this, &MainWindow::onClearRegion);
	connect(presetCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
		const QString id = presetCombo_->currentData().toString();
		if (!id.isEmpty())
			activePresetId_ = id.toStdString();
	});
	connect(recentList_, &QListWidget::itemActivated, this, [](QListWidgetItem *item) {
		const QString path = item->data(kClipPathRole).toString();
		if (!path.isEmpty())
			QDesktopServices::openUrl(QUrl::fromLocalFile(path));
	});

	// When a recording finishes (fires on a libobs thread), refresh on the GUI thread.
	recorder_.onFinished = [this](const std::string &) {
		QMetaObject::invokeMethod(this, "refreshRecentList", Qt::QueuedConnection);
	};

	regionOverlay_ = std::make_unique<RegionOverlay>();

	// Bring up the capture source now so the first record is instant.
	obs_.resetVideo(canvasSize_.width(), canvasSize_.height(), activePreset().fps);
	capture_.startCapture(0);

	stateTimer_ = new QTimer(this);
	stateTimer_->setInterval(300);
	connect(stateTimer_, &QTimer::timeout, this, &MainWindow::tickState);
	stateTimer_->start();

	idleTimer_ = new QTimer(this);
	idleTimer_->setInterval(1000);
	connect(idleTimer_, &QTimer::timeout, this, &MainWindow::tickIdle);
	idleTimer_->start();

	reloadPresetCombo();
	refreshRecentList();
	updateButtons();
}

MainWindow::~MainWindow()
{
	if (recorder_.isRecording())
		recorder_.stop();
}

const Preset &MainWindow::activePreset() const
{
	if (const Preset *p = presets_.find(activePresetId_))
		return *p;
	return presets_.presets().front();
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

	// Reconfigure the video graph for this preset's fps/resolution (only allowed
	// while not recording).
	uint32_t baseW = canvasSize_.width();
	uint32_t baseH = canvasSize_.height();
	uint32_t outW = baseW, outH = baseH;
	if (preset.resolutionMode != ResolutionMode::Native && preset.width > 0 && preset.height > 0) {
		outW = (uint32_t)preset.width;
		outH = (uint32_t)preset.height;
	}
	obs_.resetVideo(baseW, baseH, preset.fps, outW, outH);

	// Re-apply the region after a video reset (crop is on the source).
	capture_.setRegion(currentRegion_);

	const QString path = buildOutputPath(preset);
	if (!recorder_.start(preset, path.toStdString())) {
		statusLabel_->setText(QStringLiteral("Failed to start recording (see log)"));
		return;
	}
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

void MainWindow::onOpenClipLibrary()
{
	if (!clipWindow_)
		clipWindow_ = std::make_unique<ClipLibraryWindow>(presets_);
	clipWindow_->show();
	clipWindow_->raise();
	clipWindow_->activateWindow();
	clipWindow_->refresh();
}

void MainWindow::onManagePresets()
{
	PresetManagerDialog dlg(presets_, defaultFolder_, this);
	dlg.exec();
	reloadPresetCombo();
	refreshRecentList();
}

void MainWindow::onSelectRegion()
{
	RegionSelectDialog dlg(this);
	if (dlg.exec() == QDialog::Accepted) {
		currentRegion_ = dlg.region();
		capture_.setRegion(currentRegion_);
		regionOverlay_->setRegion(currentRegion_);
	}
}

void MainWindow::onClearRegion()
{
	currentRegion_ = CaptureRegion{};
	capture_.setRegion(currentRegion_);
	regionOverlay_->setRegion(currentRegion_);
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
	recentList_->clear();
	const QVector<ClipInfo> clips = ClipLibrary::recent(presetFolders(), kRecentCount, presetFolderMap());
	for (const ClipInfo &clip : clips) {
		QString label = QStringLiteral("%1\n%2 · %3").arg(clip.fileName, clip.relativeAge(), clip.humanSize());
		if (!clip.presetName.isEmpty())
			label += QStringLiteral("  ·  %1").arg(clip.presetName);
		auto *item = new QListWidgetItem(label);
		item->setData(kClipPathRole, clip.filePath);
		item->setToolTip(clip.filePath);
		recentList_->addItem(item);
	}
}

void MainWindow::updateButtons()
{
	const bool recording = recorder_.isRecording();
	const bool paused = recorder_.isPaused();

	if (!recording) {
		primaryButton_->setText(QStringLiteral("● Record"));
		statusLabel_->setText(currentRegion_.enabled
					      ? QStringLiteral("Ready · region %1×%2")
							.arg(currentRegion_.width)
							.arg(currentRegion_.height)
					      : QStringLiteral("Ready · full screen"));
	} else if (paused) {
		primaryButton_->setText(QStringLiteral("▶ Resume"));
		statusLabel_->setText(autoPaused_ ? QStringLiteral("Paused (idle)") : QStringLiteral("Paused"));
	} else {
		primaryButton_->setText(QStringLiteral("⏸ Pause"));
		statusLabel_->setText(QStringLiteral("Recording…"));
	}
	stopButton_->setEnabled(recording);
	// Preset/resolution can't change mid-recording.
	presetCombo_->setEnabled(!recording);
	presetsButton_->setEnabled(!recording);
	regionButton_->setEnabled(!recording);
	fullScreenButton_->setEnabled(!recording);
}

void MainWindow::tickState()
{
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
		if (idleSecs >= timeout) {
			if (recorder_.pause(true)) {
				autoPaused_ = true;
				updateButtons();
			}
		}
	} else if (autoPaused_) {
		if (idleSecs < timeout) {
			recorder_.pause(false);
			autoPaused_ = false;
			updateButtons();
		}
	}
}

} // namespace harpia
