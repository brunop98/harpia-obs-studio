#include "VideoEditorWindow.hpp"

#include "AudioRecorder.hpp"
#include "ClipExporter.hpp"
#include "DevPanel.hpp"
#include "EditorWidgets.hpp"
#include "ExportOptionsDialog.hpp"
#include "FrameSeeker.hpp"
#include "LevelMeter.hpp"
#include "TimelineThumbs.hpp"
#include "TrackEditor.hpp"
#include "VoiceoverMixer.hpp"
#include "VoiceoverTrack.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace harpia {

namespace {
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
	resize(900, 680);

	seeker_ = std::make_unique<FrameSeeker>();
	valid_ = seeker_->open(inPath);

	auto *root = new QVBoxLayout(this);

	canvas_ = new PreviewCanvas(this);
	root->addWidget(canvas_, 1);

	// Mode switch: Simple Trim (one range) vs Multi-Cut (assemble many cuts).
	auto *modeRow = new QHBoxLayout;
	trimModeBtn_ = new QPushButton(QStringLiteral("Simple Trim"), this);
	trimModeBtn_->setCheckable(true);
	trimModeBtn_->setChecked(true);
	trimModeBtn_->setToolTip(QStringLiteral("Trim one start/end range"));
	cutModeBtn_ = new QPushButton(QStringLiteral("Multi-Cut"), this);
	cutModeBtn_->setCheckable(true);
	cutModeBtn_->setToolTip(QStringLiteral(
		"Drag on the Source track to select the sections to keep; they are joined in order. "
		"Each cut gets its own playback speed."));
	modeRow->addWidget(trimModeBtn_);
	modeRow->addWidget(cutModeBtn_);
	modeRow->addStretch(1);
	// Developer Panel: live-tweak every timeline layout variable to find the
	// best UI configuration (editor-only tool, values are not persisted).
	auto *devBtn = new QPushButton(QStringLiteral("Dev"), this);
	devBtn->setFlat(true);
	devBtn->setToolTip(QStringLiteral(
		"Developer Panel — tweak timeline spacing, thumbnail size, padding and zoom live"));
	connect(devBtn, &QPushButton::clicked, this, [this]() {
		if (!devPanel_)
			devPanel_ = new DevPanel(timeline_, tracks_, voTrack_, canvas_, this);
		devPanel_->show();
		devPanel_->raise();
		devPanel_->activateWindow();
	});
	modeRow->addWidget(devBtn);
	modeRow->addSpacing(8);
	// Live cursor readout: the source time of the frame being previewed.
	cursorTimeLabel_ = new QLabel(QStringLiteral("0:00.000"), this);
	cursorTimeLabel_->setStyleSheet(QStringLiteral("color:#9a9fa8; font-family:monospace;"));
	cursorTimeLabel_->setToolTip(QStringLiteral("Time of the frame shown in the preview"));
	modeRow->addWidget(cursorTimeLabel_);
	root->addLayout(modeRow);

	timeline_ = new Timeline(this);
	tracks_ = new TrackEditor(this);
	stack_ = new QStackedWidget(this);
	stack_->addWidget(timeline_); // index 0 = Simple Trim
	stack_->addWidget(tracks_);   // index 1 = Multi-Cut
	root->addWidget(stack_);

	// ---- Voiceover: a narration track + its recording controls -----------
	voTrack_ = new VoiceoverTrack(this);
	root->addWidget(voTrack_);

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
	root->addLayout(voRow);

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
	root->addLayout(voMixRow);

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
	connect(voTrack_, &VoiceoverTrack::clipsChanged, this, [this]() { updateInfoLabel(); });

	// Playback + speed row: play/pause loops the trimmed region at the chosen
	// speed so you can judge the speed before exporting.
	auto *playRow = new QHBoxLayout;
	playBtn_ = new QPushButton(QStringLiteral("▶  Play"), this);
	playBtn_->setToolTip(QStringLiteral("Loop-play the trimmed section at the current speed"));
	playRow->addWidget(playBtn_);
	playRow->addSpacing(12);
	playRow->addWidget(new QLabel(QStringLiteral("Speed"), this));
	speedSlider_ = new QSlider(Qt::Horizontal, this);
	speedSlider_->setRange(0, kSpeedTicks); // exponential 0.1×..50×
	speedSlider_->setPageStep(kSpeedTicks / 20);
	speedSlider_->setValue(speedToSlider(1.0));
	speedSlider_->setMinimumWidth(180);
	speedSlider_->setToolTip(QStringLiteral("Playback speed (0.1×–50×); scaled so low speeds are easy to fine-tune"));
	playRow->addWidget(speedSlider_, 1);
	// Editable numeric speed — type an exact value.
	speedSpin_ = new QDoubleSpinBox(this);
	speedSpin_->setRange(kMinSpeed, kMaxSpeed);
	speedSpin_->setDecimals(2);
	speedSpin_->setSingleStep(0.1);
	speedSpin_->setSuffix(QStringLiteral("×"));
	speedSpin_->setValue(1.0);
	speedSpin_->setKeyboardTracking(false); // apply on Enter/focus-out, not each digit
	speedSpin_->setFixedWidth(72);
	playRow->addWidget(speedSpin_);
	speedLabel_ = new QLabel(QString(), this); // "(N cuts)" / "—" status
	speedLabel_->setMinimumWidth(56);
	speedLabel_->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	playRow->addWidget(speedLabel_);
	root->addLayout(playRow);

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
	auto *saveBtn = new QPushButton(QStringLiteral("Save…"), this);
	saveBtn->setDefault(true);
	auto *cancelBtn = new QPushButton(QStringLiteral("Close"), this);
	controls->addWidget(saveBtn);
	controls->addWidget(cancelBtn);
	root->addLayout(controls);

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

	connect(timeline_, &Timeline::scrub, this, &VideoEditorWindow::onScrub);
	connect(timeline_, &Timeline::hoverScrub, this, &VideoEditorWindow::onHoverScrub);
	connect(timeline_, &Timeline::startChanged, this, [this]() { updateVoiceoverAxis(); });
	connect(timeline_, &Timeline::endChanged, this, [this]() { updateVoiceoverAxis(); });
	connect(tracks_, &TrackEditor::scrubSource, this, &VideoEditorWindow::onScrub);
	connect(tracks_, &TrackEditor::hoverScrub, this, &VideoEditorWindow::onHoverScrub);
	connect(tracks_, &TrackEditor::segmentsChanged, this, &VideoEditorWindow::onSegmentsChanged);
	connect(tracks_, &TrackEditor::selectionChanged, this, &VideoEditorWindow::onSegmentSelected);
	connect(trimModeBtn_, &QPushButton::clicked, this, [this]() { setEditMode(false); });
	connect(cutModeBtn_, &QPushButton::clicked, this, [this]() { setEditMode(true); });
	connect(cropToggle_, &QCheckBox::toggled, this, &VideoEditorWindow::onCropToggled);
	connect(resetCrop, &QPushButton::clicked, this, [this]() { canvas_->resetCrop(); });
	connect(saveBtn, &QPushButton::clicked, this, &VideoEditorWindow::onSave);
	connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

	if (valid_) {
		canvas_->setVideoSize(seeker_->width(), seeker_->height());
		timeline_->setDuration(seeker_->durationMs());
		tracks_->setDuration(seeker_->durationMs());
		// Restore any layout tweaks saved from a previous Developer Panel session.
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
		// Filmstrip thumbnails decode in the background and stream in.
		stripThumbs_ = new TimelineThumbs(this);
		connect(stripThumbs_, &TimelineThumbs::updated, this, [this]() {
			timeline_->setThumbs(stripThumbs_->thumbs());
			tracks_->setThumbs(stripThumbs_->thumbs());
		});
		stripThumbs_->start(inPath_, 60, 128, 72);
		baseInfo_ = QStringLiteral("%1 × %2   %3s")
				    .arg(seeker_->width())
				    .arg(seeker_->height())
				    .arg(seeker_->durationMs() / 1000.0, 0, 'f', 1);
		updateInfoLabel();
		updateVoiceoverAxis();
		showFrame(0);
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

void VideoEditorWindow::onSegmentsChanged()
{
	stopPlayback();
	playSeg_ = -1;
	updateInfoLabel();
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
}

VideoEditorWindow::~VideoEditorWindow()
{
	stopPlayback();
	if (voRecorder_ && voRecorder_->isRecording())
		voRecorder_->stop();
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

void VideoEditorWindow::startPlayback()
{
	if (!valid_)
		return;
	if (multiCut()) {
		if (tracks_->segments().isEmpty())
			return; // nothing to assemble yet
		playing_ = true;
		playBtn_->setText(QStringLiteral("⏸  Pause"));
		playAnchorMs_ = 0; // output-time
		playSeg_ = -1;     // force the first segment seek
		playClock_.restart();
		playTimer_->start();
		return;
	}
	playing_ = true;
	playBtn_->setText(QStringLiteral("⏸  Pause"));
	playAnchorMs_ = timeline_->start();
	seeker_->seekTo(playAnchorMs_);
	playClock_.restart();
	playTimer_->start();
}

void VideoEditorWindow::stopPlayback()
{
	playing_ = false;
	playBtn_->setText(QStringLiteral("▶  Play"));
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
		if (seg != playSeg_) {
			seeker_->seekTo(segs[seg].srcStartMs);
			playSeg_ = seg;
		}
		// Decode forward to the target source time; only the shown frame is
		// converted (skipped catch-up frames stay in YUV — see nextFrameAt).
		const QImage img = seeker_->nextFrameAt(srcTarget, nullptr, 1280, 720, 240);
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
		updateVoiceoverAxis();
		return;
	}

	// Re-anchor the playback clock so the speed change is seamless.
	if (playing_) {
		playAnchorMs_ = playAnchorMs_ + qint64(playClock_.elapsed() * speed_);
		playClock_.restart();
	}
	speed_ = value;
	updateVoiceoverAxis();
}

void VideoEditorWindow::onPreviewTick()
{
	if (pendingMs_ >= 0)
		showFrame(pendingMs_);
	pendingMs_ = -1;
}

void VideoEditorWindow::showFrame(qint64 ms)
{
	if (!valid_)
		return;
	const QImage img = seeker_->frameAt(ms, 1280, 720);
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
