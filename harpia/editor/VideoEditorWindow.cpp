#include "VideoEditorWindow.hpp"

#include "ClipExporter.hpp"
#include "EditorWidgets.hpp"
#include "ExportOptionsDialog.hpp"
#include "FrameSeeker.hpp"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace harpia {

namespace {
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
	setWindowTitle(QStringLiteral("Trim / Crop — %1").arg(QFileInfo(inPath).fileName()));
	resize(900, 640);

	seeker_ = std::make_unique<FrameSeeker>();
	valid_ = seeker_->open(inPath);

	auto *root = new QVBoxLayout(this);

	canvas_ = new PreviewCanvas(this);
	root->addWidget(canvas_, 1);

	timeline_ = new Timeline(this);
	root->addWidget(timeline_);

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

	previewTimer_ = new QTimer(this);
	previewTimer_->setSingleShot(true);
	previewTimer_->setInterval(20);
	connect(previewTimer_, &QTimer::timeout, this, &VideoEditorWindow::onPreviewTick);

	connect(timeline_, &Timeline::scrub, this, &VideoEditorWindow::onScrub);
	connect(cropToggle_, &QCheckBox::toggled, this, &VideoEditorWindow::onCropToggled);
	connect(resetCrop, &QPushButton::clicked, this, [this]() { canvas_->resetCrop(); });
	connect(saveBtn, &QPushButton::clicked, this, &VideoEditorWindow::onSave);
	connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

	if (valid_) {
		canvas_->setVideoSize(seeker_->width(), seeker_->height());
		timeline_->setDuration(seeker_->durationMs());
		infoLabel_->setText(QStringLiteral("%1 × %2   %3s")
					    .arg(seeker_->width())
					    .arg(seeker_->height())
					    .arg(seeker_->durationMs() / 1000.0, 0, 'f', 1));
		showFrame(0);
	} else {
		infoLabel_->setText(QStringLiteral("Could not open this video."));
		saveBtn->setEnabled(false);
	}
}

VideoEditorWindow::~VideoEditorWindow()
{
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
	pendingMs_ = ms;
	if (!previewTimer_->isActive())
		previewTimer_->start();
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
	if (!valid_)
		return;

	const QString base = QFileInfo(inPath_).completeBaseName() + QStringLiteral("_clip");
	ExportOptionsDialog dlg(base, this);
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
	o.gifWidth = dlg.gifWidth();
	o.videoCrf = dlg.videoCrf();
	o.keepAudio = dlg.keepAudio();

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
	QPushButton *openBtn = box.addButton(QStringLiteral("Open Folder"), QMessageBox::ActionRole);
	box.addButton(QStringLiteral("Done"), QMessageBox::AcceptRole);
	box.exec();
	if (box.clickedButton() == openBtn)
		revealInFolder(outPath_);

	accept(); // close the editor
}

} // namespace harpia
