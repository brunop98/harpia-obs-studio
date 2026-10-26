#include "BeatDetectionDialog.hpp"
#include "TimeText.hpp"

#include <QCloseEvent>
#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace harpia {

BeatDetectionDialog::BeatDetectionDialog(const QString &sourcePath, const TlClip &clip, QWidget *parent)
	: QDialog(parent), sourcePath_(sourcePath), clip_(clip)
{
	setObjectName(QStringLiteral("beatDetectionDialog"));
	setWindowTitle(QStringLiteral("Detect beats"));
	setWindowModality(Qt::WindowModal);
	resize(520, 480);
	setMinimumWidth(420);

	auto *layout = new QVBoxLayout(this);
	auto *intro = new QLabel(
		QStringLiteral("Find the musical pulse in this clip and add timeline markers for editing on the beat."), this);
	intro->setWordWrap(true);
	layout->addWidget(intro);
	auto *source = new QLabel(QFileInfo(sourcePath_).fileName(), this);
	source->setTextFormat(Qt::PlainText);
	source->setWordWrap(true);
	layout->addWidget(source);
	status_ = new QLabel(QStringLiteral("Preparing beat detection…"), this);
	status_->setObjectName(QStringLiteral("beatStatus"));
	status_->setTextFormat(Qt::PlainText);
	status_->setWordWrap(true);
	layout->addWidget(status_);
	progress_ = new QProgressBar(this);
	progress_->setObjectName(QStringLiteral("beatProgress"));
	progress_->setRange(0, 0);
	progress_->setTextVisible(false);
	layout->addWidget(progress_);

	auto *form = new QFormLayout;
	density_ = new QComboBox(this);
	density_->setObjectName(QStringLiteral("beatDensity"));
	density_->addItems({QStringLiteral("Every beat"), QStringLiteral("Every 2 beats"), QStringLiteral("Every 4 beats")});
	density_->setToolTip(QStringLiteral("Use fewer markers for longer cuts. This does not change the music."));
	form->addRow(QStringLiteral("Add markers on"), density_);
	layout->addLayout(form);
	candidates_ = new QListWidget(this);
	candidates_->setObjectName(QStringLiteral("beatCandidates"));
	candidates_->setAccessibleName(QStringLiteral("Beat positions on the timeline"));
	candidates_->setSelectionMode(QAbstractItemView::NoSelection);
	candidates_->setMinimumHeight(150);
	layout->addWidget(candidates_, 1);
	summary_ = new QLabel(this);
	summary_->setObjectName(QStringLiteral("beatSummary"));
	summary_->setTextFormat(Qt::PlainText);
	summary_->setWordWrap(true);
	layout->addWidget(summary_);
	auto *hint = new QLabel(
		QStringLiteral("Best with steady music. Beat positions are estimates; you can move or delete the markers later. "
			       "Existing markers are kept, and adding beats can be undone."), this);
	hint->setWordWrap(true);
	layout->addWidget(hint);
	auto *buttons = new QHBoxLayout;
	buttons->addStretch();
	cancelButton_ = new QPushButton(QStringLiteral("Cancel"), this);
	cancelButton_->setObjectName(QStringLiteral("beatCancel"));
	buttons->addWidget(cancelButton_);
	apply_ = new QPushButton(QStringLiteral("Add markers"), this);
	apply_->setObjectName(QStringLiteral("beatAddMarkers"));
	apply_->setDefault(true);
	apply_->setEnabled(false);
	buttons->addWidget(apply_);
	layout->addLayout(buttons);
	connect(density_, &QComboBox::currentIndexChanged, this, &BeatDetectionDialog::updateReview);
	connect(cancelButton_, &QPushButton::clicked, this, &BeatDetectionDialog::reject);
	connect(apply_, &QPushButton::clicked, this, &BeatDetectionDialog::accept);
	setBusy(true);
	QTimer::singleShot(0, this, &BeatDetectionDialog::startAnalysis);
}

BeatDetectionDialog::~BeatDetectionDialog()
{
	cancel_.store(true);
	if (worker_.joinable())
		worker_.join();
}

double BeatDetectionDialog::playbackSpeed() const
{
	return std::isfinite(clip_.speed) && clip_.speed > 0.01 ? clip_.speed : 1.0;
}

void BeatDetectionDialog::setBusy(bool busy)
{
	busy_ = busy;
	progress_->setVisible(busy);
	density_->setEnabled(!busy && !closing_ && !beatTimes_.isEmpty());
	apply_->setEnabled(!busy && !closing_ && !beatTimes_.isEmpty());
}

void BeatDetectionDialog::startAnalysis()
{
	if (closing_)
		return;
	if (sourcePath_.isEmpty() || clip_.srcStartMs < 0 || clip_.srcEndMs <= clip_.srcStartMs) {
		setBusy(false);
		status_->setText(QStringLiteral("This clip has no valid audio source range to analyze."));
		return;
	}
	const QString sourcePath = sourcePath_;
	const qint64 fromMs = clip_.srcStartMs;
	const qint64 toMs = clip_.srcEndMs;
	cancel_.store(false);
	status_->setText(QStringLiteral("Decoding this clip’s audio and detecting beats…"));
	worker_ = std::thread([this, sourcePath, fromMs, toMs]() {
		beat_detection::Result result;
		try {
			result = beat_detection::analyzeFile(sourcePath, fromMs, toMs, {}, &cancel_);
		} catch (const std::exception &) {
			result.error = QStringLiteral("Could not analyze this clip’s audio. Try a shorter clip or another audio file.");
		}
		QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable {
			completeAnalysis(std::move(result));
		}, Qt::QueuedConnection);
	});
}

void BeatDetectionDialog::completeAnalysis(beat_detection::Result result)
{
	if (worker_.joinable())
		worker_.join();
	setBusy(false);
	if (closing_) {
		QDialog::reject();
		return;
	}
	if (!result.error.isEmpty() || result.canceled) {
		status_->setText(result.canceled ? QStringLiteral("Beat detection was canceled.") : result.error);
		return;
	}
	// The detector reports offsets in the kept source range. Transform them
	// into the same rounded output milliseconds the timeline uses. Computing
	// in long double keeps malformed extreme positions away from integer
	// overflow, even before the ordinary clip boundaries are applied.
	const long double speed = playbackSpeed();
	const long double sourceLength = static_cast<long double>(clip_.srcEndMs) - clip_.srcStartMs;
	const long double duration = std::max(1.0L, std::round(sourceLength / speed));
	const long double end = static_cast<long double>(clip_.outStartMs) + duration;
	for (qint64 relative : result.beatsMs) {
		if (relative < 0 || static_cast<long double>(relative) >= sourceLength)
			continue;
		const long double position = static_cast<long double>(clip_.outStartMs) +
			std::round(static_cast<long double>(relative) / speed);
		if (position < 0 || position < clip_.outStartMs || position >= end ||
		    position >= static_cast<long double>(std::numeric_limits<qint64>::max()))
			continue;
		beatTimes_.append(static_cast<qint64>(position));
	}
	std::sort(beatTimes_.begin(), beatTimes_.end());
	beatTimes_.erase(std::unique(beatTimes_.begin(), beatTimes_.end()), beatTimes_.end());
	sourceBpm_ = std::isfinite(result.bpm) && result.bpm > 0 ? result.bpm : 0.0;
	status_->setText(beatTimes_.isEmpty()
		? QStringLiteral("No steady beat was found. Try music with a clearer rhythmic pulse.")
		: QStringLiteral("Review the beat positions below, then add them to the timeline."));
	updateReview();
}

QVector<qint64> BeatDetectionDialog::selectedBeatTimes() const
{
	QVector<qint64> result;
	if (busy_)
		return result;
	const int step = density_->currentIndex() == 1 ? 2 : density_->currentIndex() == 2 ? 4 : 1;
	for (qsizetype index = 0; index < beatTimes_.size(); index += step)
		result.append(beatTimes_[index]);
	return result;
}

void BeatDetectionDialog::updateReview()
{
	if (busy_ || closing_)
		return;
	const auto selected = selectedBeatTimes();
	candidates_->clear();
	constexpr qsizetype previewLimit = 500;
	const qsizetype shown = std::min(selected.size(), previewLimit);
	for (qsizetype index = 0; index < shown; ++index)
		candidates_->addItem(timeTextMs(selected[index]));
	QString summary = QStringLiteral("%1 beat%2 found · %3 marker%4 selected")
		.arg(beatTimes_.size()).arg(beatTimes_.size() == 1 ? QString() : QStringLiteral("s"))
		.arg(selected.size()).arg(selected.size() == 1 ? QString() : QStringLiteral("s"));
	const double outputBpm = sourceBpm_ * playbackSpeed();
	if (std::isfinite(outputBpm) && outputBpm > 0)
		summary += QStringLiteral(" · About %1 BPM at this clip’s speed")
			.arg(QLocale().toString(outputBpm, 'f', 1));
	if (selected.size() > shown)
		summary += QStringLiteral("\nShowing the first %1 positions. All %2 selected markers will be added.")
			.arg(shown).arg(selected.size());
	summary_->setText(summary);
	density_->setEnabled(!beatTimes_.isEmpty());
	apply_->setEnabled(!selected.isEmpty());
}

void BeatDetectionDialog::accept()
{
	if (busy_ || closing_ || selectedBeatTimes().isEmpty())
		return;
	closing_ = true;
	QDialog::accept();
}

void BeatDetectionDialog::reject()
{
	closing_ = true;
	if (busy_ && worker_.joinable()) {
		cancel_.store(true);
		setBusy(true);
		cancelButton_->setEnabled(false);
		cancelButton_->setText(QStringLiteral("Canceling…"));
		status_->setText(QStringLiteral("Canceling beat detection…"));
		return;
	}
	QDialog::reject();
}

void BeatDetectionDialog::closeEvent(QCloseEvent *event)
{
	if (busy_) {
		event->ignore();
		reject();
		return;
	}
	QDialog::closeEvent(event);
}

} // namespace harpia
