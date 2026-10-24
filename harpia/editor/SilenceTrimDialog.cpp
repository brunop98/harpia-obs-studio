#include "SilenceTrimDialog.hpp"
#include "TimeText.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <exception>

namespace harpia {
namespace {

bool audioClip(const TimelineModel &model, int track, const TlClip &clip, bool ignoreSolo = false)
{
	const auto &lane = model.tracks[track];
	const bool audible = ignoreSolo ? !lane.muted && TlTrack::kindHasSound(lane.kind) : model.trackAudible(lane);
	return audible && clip.type == TlClip::Type::Video &&
	       clip.outDurationMs() > 0 && TimelineAudio::takeVolume(model.tracks[track], clip) > 0.0;
}

QString secondsText(qint64 milliseconds)
{
	return QLocale().toString(milliseconds / 1000.0, 'f', 2) + QStringLiteral(" s");
}

} // namespace

SilenceTrimDialog::SilenceTrimDialog(const TimelineModel &timeline,
				   const TimelineAudio::SourceLookup &pathFor,
				   const QVector<QPair<int, int>> &selected, QWidget *parent)
	: QDialog(parent), timeline_(timeline)
{
	// Resolve media paths while still on the GUI thread. A caller's lookup can
	// refer to editor-owned objects; workers must only see these copied paths.
	for (int ti = 0; ti < timeline_.tracks.size(); ++ti)
		for (const TlClip &clip : timeline_.tracks[ti].clips)
			if (audioClip(timeline_, ti, clip, true) && !sources_.contains(clip.sourceId))
				sources_.insert(clip.sourceId, pathFor ? pathFor(clip.sourceId) : QString());
	for (const auto &pair : selected)
		if (pair.first >= 0 && pair.first < timeline_.tracks.size() && pair.second >= 0 &&
		    pair.second < timeline_.tracks[pair.first].clips.size() &&
		    audioClip(timeline_, pair.first, timeline_.tracks[pair.first].clips[pair.second], true) &&
		    !sources_.value(timeline_.tracks[pair.first].clips[pair.second].sourceId).isEmpty() &&
		    !selected_.contains(pair))
			selected_.append(pair);
	buildUi(!selected_.isEmpty());
	QTimer::singleShot(0, this, &SilenceTrimDialog::startAnalysis);
}

SilenceTrimDialog::~SilenceTrimDialog()
{
	cancel_.store(true);
	if (worker_.joinable())
		worker_.join();
}

void SilenceTrimDialog::buildUi(bool canSelect)
{
	setObjectName(QStringLiteral("silenceTrimDialog"));
	setWindowTitle(QStringLiteral("Trim silence"));
	setWindowModality(Qt::WindowModal);
	resize(610, 580);
	setMinimumWidth(490);
	auto *layout = new QVBoxLayout(this);
	auto *intro = new QLabel(QStringLiteral("Review quiet pauses before cutting. Checked ranges are removed from every track together to keep the timeline in sync."), this);
	intro->setWordWrap(true);
	intro->setTextFormat(Qt::PlainText);
	layout->addWidget(intro);
	selectedOnly_ = new QCheckBox(QStringLiteral("Analyze selected clips only"), this);
	selectedOnly_->setObjectName(QStringLiteral("silenceSelectedOnly"));
	selectedOnly_->setChecked(canSelect);
	selectedOnly_->setEnabled(canSelect);
	selectedOnly_->setToolTip(QStringLiteral("Use the selected video or audio clips to find pauses. Applying cuts still closes those time ranges across every track."));
	layout->addWidget(selectedOnly_);
	auto *form = new QFormLayout;
	threshold_ = new QDoubleSpinBox(this);
	threshold_->setObjectName(QStringLiteral("silenceThreshold"));
	threshold_->setRange(-60, -15);
	threshold_->setDecimals(0);
	threshold_->setSuffix(QStringLiteral(" dB"));
	threshold_->setValue(-40);
	threshold_->setToolTip(QStringLiteral("Audio below this level is considered quiet. A lower value keeps more quiet speech and room noise."));
	form->addRow(QStringLiteral("Quiet threshold"), threshold_);
	minimum_ = new QDoubleSpinBox(this);
	minimum_->setObjectName(QStringLiteral("silenceMinPause"));
	minimum_->setRange(0.2, 3.0);
	minimum_->setDecimals(2);
	minimum_->setSingleStep(0.1);
	minimum_->setSuffix(QStringLiteral(" s"));
	minimum_->setValue(0.5);
	form->addRow(QStringLiteral("Minimum pause"), minimum_);
	keep_ = new QDoubleSpinBox(this);
	keep_->setObjectName(QStringLiteral("silenceKeepPause"));
	keep_->setRange(0.04, 1.0);
	keep_->setDecimals(2);
	keep_->setSingleStep(0.05);
	keep_->setSuffix(QStringLiteral(" s"));
	keep_->setValue(0.15);
	keep_->setToolTip(QStringLiteral("Leave this much of each pause around the cut so breaths and word endings are not clipped."));
	form->addRow(QStringLiteral("Keep around each pause"), keep_);
	layout->addLayout(form);
	edges_ = new QCheckBox(QStringLiteral("Also trim quiet beginnings and endings"), this);
	edges_->setObjectName(QStringLiteral("silenceEdges"));
	layout->addWidget(edges_);
	status_ = new QLabel(QStringLiteral("Preparing audio analysis…"), this);
	status_->setObjectName(QStringLiteral("silenceStatus"));
	status_->setTextFormat(Qt::PlainText);
	status_->setWordWrap(true);
	layout->addWidget(status_);
	progress_ = new QProgressBar(this);
	progress_->setObjectName(QStringLiteral("silenceProgress"));
	progress_->setRange(0, 0);
	progress_->setTextVisible(false);
	layout->addWidget(progress_);
	cuts_ = new QListWidget(this);
	cuts_->setObjectName(QStringLiteral("silenceCandidates"));
	cuts_->setAccessibleName(QStringLiteral("Pauses proposed for removal"));
	cuts_->setMinimumHeight(150);
	layout->addWidget(cuts_, 1);
	summary_ = new QLabel(this);
	summary_->setObjectName(QStringLiteral("silenceSummary"));
	summary_->setTextFormat(Qt::PlainText);
	summary_->setWordWrap(true);
	layout->addWidget(summary_);
	auto *buttons = new QHBoxLayout;
	analyze_ = new QPushButton(QStringLiteral("Analyze again"), this);
	analyze_->setObjectName(QStringLiteral("silenceReanalyze"));
	buttons->addWidget(analyze_);
	preview_ = new QPushButton(QStringLiteral("Preview trimmed timeline"), this);
	preview_->setObjectName(QStringLiteral("silencePreview"));
	preview_->setToolTip(QStringLiteral("Play the checked cuts in the editor preview without changing the project."));
	buttons->addWidget(preview_);
	buttons->addStretch();
	cancelButton_ = new QPushButton(QStringLiteral("Cancel"), this);
	cancelButton_->setObjectName(QStringLiteral("silenceCancel"));
	buttons->addWidget(cancelButton_);
	apply_ = new QPushButton(QStringLiteral("Apply cuts"), this);
	apply_->setObjectName(QStringLiteral("silenceApply"));
	apply_->setDefault(true);
	buttons->addWidget(apply_);
	layout->addLayout(buttons);
	optionsTimer_ = new QTimer(this);
	optionsTimer_->setSingleShot(true);
	optionsTimer_->setInterval(250);
	connect(optionsTimer_, &QTimer::timeout, this, &SilenceTrimDialog::startAnalysis);
	for (auto *spin : {threshold_, minimum_, keep_})
		connect(spin, &QDoubleSpinBox::valueChanged, this, &SilenceTrimDialog::optionsChanged);
	connect(edges_, &QCheckBox::toggled, this, &SilenceTrimDialog::optionsChanged);
	connect(selectedOnly_, &QCheckBox::toggled, this, &SilenceTrimDialog::optionsChanged);
	connect(analyze_, &QPushButton::clicked, this, &SilenceTrimDialog::startAnalysis);
	connect(cancelButton_, &QPushButton::clicked, this, &SilenceTrimDialog::reject);
	connect(apply_, &QPushButton::clicked, this, &SilenceTrimDialog::accept);
	connect(cuts_, &QListWidget::itemChanged, this, [this]() {
		stopPreview();
		updateSummary();
	});
	connect(preview_, &QPushButton::clicked, this, [this]() {
		if (previewing_) {
			stopPreview();
			updateSummary();
			return;
		}
		const auto chosen = selectedCuts();
		if (chosen.isEmpty() || !silence_trim::apply(timeline_, chosen).ok())
			return;
		previewing_ = true;
		preview_->setText(QStringLiteral("Stop preview"));
		status_->setText(QStringLiteral("Playing the trimmed timeline in the editor preview. The project has not changed."));
		emit previewRequested(chosen);
	});
	apply_->setEnabled(false);
	preview_->setEnabled(false);
}

silence_trim::Options SilenceTrimDialog::options() const
{
	silence_trim::Options result;
	result.thresholdDb = threshold_->value();
	result.minSilenceMs = qint64(std::llround(minimum_->value() * 1000));
	result.keepPauseMs = qint64(std::llround(keep_->value() * 1000));
	result.includeEdges = edges_->isChecked();
	return result;
}

TimelineModel SilenceTrimDialog::analysisModel(bool selectedOnly, QVector<TlSpan> *scopes) const
{
	TimelineModel result = timeline_;
	for (int ti = 0; ti < result.tracks.size(); ++ti) {
		if (selectedOnly)
			result.tracks[ti].solo = false; // an unselected solo lane must not silence the chosen speech
		auto &clips = result.tracks[ti].clips;
		clips.clear();
		for (int ci = 0; ci < timeline_.tracks[ti].clips.size(); ++ci) {
			const TlClip &clip = timeline_.tracks[ti].clips[ci];
			if (!audioClip(timeline_, ti, clip, selectedOnly) || sources_.value(clip.sourceId).isEmpty() ||
			    (selectedOnly && !selected_.contains({ti, ci})))
				continue;
			clips.append(clip);
			scopes->append({std::max<qint64>(0, clip.outStartMs), clip.outEndMs()});
		}
	}
	*scopes = silence_trim::normalize(*scopes, timeline_.durationMs());
	return result;
}

void SilenceTrimDialog::setBusy(bool busy)
{
	busy_ = busy;
	progress_->setVisible(busy);
	const QVector<QWidget *> controls{threshold_, minimum_, keep_, edges_, cuts_, analyze_};
	for (QWidget *control : controls)
		control->setEnabled(!busy && !closing_);
	selectedOnly_->setEnabled(!busy && !closing_ && !selected_.isEmpty());
	apply_->setEnabled(false);
	preview_->setEnabled(false);
}

void SilenceTrimDialog::optionsChanged()
{
	if (closing_)
		return;
	stopPreview();
	apply_->setEnabled(false);
	preview_->setEnabled(false);
	cuts_->setEnabled(false);
	status_->setText(QStringLiteral("Updating pause detection…"));
	optionsTimer_->start();
}

void SilenceTrimDialog::startAnalysis()
{
	if (busy_ || closing_)
		return;
	optionsTimer_->stop();
	stopPreview();
	if (worker_.joinable())
		worker_.join();
	const bool selectedOnly = selectedOnly_->isChecked();
	QVector<TlSpan> scopes;
	TimelineModel model = analysisModel(selectedOnly, &scopes);
	// A short clip late in a long project must not allocate a buffer for all
	// the preceding empty time. Only the analysis is rebased; reviewed cuts
	// are translated back to their original project positions below.
	const qint64 offset = scopes.isEmpty() ? 0 : scopes.first().fromMs;
	for (auto &track : model.tracks)
		for (auto &clip : track.clips)
			clip.outStartMs -= offset;
	const bool reuse = hasCache_ && cachedSelectedOnly_ == selectedOnly;
	const auto cached = reuse ? pcm_ : std::shared_ptr<const std::vector<float>>();
	const auto settings = options();
	const auto sources = sources_;
	cachedSelectedOnly_ = selectedOnly;
	cancel_.store(false);
	setBusy(true);
	status_->setText(reuse ? QStringLiteral("Finding quiet pauses…") : QStringLiteral("Preparing timeline audio…"));
	worker_ = std::thread([this, model, scopes, sources, cached, settings, offset]() mutable {
		Analysis result;
		try {
			result.pcm = cached;
			if (!result.pcm && !scopes.isEmpty() && !cancel_.load()) {
				auto pcm = TimelineAudio::mixToBuffer(model, [sources](int id) { return sources.value(id); }, &cancel_);
				result.pcm = std::make_shared<const std::vector<float>>(std::move(pcm));
			}
			result.hasAudio = result.pcm && !result.pcm->empty();
			if (result.hasAudio && !cancel_.load()) {
				const auto &pcm = *result.pcm;
				const qint64 frames = qint64(pcm.size() / TimelineAudio::kChannels);
				const double threshold = std::pow(10.0, settings.thresholdDb / 20.0);
				for (const TlSpan &scope : scopes) {
					if (cancel_.load())
						break;
					const qint64 first = std::clamp(TimelineAudio::frameForMs(scope.fromMs - offset), qint64(0), frames);
					const qint64 last = std::clamp(TimelineAudio::frameForMs(scope.toMs - offset), first, frames);
					if (last <= first)
						continue;
					// Each covered span is analyzed on its own: gaps outside the
					// chosen clips and their quiet edges are not internal pauses.
					for (qint64 index = first * TimelineAudio::kChannels; index < last * TimelineAudio::kChannels; ++index) {
						if ((index & 0x3fff) == 0 && cancel_.load())
							break;
						if (std::isfinite(pcm[size_t(index)]) && std::abs(pcm[size_t(index)]) > threshold) {
							result.hasSignal = true;
							break;
						}
					}
					const auto local = silence_trim::detectRange(pcm, TimelineAudio::kRate, TimelineAudio::kChannels,
									 scope.fromMs - offset, scope.toMs - offset, settings, &cancel_);
					for (const TlSpan &cut : local)
						result.cuts.append({cut.fromMs + offset, cut.toMs + offset});
				}
			}
		} catch (const std::exception &) {
			result.error = QStringLiteral("Could not prepare the audio for analysis. Try a shorter selection.");
		}
		QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable {
			completeAnalysis(std::move(result));
		}, Qt::QueuedConnection);
	});
}

void SilenceTrimDialog::completeAnalysis(Analysis result)
{
	if (worker_.joinable())
		worker_.join();
	setBusy(false);
	if (closing_) {
		QDialog::reject();
		return;
	}
	pcm_ = std::move(result.pcm);
	hasCache_ = bool(pcm_);
	candidates_ = silence_trim::normalize(result.cuts, timeline_.durationMs());
	{
		const QSignalBlocker block(cuts_);
		cuts_->clear();
		for (const TlSpan &cut : candidates_) {
			auto *item = new QListWidgetItem(QStringLiteral("%1 – %2    Remove %3")
				.arg(timeTextMs(cut.fromMs), timeTextMs(cut.toMs), secondsText(cut.durationMs())), cuts_);
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(Qt::Checked);
		}
	}
	if (!result.error.isEmpty())
		analysisMessage_ = result.error;
	else if (!result.hasAudio)
		analysisMessage_ = QStringLiteral("No usable audio was found in the chosen clips. Choose an audible video or audio clip and try again.");
	else if (!result.hasSignal)
		analysisMessage_ = QStringLiteral("The chosen audio stays below the threshold. Lower the threshold or choose another clip; nothing will be removed.");
	else if (candidates_.isEmpty())
		analysisMessage_ = QStringLiteral("No pauses match these settings. Try a shorter minimum pause or a higher quiet threshold.");
	else
		analysisMessage_ = QStringLiteral("Uncheck any pause you want to keep. Preview the result before applying the cuts.");
	updateSummary();
}

QVector<TlSpan> SilenceTrimDialog::selectedCuts() const
{
	QVector<TlSpan> selected;
	for (int index = 0; index < cuts_->count() && index < candidates_.size(); ++index)
		if (cuts_->item(index)->checkState() == Qt::Checked)
			selected.append(candidates_[index]);
	return selected;
}

void SilenceTrimDialog::updateSummary()
{
	if (busy_ || closing_)
		return;
	const auto selected = selectedCuts();
	const auto edit = silence_trim::apply(timeline_, selected);
	qint64 removed = 0;
	for (const TlSpan &cut : selected)
		removed += cut.durationMs();
	summary_->setText(QStringLiteral("%1 pause%2 selected · %3 removed · New duration %4")
		.arg(selected.size()).arg(selected.size() == 1 ? QString() : QStringLiteral("s"))
		.arg(secondsText(removed), timeTextMs(std::max<qint64>(0, timeline_.durationMs() - removed))));
	const bool allowed = !optionsTimer_->isActive() && !selected.isEmpty() && edit.ok() && !edit.model.isEmpty();
	apply_->setEnabled(allowed);
	preview_->setEnabled(allowed);
	status_->setText(edit.ok() ? analysisMessage_ : edit.error);
}

void SilenceTrimDialog::stopPreview()
{
	previewing_ = false;
	preview_->setText(QStringLiteral("Preview trimmed timeline"));
	emit previewStopped();
}

void SilenceTrimDialog::accept()
{
	if (busy_ || closing_ || optionsTimer_->isActive())
		return;
	const auto selected = selectedCuts();
	const auto edit = silence_trim::apply(timeline_, selected);
	if (selected.isEmpty() || !edit.ok() || edit.model.isEmpty())
		return;
	stopPreview();
	closing_ = true;
	QDialog::accept();
}

void SilenceTrimDialog::reject()
{
	stopPreview();
	optionsTimer_->stop();
	closing_ = true;
	if (busy_) {
		cancel_.store(true);
		setBusy(true);
		cancelButton_->setEnabled(false);
		cancelButton_->setText(QStringLiteral("Canceling…"));
		status_->setText(QStringLiteral("Canceling audio analysis…"));
		return;
	}
	QDialog::reject();
}

void SilenceTrimDialog::closeEvent(QCloseEvent *event)
{
	if (busy_) {
		event->ignore();
		reject();
		return;
	}
	QDialog::closeEvent(event);
}

} // namespace harpia
