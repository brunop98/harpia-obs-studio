#include "MainWindow.hpp"

#include "RecentListWidget.hpp"
#include "core/ObsContext.hpp"
#include "library/ClipLibrary.hpp"
#include "model/PresetStore.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QLabel>
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

MainWindow::MainWindow(ObsContext &obs, PresetStore &presets, QWidget *parent)
	: QMainWindow(parent),
	  obs_(obs),
	  presets_(presets),
	  idle_(IdleMonitor::create())
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

	auto *central = new QWidget(this);
	auto *layout = new QVBoxLayout(central);

	primaryButton_ = new QPushButton(QStringLiteral("● Record"), central);
	primaryButton_->setMinimumHeight(72);
	QFont f = primaryButton_->font();
	f.setPointSize(f.pointSize() + 6);
	primaryButton_->setFont(f);

	stopButton_ = new QPushButton(QStringLiteral("■ Stop"), central);
	stopButton_->setMinimumHeight(40);

	statusLabel_ = new QLabel(QStringLiteral("Ready"), central);
	statusLabel_->setAlignment(Qt::AlignCenter);

	auto *recentLabel = new QLabel(QStringLiteral("Recent recordings (drag to another app)"), central);
	recentList_ = new RecentListWidget(central);
	recentList_->setMinimumHeight(220);

	libraryButton_ = new QPushButton(QStringLiteral("Open Clip Library…"), central);

	layout->addWidget(primaryButton_);
	layout->addWidget(stopButton_);
	layout->addWidget(statusLabel_);
	layout->addSpacing(8);
	layout->addWidget(recentLabel);
	layout->addWidget(recentList_, 1);
	layout->addWidget(libraryButton_);
	setCentralWidget(central);
	resize(420, 560);

	connect(primaryButton_, &QPushButton::clicked, this, &MainWindow::onPrimaryButton);
	connect(stopButton_, &QPushButton::clicked, this, &MainWindow::onStopButton);
	connect(libraryButton_, &QPushButton::clicked, this, &MainWindow::onOpenClipLibrary);

	// Double-click a recent item to open it in the system player.
	connect(recentList_, &QListWidget::itemActivated, this, [](QListWidgetItem *item) {
		const QString path = item->data(kClipPathRole).toString();
		if (!path.isEmpty())
			QDesktopServices::openUrl(QUrl::fromLocalFile(path));
	});

	// When a recording finishes (fires on a libobs thread), refresh the list on
	// the GUI thread.
	recorder_.onFinished = [this](const std::string &) {
		QMetaObject::invokeMethod(this, "refreshRecentList", Qt::QueuedConnection);
	};

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
	// MVP: use the "default" preset, or the first one. Preset selection UI is a
	// follow-up.
	if (const Preset *p = presets_.find("default"))
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

	// Reconfigure the video graph for this preset's fps/resolution (only
	// allowed while not recording).
	uint32_t baseW = canvasSize_.width();
	uint32_t baseH = canvasSize_.height();
	uint32_t outW = baseW, outH = baseH;
	if (preset.resolutionMode != ResolutionMode::Native && preset.width > 0 && preset.height > 0) {
		outW = (uint32_t)preset.width;
		outH = (uint32_t)preset.height;
	}
	obs_.resetVideo(baseW, baseH, preset.fps, outW, outH);

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
	// Recording: toggle pause/resume.
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
	// MVP: the full thumbnail-grid Clip Library window is a follow-up. For now,
	// open the primary recordings folder in the system file manager so clips are
	// reachable.
	const QStringList folders = presetFolders();
	if (!folders.isEmpty())
		QDesktopServices::openUrl(QUrl::fromLocalFile(folders.front()));
}

void MainWindow::refreshRecentList()
{
	recentList_->clear();
	const QVector<ClipInfo> clips = ClipLibrary::recent(presetFolders(), kRecentCount);
	for (const ClipInfo &clip : clips) {
		auto *item = new QListWidgetItem(
			QStringLiteral("%1\n%2 · %3").arg(clip.fileName, clip.relativeAge(), clip.humanSize()));
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
		statusLabel_->setText(QStringLiteral("Ready"));
	} else if (paused) {
		primaryButton_->setText(QStringLiteral("▶ Resume"));
		statusLabel_->setText(autoPaused_ ? QStringLiteral("Paused (idle)") : QStringLiteral("Paused"));
	} else {
		primaryButton_->setText(QStringLiteral("⏸ Pause"));
		statusLabel_->setText(QStringLiteral("Recording…"));
	}
	stopButton_->setEnabled(recording);
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
		// We paused due to idle; resume as soon as input returns.
		if (idleSecs < timeout) {
			recorder_.pause(false);
			autoPaused_ = false;
			updateButtons();
		}
	}
}

} // namespace harpia
