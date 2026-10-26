#include "ShareExportDialog.hpp"
#include "PhoneSharingDialog.hpp"

#include "UiText.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace harpia {

namespace {

QString humanEta(qint64 ms)
{
	if (ms <= 0)
		return QStringLiteral("—");
	const qint64 s = ms / 1000;
	if (s < 60)
		return QStringLiteral("%1s").arg(s);
	return QStringLiteral("%1m %2s").arg(s / 60).arg(s % 60);
}

// Reveal the file in the OS file manager (selected on Windows).
void revealInFolder(const QString &path)
{
#ifdef Q_OS_WIN
	// Pass explorer's "/select,<path>" as native args — Qt's normal argument
	// quoting mangles it, making Explorer open a default location instead.
	QProcess p;
	p.setProgram(QStringLiteral("explorer.exe"));
	p.setNativeArguments(
		QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(path)));
	if (!p.startDetached())
		QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#else
	QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

} // namespace

void ShareExportDialog::runModal(const QString &inPath, ShareExporter::Level level, QWidget *parent)
{
	const QFileInfo fi(inPath);
	// Always MP4 for maximum compatibility; sit beside the source with _shared.
	QString outPath = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() +
			  QStringLiteral("_shared.mp4");
	if (level == ShareExporter::Level::Discord) {
		const QString base = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() +
				     QStringLiteral("_discord");
		outPath = base;
		for (int suffix = 2; QFileInfo::exists(outPath); ++suffix)
			outPath = base + QStringLiteral("_%1").arg(suffix);
	}
	ShareExportDialog dlg(inPath, outPath, level, parent);
	dlg.exec();
}

ShareExportDialog::ShareExportDialog(const QString &inPath, const QString &outPath, ShareExporter::Level level,
				     QWidget *parent)
	: QDialog(parent), inPath_(inPath), outPath_(outPath), level_(level)
{
	setWindowTitle(QStringLiteral("Generate Sharing Copy"));
	setModal(true);
	setMinimumWidth(440);

	originalBytes_ = QFileInfo(inPath_).size();

	stack_ = new QStackedWidget(this);

	// ---- Progress page -------------------------------------------------
	auto *prog = new QWidget(this);
	auto *pv = new QVBoxLayout(prog);
	pv->setContentsMargins(18, 16, 18, 16);
	pv->setSpacing(10);

	titleLabel_ = new QLabel(
		QStringLiteral("Optimizing for sharing (%1)…").arg(ShareExporter::levelName(level_)), prog);
	titleLabel_->setStyleSheet(QStringLiteral("font-weight:bold;"));
	pv->addWidget(titleLabel_);

	auto *sub = new QLabel(level_ == ShareExporter::Level::Discord
				     ? QStringLiteral("Creating numbered MP4 parts of at most 49 MB each. "
						      "The original is not changed.")
				     : QStringLiteral("Creating a compressed copy — the original is not changed."), prog);
	sub->setStyleSheet(QStringLiteral("color:#8a8f98;"));
	sub->setWordWrap(true);
	pv->addWidget(sub);

	bar_ = new QProgressBar(prog);
	bar_->setRange(0, 100);
	bar_->setValue(0);
	pv->addWidget(bar_);

	auto *statsRow = new QHBoxLayout;
	pctLabel_ = new QLabel(QStringLiteral("0%"), prog);
	pctLabel_->setStyleSheet(QStringLiteral("font-weight:bold;"));
	speedLabel_ = new QLabel(QStringLiteral("Speed: —"), prog);
	etaLabel_ = new QLabel(QStringLiteral("ETA: —"), prog);
	sizeLabel_ = new QLabel(QStringLiteral("Output: —"), prog);
	for (QLabel *l : {speedLabel_, etaLabel_, sizeLabel_})
		l->setStyleSheet(QStringLiteral("color:#c9ccd1;"));
	statsRow->addWidget(pctLabel_);
	statsRow->addStretch(1);
	statsRow->addWidget(speedLabel_);
	statsRow->addSpacing(12);
	statsRow->addWidget(etaLabel_);
	pv->addLayout(statsRow);
	pv->addWidget(sizeLabel_);

	auto *pbtns = new QHBoxLayout;
	pbtns->addStretch(1);
	cancelBtn_ = new QPushButton(QStringLiteral("Cancel"), prog);
	pbtns->addWidget(cancelBtn_);
	pv->addLayout(pbtns);
	connect(cancelBtn_, &QPushButton::clicked, this, &ShareExportDialog::onCancel);

	stack_->addWidget(prog);

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->addWidget(stack_);

	exporter_ = new ShareExporter(this);
	connect(exporter_, &ShareExporter::progress, this, &ShareExportDialog::onProgress);
	connect(exporter_, &ShareExporter::finished, this, &ShareExportDialog::onFinished);
	connect(exporter_, &ShareExporter::statusChanged, titleLabel_, &QLabel::setText);

	// Kick off after the dialog is shown.
	QMetaObject::invokeMethod(
		this, [this]() { startEncoding(); }, Qt::QueuedConnection);
}

ShareExportDialog::~ShareExportDialog()
{
	joinWorker();
}

void ShareExportDialog::joinWorker()
{
	if (worker_.joinable()) {
		if (exporter_)
			exporter_->cancel();
		worker_.join();
	}
}

void ShareExportDialog::startEncoding()
{
	worker_ = std::thread([this]() {
		exporter_->run(inPath_, outPath_, ShareExporter::optionsFor(level_));
	});
}

void ShareExportDialog::onProgress(int percent, double speed, qint64 etaMs, qint64 outBytes)
{
	if (done_)
		return;
	bar_->setValue(percent);
	pctLabel_->setText(QStringLiteral("%1%").arg(percent));
	speedLabel_->setText(QStringLiteral("Speed: %1x").arg(speed, 0, 'f', 1));
	etaLabel_->setText(QStringLiteral("ETA: %1").arg(humanEta(etaMs)));
	sizeLabel_->setText(QStringLiteral("Output so far: %1").arg(humanFileSize(outBytes)));
}

void ShareExportDialog::onFinished(bool ok, bool canceled, const QString &error)
{
	done_ = true;
	joinWorker();

	if (canceled || !ok) {
		// Never leave a partial/failed file behind.
		if (level_ != ShareExporter::Level::Discord)
			QFile::remove(outPath_);
		if (!canceled) {
			titleLabel_->setText(QStringLiteral("Could not create the sharing copy"));
			titleLabel_->setStyleSheet(QStringLiteral("font-weight:bold; color:#e5484d;"));
			sizeLabel_->setText(error.isEmpty() ? QStringLiteral("Encoding failed.") : error);
			cancelBtn_->setText(QStringLiteral("Close"));
			cancelBtn_->setEnabled(true);
			disconnect(cancelBtn_, nullptr, this, nullptr);
			connect(cancelBtn_, &QPushButton::clicked, this, &QDialog::reject);
			return;
		}
		reject();
		return;
	}

	outputFiles_ = exporter_->outputFiles();
	finalBytes_ = 0;
	for (const QString &file : outputFiles_)
		finalBytes_ += QFileInfo(file).size();
	buildSummary();
}

void ShareExportDialog::buildSummary()
{
	bar_->setValue(100);

	auto *page = new QWidget(this);
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(18, 16, 18, 16);
	v->setSpacing(10);

	const bool discord = level_ == ShareExporter::Level::Discord;
	auto *title = new QLabel(
		discord ? QStringLiteral("Discord parts ready") : QStringLiteral("Sharing copy ready"), page);
	title->setStyleSheet(QStringLiteral("font-weight:bold; color:#3fb950;"));
	v->addWidget(title);

	const double savedPct =
		originalBytes_ > 0 ? (1.0 - double(finalBytes_) / double(originalBytes_)) * 100.0 : 0.0;
	summaryLabel_ = new QLabel(page);
	summaryLabel_->setTextFormat(Qt::RichText);
	summaryLabel_->setText(
		(discord ? QStringLiteral("Original: <b>%1</b><br>All parts: <b>%2</b><br>Space saved: <b>%3%</b>")
			 : QStringLiteral("Original: <b>%1</b><br>Optimized: <b>%2</b><br>Space saved: <b>%3%</b>"))
			.arg(humanFileSize(originalBytes_), humanFileSize(finalBytes_))
			.arg(savedPct, 0, 'f', 0));
	v->addWidget(summaryLabel_);
	if (discord) {
		auto *partsLabel = new QLabel(
			QStringLiteral("%1 numbered MP4 parts, each at most 49 MB. Send them in order; "
				       "the last part contains the remaining video.").arg(outputFiles_.size()), page);
		partsLabel->setWordWrap(true);
		v->addWidget(partsLabel);
	}

	auto *pathLabel = new QLabel(QDir::toNativeSeparators(outPath_), page);
	pathLabel->setStyleSheet(QStringLiteral("color:#8a8f98;"));
	pathLabel->setWordWrap(true);
	pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	v->addWidget(pathLabel);

	auto *sendRow = new QHBoxLayout;
	auto *sendPhone = new QPushButton(QStringLiteral("Send to phone…"), page);
	sendPhone->setObjectName(QStringLiteral("sendToPhoneButton"));
	sendPhone->setEnabled(!outputFiles_.isEmpty());
	sendPhone->setToolTip(discord ? QStringLiteral("Choose how to send all numbered parts to your phone.")
				    : QStringLiteral("Choose how to send the sharing copy to your phone."));
	sendRow->addWidget(sendPhone);
	sendRow->addStretch(1);
	v->addLayout(sendRow);
	connect(sendPhone, &QPushButton::clicked, this,
		[this]() { PhoneSharingDialog::runModal(outputFiles_, this); });

	auto *btns = new QHBoxLayout;
	auto *openFolder = new QPushButton(QStringLiteral("Open Folder"), page);
	auto *copyFile = new QPushButton(
		discord ? QStringLiteral("Copy All Parts") : QStringLiteral("Copy File"), page);
	copyFile->setToolTip(discord ? QStringLiteral("Copy all numbered parts so you can paste them into Discord.")
				     : QStringLiteral("Copy the file so you can paste it into WhatsApp, Explorer, etc."));
	auto *copyPath = new QPushButton(
		discord ? QStringLiteral("Copy Folder Path") : QStringLiteral("Copy Path"), page);
	auto *closeBtn = new QPushButton(QStringLiteral("Close"), page);
	btns->addWidget(openFolder);
	btns->addWidget(copyFile);
	btns->addWidget(copyPath);
	btns->addStretch(1);
	btns->addWidget(closeBtn);
	v->addLayout(btns);

	connect(openFolder, &QPushButton::clicked, this, [this, discord]() {
		if (discord)
			QDesktopServices::openUrl(QUrl::fromLocalFile(outPath_));
		else
			revealInFolder(outPath_);
	});
	connect(copyFile, &QPushButton::clicked, this, [this]() {
		auto *mime = new QMimeData();
		QList<QUrl> urls;
		for (const QString &file : outputFiles_)
			urls.append(QUrl::fromLocalFile(file));
		mime->setUrls(urls);
		QApplication::clipboard()->setMimeData(mime);
	});
	connect(copyPath, &QPushButton::clicked, this,
		[this]() { QApplication::clipboard()->setText(QDir::toNativeSeparators(outPath_)); });
	connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

	stack_->addWidget(page);
	stack_->setCurrentWidget(page);
}

void ShareExportDialog::onCancel()
{
	if (done_ || canceling_)
		return;
	canceling_ = true;
	cancelBtn_->setEnabled(false);
	cancelBtn_->setText(QStringLiteral("Canceling…"));
	titleLabel_->setText(QStringLiteral("Canceling…"));
	if (exporter_)
		exporter_->cancel();
}

void ShareExportDialog::closeEvent(QCloseEvent *event)
{
	// Closing the window mid-encode cancels rather than orphaning the worker.
	if (!done_ && worker_.joinable()) {
		onCancel();
		event->ignore();
		return;
	}
	QDialog::closeEvent(event);
}

} // namespace harpia
