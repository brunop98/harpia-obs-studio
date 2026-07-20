#include "VideoEditorWindow.hpp"

#include "ClipExporter.hpp"
#include "EditorWidgets.hpp"
#include "ExportOptionsDialog.hpp"
#include "FrameSeeker.hpp"
#include "TimelineThumbs.hpp"
#include "TrackEditor.hpp"

#include <QCheckBox>
#include <QDesktopServices>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <QDir>
#include <QFile>
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

	// Playback + speed row: play/pause loops the trimmed region at the chosen
	// speed so you can judge the speed before exporting.
	auto *playRow = new QHBoxLayout;
	playBtn_ = new QPushButton(QStringLiteral("▶  Play"), this);
	playBtn_->setToolTip(QStringLiteral("Loop-play the trimmed section at the current speed"));
	playRow->addWidget(playBtn_);
	playRow->addSpacing(12);
	playRow->addWidget(new QLabel(QStringLiteral("Speed"), this));
	speedSlider_ = new QSlider(Qt::Horizontal, this);
	speedSlider_->setRange(25, 400); // 0.25× .. 4.00×
	speedSlider_->setSingleStep(5);
	speedSlider_->setPageStep(25);
	speedSlider_->setValue(100); // 1.0×
	speedSlider_->setMinimumWidth(180);
	playRow->addWidget(speedSlider_, 1);
	speedLabel_ = new QLabel(QStringLiteral("1.00×"), this);
	speedLabel_->setMinimumWidth(48);
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
	connect(this, &QDialog::rejected, this, &VideoEditorWindow::stopPlayback);

	previewTimer_ = new QTimer(this);
	previewTimer_->setSingleShot(true);
	previewTimer_->setInterval(20);
	connect(previewTimer_, &QTimer::timeout, this, &VideoEditorWindow::onPreviewTick);

	connect(timeline_, &Timeline::scrub, this, &VideoEditorWindow::onScrub);
	connect(timeline_, &Timeline::hoverScrub, this, &VideoEditorWindow::onHoverScrub);
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
		showFrame(0);
	} else {
		infoLabel_->setText(QStringLiteral("Could not open this video."));
		saveBtn->setEnabled(false);
		playBtn_->setEnabled(false);
		speedSlider_->setEnabled(false);
		cropToggle_->setEnabled(false);
		trimModeBtn_->setEnabled(false);
		cutModeBtn_->setEnabled(false);
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
		{
			QSignalBlocker block(speedSlider_);
			speedSlider_->setValue(int(speed_ * 100.0));
		}
		speedLabel_->setText(QStringLiteral("%1×").arg(speed_, 0, 'f', 2));
	}
	updateInfoLabel();
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
}

void VideoEditorWindow::onSegmentSelected(int index)
{
	if (!multiCut())
		return;
	if (index >= 0 && index < tracks_->segments().size()) {
		speedSlider_->setEnabled(true);
		const double sp = tracks_->segments()[index].speed;
		{
			QSignalBlocker block(speedSlider_);
			speedSlider_->setValue(int(std::lround(sp * 100.0)));
		}
		const int n = tracks_->selectedIndices().size();
		speedLabel_->setText(n > 1 ? QStringLiteral("%1× (%2 cuts)").arg(sp, 0, 'f', 2).arg(n)
					   : QStringLiteral("%1×").arg(sp, 0, 'f', 2));
	} else {
		// No clip selected — the slider has nothing to edit.
		speedSlider_->setEnabled(false);
		speedLabel_->setText(QStringLiteral("—"));
	}
}

VideoEditorWindow::~VideoEditorWindow()
{
	stopPlayback();
	joinExport();
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
	cursorTimeLabel_->setText(previewTimeText(ts));
}

void VideoEditorWindow::onSpeedChanged(int sliderValue)
{
	const double value = sliderValue / 100.0;

	if (multiCut()) {
		// The slider edits EVERY selected cut's speed.
		const QList<int> sel = tracks_->selectedIndices();
		if (sel.isEmpty())
			return;
		for (int idx : sel)
			tracks_->setSegmentSpeed(idx, value);
		speedLabel_->setText(sel.size() > 1 ? QStringLiteral("%1× (%2 cuts)")
							      .arg(value, 0, 'f', 2)
							      .arg(sel.size())
						    : QStringLiteral("%1×").arg(value, 0, 'f', 2));
		if (playing_) {
			// Output durations shifted — re-anchor and re-map on the next tick.
			playAnchorMs_ = std::min(playAnchorMs_ + playClock_.elapsed(),
						 std::max<qint64>(0, tracks_->totalOutputMs() - 1));
			playClock_.restart();
			playSeg_ = -1;
		}
		updateInfoLabel();
		return;
	}

	// Re-anchor the playback clock so the speed change is seamless.
	if (playing_) {
		playAnchorMs_ = playAnchorMs_ + qint64(playClock_.elapsed() * speed_);
		playClock_.restart();
	}
	speed_ = value;
	speedLabel_->setText(QStringLiteral("%1×").arg(speed_, 0, 'f', 2));
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
