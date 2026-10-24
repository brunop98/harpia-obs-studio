#include "LoadingDialog.hpp"

#include <QCloseEvent>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace harpia {

LoadingDialog::LoadingDialog(const QString &title, QWidget *parent) : QDialog(parent)
{
	setObjectName(QStringLiteral("loadingDialog"));
	setWindowTitle(title);
	setWindowModality(parent && parent->isVisible() ? Qt::WindowModal : Qt::ApplicationModal);
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	setWindowFlag(Qt::WindowCloseButtonHint, false);
	setMinimumWidth(380);
	resize(460, 145);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(20, 18, 20, 18);
	layout->setSpacing(12);

	stage_ = new QLabel(QStringLiteral("Preparing…"), this);
	stage_->setObjectName(QStringLiteral("loadingStage"));
	stage_->setTextFormat(Qt::PlainText);
	stage_->setWordWrap(true);
	stage_->setMinimumWidth(0);
	stage_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	QFont stageFont = stage_->font();
	stageFont.setBold(true);
	stage_->setFont(stageFont);
	layout->addWidget(stage_);

	// A read-only text field keeps long paths copyable without making an
	// unbroken filename force the dialog beyond the screen's width.
	detail_ = new QLineEdit(this);
	detail_->setObjectName(QStringLiteral("loadingDetail"));
	detail_->setReadOnly(true);
	detail_->setFrame(false);
	detail_->setMinimumWidth(0);
	detail_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	detail_->hide();
	layout->addWidget(detail_);

	progress_ = new QProgressBar(this);
	progress_->setObjectName(QStringLiteral("loadingProgress"));
	progress_->setMinimumHeight(20);
	setIndeterminate();
	layout->addWidget(progress_);

	elapsed_ = new QLabel(this);
	elapsed_->setObjectName(QStringLiteral("loadingElapsed"));
	elapsed_->setTextFormat(Qt::PlainText);
	layout->addWidget(elapsed_);
	clock_.start();
	timer_ = new QTimer(this);
	timer_->setInterval(500);
	connect(timer_, &QTimer::timeout, this, &LoadingDialog::refreshElapsed);
	timer_->start();
	refreshElapsed();
}

void LoadingDialog::setIndeterminate()
{
	progress_->setRange(0, 0);
	progress_->setTextVisible(false);
}

void LoadingDialog::setStage(const QString &message, const QString &detail)
{
	if (finished_)
		return;
	stage_->setText(message);
	detail_->setText(detail);
	detail_->setCursorPosition(0);
	detail_->setToolTip(detail.isEmpty() ? QString() : QStringLiteral("<qt>%1</qt>").arg(detail.toHtmlEscaped()));
	detail_->setVisible(!detail.isEmpty());
	setIndeterminate();
}

void LoadingDialog::setItemProgress(int completed, int total)
{
	if (finished_)
		return;
	if (total <= 0) {
		setIndeterminate();
		return;
	}
	// Qt can repaint immediately when its range changes. Set range and value
	// together so a busy bar never briefly paints an uninitialized item count.
	progress_->setUpdatesEnabled(false);
	progress_->setRange(0, total);
	progress_->setFormat(QStringLiteral("%v of %m"));
	progress_->setTextVisible(true);
	progress_->setValue(std::clamp(completed, 0, total));
	progress_->setUpdatesEnabled(true);
}

void LoadingDialog::refreshElapsed()
{
	const qint64 seconds = std::max<qint64>(0, clock_.elapsed()) / 1000;
	const QString duration = seconds >= 3600
		? QStringLiteral("%1:%2:%3").arg(seconds / 3600)
			.arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
			.arg(seconds % 60, 2, 10, QLatin1Char('0'))
		: QStringLiteral("%1:%2").arg(seconds / 60)
			.arg(seconds % 60, 2, 10, QLatin1Char('0'));
	elapsed_->setText(QStringLiteral("Elapsed %1").arg(duration));
}

void LoadingDialog::finish()
{
	if (finished_)
		return;
	finished_ = true;
	timer_->stop();
	refreshElapsed();
	QDialog::accept();
}

void LoadingDialog::accept()
{
	if (finished_)
		QDialog::accept();
}

void LoadingDialog::reject()
{
	if (finished_)
		QDialog::reject();
}

void LoadingDialog::closeEvent(QCloseEvent *event)
{
	if (finished_)
		QDialog::closeEvent(event);
	else
		event->ignore();
}

} // namespace harpia
