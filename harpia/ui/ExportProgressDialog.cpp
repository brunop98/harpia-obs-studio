#include "ui/ExportProgressDialog.hpp"
#include "ui/UiText.hpp"

#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace harpia {
namespace {
QString durationText(qint64 milliseconds)
{
	const qint64 seconds = std::max<qint64>(0, milliseconds) / 1000;
	if (seconds >= 3600)
		return QStringLiteral("%1:%2:%3")
			.arg(seconds / 3600)
			.arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
			.arg(seconds % 60, 2, 10, QLatin1Char('0'));
	return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QLabel *plainLabel(const QString &text, const char *name, QWidget *parent)
{
	auto *label = new QLabel(text, parent);
	label->setTextFormat(Qt::PlainText);
	label->setObjectName(QString::fromLatin1(name));
	return label;
}

QLineEdit *pathField(const char *name, QWidget *parent)
{
	auto *edit = new QLineEdit(parent);
	edit->setObjectName(QString::fromLatin1(name));
	edit->setReadOnly(true);
	edit->setFrame(false);
	edit->setMinimumWidth(0);
	edit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	return edit;
}
} // namespace

ExportProgressDialog::ExportProgressDialog(const QString &outputPath, const QString &settingsSummary,
					 QWidget *parent)
	: QDialog(parent)
{
	setObjectName(QStringLiteral("exportProgressDialog"));
	setWindowTitle(QStringLiteral("Exporting video"));
	setWindowModality(Qt::WindowModal);
	setMinimumWidth(420);
	resize(540, 340);
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(20, 18, 20, 18);
	layout->setSpacing(12);

	filename_ = pathField("exportFilename", this);
	QFont titleFont = filename_->font();
	titleFont.setBold(true);
	if (titleFont.pointSizeF() > 0)
		titleFont.setPointSizeF(titleFont.pointSizeF() + 2);
	else
		titleFont.setPixelSize(titleFont.pixelSize() + 2);
	filename_->setFont(titleFont);
	layout->addWidget(filename_);

	auto *destinationRow = new QHBoxLayout;
	destinationRow->setSpacing(8);
	destinationRow->addWidget(plainLabel(QStringLiteral("Destination"), "exportDestinationCaption", this));
	destination_ = pathField("exportDestination", this);
	destinationRow->addWidget(destination_, 1);
	layout->addLayout(destinationRow);

	auto *settings = plainLabel(settingsSummary, "exportSettings", this);
	settings->setWordWrap(true);
	settings->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(settings);

	item_ = plainLabel(QString(), "exportItem", this);
	item_->hide();
	layout->addWidget(item_);

	stage_ = plainLabel(QStringLiteral("Preparing export…"), "exportStage", this);
	QFont stageFont = stage_->font();
	stageFont.setBold(true);
	stage_->setFont(stageFont);
	layout->addWidget(stage_);

	progress_ = new QProgressBar(this);
	progress_->setObjectName(QStringLiteral("exportProgress"));
	progress_->setRange(0, 100);
	progress_->setValue(0);
	progress_->setMinimumHeight(22);
	layout->addWidget(progress_);

	auto *details = new QGridLayout;
	details->setHorizontalSpacing(18);
	details->setVerticalSpacing(8);
	details->setColumnStretch(1, 1);
	details->setColumnStretch(3, 1);
	auto addDetail = [&](int row, int column, const QString &caption, const char *name) {
		details->addWidget(plainLabel(caption, "exportDetailCaption", this), row, column);
		auto *value = plainLabel(QStringLiteral("—"), name, this);
		details->addWidget(value, row, column + 1);
		return value;
	};
	elapsed_ = addDetail(0, 0, QStringLiteral("Elapsed"), "exportElapsed");
	remaining_ = addDetail(0, 2, QStringLiteral("Estimated left"), "exportRemaining");
	remaining_->setToolTip(QStringLiteral("Estimate for the current stage. It resets when the export moves to another stage."));
	frames_ = addDetail(1, 0, QStringLiteral("Frames"), "exportFrames");
	speed_ = addDetail(1, 2, QStringLiteral("Render speed"), "exportSpeed");
	details->addWidget(plainLabel(QStringLiteral("Written"), "exportDetailCaption", this), 2, 0);
	bytes_ = plainLabel(QString(), "exportBytes", this);
	details->addWidget(bytes_, 2, 1, 1, 3);
	layout->addLayout(details);

	cancelHint_ = plainLabel(QString(), "exportCancelHint", this);
	cancelHint_->setWordWrap(true);
	cancelHint_->hide();
	layout->addWidget(cancelHint_);

	auto *buttons = new QHBoxLayout;
	buttons->addStretch();
	cancel_ = new QPushButton(QStringLiteral("Cancel"), this);
	cancel_->setObjectName(QStringLiteral("exportCancel"));
	cancel_->setAutoDefault(false);
	connect(cancel_, &QPushButton::clicked, this, &ExportProgressDialog::requestCancel);
	buttons->addWidget(cancel_);
	layout->addLayout(buttons);

	setOutputPath(outputPath);
	bytes_->setText(QStringLiteral("Waiting for output…"));
	elapsedClock_.start();
	stageClock_.start();
	timer_ = new QTimer(this);
	timer_->setInterval(500);
	connect(timer_, &QTimer::timeout, this, &ExportProgressDialog::refreshTiming);
	timer_->start();
	refreshTiming();
}

void ExportProgressDialog::setOutputPath(const QString &path)
{
	const QFileInfo file(path);
	filename_->setText(file.fileName());
	filename_->setCursorPosition(0);
	filename_->setToolTip(QDir::toNativeSeparators(path));
	const QString folder = path.isEmpty() ? QString() : QDir::toNativeSeparators(file.absolutePath());
	destination_->setText(folder);
	destination_->setCursorPosition(0);
	destination_->setToolTip(folder);
}

void ExportProgressDialog::setStage(const QString &stage)
{
	if (finished_ || canceling_ || stage_->text() == stage)
		return;
	stage_->setText(stage);
	stageClock_.restart();
	smoothedEtaMs_ = -1;
	etaUpdatedMs_ = 0;
	// A speed measured during the previous stage does not describe audio
	// mixing or finalization. Frame totals stay visible as useful context.
	completedFrames_ = 0;
	totalFrames_ = 0;
	speed_->setText(QStringLiteral("—"));
	refreshTiming();
}

void ExportProgressDialog::setFrameProgress(qint64 completed, qint64 total)
{
	if (finished_ || canceling_)
		return;
	completedFrames_ = std::max<qint64>(0, completed);
	totalFrames_ = total > 0 ? std::max(completedFrames_, total) : 0;
	refreshFrames();
	refreshTiming();
}

void ExportProgressDialog::setProgress(int percent, qint64 etaMs, qint64 bytes)
{
	if (finished_ || canceling_)
		return;
	progress_->setValue(std::max(progress_->value(), std::clamp(percent, 0, 99)));
	setBytes(bytes);
	if (etaMs > 0) {
		const qint64 now = stageClock_.elapsed();
		const double previous = smoothedEtaMs_ - std::max<qint64>(0, now - etaUpdatedMs_);
		smoothedEtaMs_ = smoothedEtaMs_ < 0 ? etaMs : 0.7 * std::max(0.0, previous) + 0.3 * etaMs;
		etaUpdatedMs_ = now;
	} else {
		smoothedEtaMs_ = -1;
	}
	refreshTiming();
}

void ExportProgressDialog::setBytes(qint64 bytes)
{
	if (finished_ || canceling_)
		return;
	maxBytes_ = std::max(maxBytes_, bytes);
	if (maxBytes_ > 0)
		bytes_->setText(humanFileSize(maxBytes_));
}

void ExportProgressDialog::setItemProgress(int index, int count)
{
	if (finished_ || canceling_)
		return;
	item_->setText(QStringLiteral("Clip %1 of %2").arg(index + 1).arg(count));
	item_->setVisible(count > 1);
	remaining_->setToolTip(QStringLiteral("Estimated time left for the whole batch. The estimate updates as each clip is exported."));
	if (currentItem_ == index)
		return;
	currentItem_ = index;
	maxBytes_ = 0;
	completedFrames_ = totalFrames_ = 0;
	frames_->setText(QStringLiteral("—"));
	bytes_->setText(QStringLiteral("Waiting for output…"));
	speed_->setText(QStringLiteral("—"));
	stageClock_.restart();
	smoothedEtaMs_ = -1;
	refreshTiming();
}

void ExportProgressDialog::refreshFrames()
{
	if (totalFrames_ > 0)
		frames_->setText(QStringLiteral("%1 / %2").arg(QLocale().toString(completedFrames_),
							     QLocale().toString(totalFrames_)));
	else if (completedFrames_ > 0)
		frames_->setText(QLocale().toString(completedFrames_));
}

void ExportProgressDialog::refreshTiming()
{
	elapsed_->setText(durationText(elapsedClock_.elapsed()));
	if (finished_ || canceling_) {
		remaining_->setText(QStringLiteral("—"));
		return;
	}
	const qint64 stageMs = stageClock_.elapsed();
	if (stageMs >= 1000 && completedFrames_ > 0)
		speed_->setText(QStringLiteral("%1 fps").arg(QLocale().toString(completedFrames_ * 1000.0 / stageMs, 'f', 1)));
	const double left = smoothedEtaMs_ - std::max<qint64>(0, stageMs - etaUpdatedMs_);
	// Early extrapolations can be wildly wrong. An old/expired estimate is
	// equally misleading while a stage is waiting on the encoder or disk.
	if (stageMs < 3000 || progress_->value() < 2 || smoothedEtaMs_ < 0 || left <= 0 ||
	    stageMs - etaUpdatedMs_ > 5000) {
		remaining_->setText(QStringLiteral("Calculating…"));
	} else {
		remaining_->setText(QStringLiteral("About %1").arg(durationText(qint64(std::ceil(left / 1000.0)) * 1000)));
	}
}

void ExportProgressDialog::requestCancel()
{
	if (finished_ || canceling_)
		return;
	canceling_ = true;
	stage_->setText(QStringLiteral("Canceling…"));
	cancel_->setText(QStringLiteral("Canceling…"));
	cancel_->setEnabled(false);
	cancelHint_->setText(QStringLiteral("Stopping the export and removing the unfinished file…"));
	cancelHint_->show();
	speed_->setText(QStringLiteral("—"));
	refreshTiming();
	emit cancelRequested();
}

void ExportProgressDialog::finish(bool success)
{
	if (finished_)
		return;
	finished_ = true;
	timer_->stop();
	cancel_->setEnabled(false);
	if (success) {
		progress_->setValue(100);
		stage_->setText(QStringLiteral("Export complete"));
	}
	refreshTiming();
	QDialog::accept();
}

void ExportProgressDialog::reject()
{
	if (finished_)
		QDialog::reject();
	else
		requestCancel();
}

void ExportProgressDialog::closeEvent(QCloseEvent *event)
{
	if (finished_) {
		QDialog::closeEvent(event);
		return;
	}
	event->ignore();
	requestCancel();
}

} // namespace harpia
