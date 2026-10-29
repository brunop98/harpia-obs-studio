#include "RandomClipCompareDialog.hpp"

#include "FrameSeeker.hpp"
#include "component/ComponentRegistry.hpp"
#include "component/ShaderComponent.hpp"
#include "script/TransformScript.hpp"
#include "timeline/TimelineCompositor.hpp"
#include "ui/UiIcons.hpp"

#include <QDialogButtonBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFrame>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWaitCondition>

#include <algorithm>
#include <atomic>
#include <vector>

namespace harpia {
namespace {

QString timeText(qint64 ms)
{
	return QStringLiteral("%1:%2.%3")
		.arg(ms / 60000)
		.arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
		.arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

QSize smallCanvas(QSize canvas, int count)
{
	if (canvas.isEmpty()) {
		canvas = QSize(1920, 1080);
	}
	const int side = count > 9 ? 160 : 256;
	const QSize fit = canvas.scaled(side, side, Qt::KeepAspectRatio);
	return QSize(std::max(2, fit.width()), std::max(2, fit.height()));
}

class ComparePreview final : public QWidget {
public:
	explicit ComparePreview(QSize aspect, QWidget *parent) : QWidget(parent), aspect_(aspect)
	{
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		setMinimumSize(100, 90);
	}
	QImage image;
	QString error;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.fillRect(rect(), QColor(11, 13, 16));
		if (!image.isNull()) {
			const QSize size = aspect_.scaled(this->size(), Qt::KeepAspectRatio);
			p.drawImage(QRect(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size),
				    image);
		}
		if (!error.isEmpty() || image.isNull()) {
			p.setPen(QColor(220, 224, 230));
			p.drawText(rect().adjusted(10, 10, -10, -10), Qt::AlignCenter | Qt::TextWordWrap,
				   error.isEmpty() ? QStringLiteral("Loading preview…") : error);
		}
	}

private:
	QSize aspect_;
};

// A separate decoder for each choice matters even when two choices use the
// same file: they may play different moments and must never fight over a seek.
class OptionFrames final : public TimelineCompositor::FrameProvider {
public:
	RandomClipCompareDialog::Source source;
	QSize bound;
	QString error;
	std::unique_ptr<FrameSeeker> seeker;
	bool attempted = false;
	QSize sourceSize(int) override { return source.originalSize; }
	QImage frameFor(int, qint64 sourceMs) override
	{
		if (!attempted) {
			attempted = true;
			seeker = std::make_unique<FrameSeeker>();
			// Keep many simultaneous tiles from each starting a CPU-sized
			// decoder thread pool. Never skip non-reference animation frames.
			if (source.missing || source.decodePath.isEmpty() ||
			    !seeker->open(source.decodePath, false, 1)) {
				error = QStringLiteral("Video unavailable. Relink this source in the editor.");
				seeker.reset();
			}
		}
		if (!seeker) {
			return {};
		}
		QImage image = seeker->frameAt(sourceMs, bound.width(), bound.height());
		error = image.isNull() ? QStringLiteral("Could not decode this video frame.") : QString();
		return image;
	}
};
} // namespace

struct RandomClipCompareRenderer::Impl {
	RandomClipCompareDialog::Input input;
	ComponentRegistry registry;
	QSize size;
	QMutex mutex;
	QWaitCondition wake;
	std::atomic<bool> cancelled{false};
	bool pending = false;
	qint64 localMs = 0;
	quint64 revision = 0;
	Batch batch;
};

RandomClipCompareRenderer::RandomClipCompareRenderer(const RandomClipCompareDialog::Input &input, QSize size)
	: d_(std::make_unique<Impl>())
{
	d_->input = input;
	// Copy on the GUI thread before asynchronous work. Registry containers
	// detach if the editor hot reloads a component while this is open.
	d_->registry = ComponentRegistry::instance();
	if (d_->input.canvas.isEmpty()) {
		d_->input.canvas = QSize(1920, 1080);
	}
	d_->size = size;
}
RandomClipCompareRenderer::~RandomClipCompareRenderer() = default;

void RandomClipCompareRenderer::request(qint64 localMs, quint64 revision)
{
	QMutexLocker lock(&d_->mutex);
	d_->localMs = localMs;
	d_->revision = revision;
	d_->pending = true;
	d_->wake.wakeOne();
}
void RandomClipCompareRenderer::cancel()
{
	QMutexLocker lock(&d_->mutex);
	d_->cancelled = true;
	d_->wake.wakeOne();
}
RandomClipCompareRenderer::Batch RandomClipCompareRenderer::takeBatch()
{
	QMutexLocker lock(&d_->mutex);
	return std::move(d_->batch);
}

void RandomClipCompareRenderer::run()
{
	struct ReleaseGl {
		~ReleaseGl() { ShaderComponents::releaseThreadResources(); }
	} releaseGl;
	// Everything with thread affinity, including component/script runtimes and
	// decoder teardown, is created and destroyed on this one rendering thread.
	TransformEvaluator evaluator;
	QString scriptError;
	for (auto it = d_->input.scripts.cbegin(); it != d_->input.scripts.cend(); ++it) {
		QString why;
		if (!evaluator.compile(it.key(), it.value(), &why)) {
			scriptError = why;
		}
	}
	for (const TlScript &script : d_->input.clip.scripts) {
		if (!script.name.isEmpty() && !d_->input.scripts.contains(script.name)) {
			scriptError = QStringLiteral("A transform script is unavailable: %1").arg(script.name);
		}
	}
	struct Option {
		TimelineModel model;
		OptionFrames frames;
	};
	std::vector<std::unique_ptr<Option>> options;
	for (int k = 0; k < d_->input.clip.clipOptions.size(); ++k) {
		auto option = std::make_unique<Option>();
		TlClip clip = d_->input.clip;
		const TlClipOption choice = clip.clipOptionAt(k);
		const qint64 length = clip.srcLenMs();
		clip.sourceId = choice.sourceId;
		clip.srcStartMs = choice.srcStartMs;
		clip.srcEndMs = choice.srcStartMs + length;
		// Each tile needs only its own choice, not a copy of the entire option
		// list. Retain that choice's offset for the compositor.
		clip.clipOptions = {choice};
		clip.clipShown = 0;
		// This is a comparison of the clip itself, not its neighbouring
		// transition. Keep absolute timeline time for scripts and keyframes.
		clip.transition.enabled = false;
		TlTrack track;
		track.clips = {clip};
		option->model.tracks = {track};
		option->frames.source.id = clip.sourceId;
		option->frames.source.missing = true;
		for (const auto &source : d_->input.sources) {
			if (source.id == clip.sourceId) {
				option->frames.source = source;
				break;
			}
		}
		option->frames.bound = d_->size;
		options.push_back(std::move(option));
	}
	for (;;) {
		Batch batch;
		{
			QMutexLocker lock(&d_->mutex);
			while (!d_->pending && !d_->cancelled) {
				d_->wake.wait(&d_->mutex);
			}
			if (d_->cancelled) {
				return;
			}
			batch.localMs = d_->localMs;
			batch.revision = d_->revision;
			d_->pending = false;
		}
		for (auto &option : options) {
			if (d_->cancelled) {
				return;
			}
			TimelineCompositor::RenderError error;
			evaluator.clearError();
			QImage image = TimelineCompositor::compose(option->model,
								   d_->input.clip.outStartMs + batch.localMs, d_->size,
								   option->frames, &evaluator, d_->input.fps,
								   d_->input.canvas, &error, &d_->registry);
			batch.images.append(image);
			batch.errors.append(!option->frames.error.isEmpty()    ? option->frames.error
					    : !error.message.isEmpty()         ? error.message
					    : !evaluator.lastError().isEmpty() ? evaluator.lastError()
									       : scriptError);
		}
		{
			QMutexLocker lock(&d_->mutex);
			if (d_->cancelled) {
				return;
			}
			d_->batch = std::move(batch);
		}
		emit batchReady();
	}
}

struct RandomClipCompareDialog::Impl {
	QPointer<RandomClipCompareRenderer> renderer;
	QVector<QFrame *> cards;
	QVector<ComparePreview *> previews;
	QGridLayout *grid = nullptr;
	QScrollArea *scroll = nullptr;
	QLabel *readout = nullptr;
	QPushButton *play = nullptr;
	QSlider *slider = nullptr;
	QSize size;
	QTimer timer;
	QElapsedTimer clock, renderClock;
	qint64 duration = 1, playMs = 0, anchorMs = 0, wantedMs = 0;
	quint64 revision = 0;
	int interval = 84, columns = 0;
	bool playing = true, busy = false, stopped = false, scrubbing = false, haveFrames = false;
};

RandomClipCompareDialog::RandomClipCompareDialog(const Input &input, QWidget *parent)
	: QDialog(parent),
	  d_(std::make_unique<Impl>())
{
	setObjectName(QStringLiteral("randomClipCompareDialog"));
	setWindowTitle(QStringLiteral("Compare Random Clip variations"));
	setWindowModality(Qt::WindowModal);
	setMinimumSize(480, 360);
	resize(960, 660);
	d_->duration = std::max<qint64>(1, input.clip.outDurationMs());
	d_->size = smallCanvas(input.canvas, int(input.clip.clipOptions.size()));
	d_->interval = input.clip.clipOptions.size() > 9 ? 125 : 84;
	auto *layout = new QVBoxLayout(this);
	auto *hint = new QLabel(QStringLiteral("All variations play together · low resolution · muted"), this);
	hint->setToolTip(QStringLiteral(
		"Uses each option's source time, framing, effects and animation. All tiles update at the same output time. Preview quality is reduced to keep the editor responsive; export quality stays unchanged."));
	layout->addWidget(hint);
	d_->scroll = new QScrollArea(this);
	d_->scroll->setWidgetResizable(true);
	auto *content = new QWidget(d_->scroll);
	d_->grid = new QGridLayout(content);
	d_->grid->setContentsMargins(0, 0, 0, 0);
	for (int k = 0; k < input.clip.clipOptions.size(); ++k) {
		const auto choice = input.clip.clipOptionAt(k);
		QString name = QStringLiteral("Missing source");
		for (const auto &source : input.sources) {
			if (source.id == choice.sourceId) {
				name = source.name;
				break;
			}
		}
		auto *card = new QFrame(content);
		card->setObjectName(QStringLiteral("compareVariationCard"));
		card->setProperty("optionIndex", k);
		card->setMinimumHeight(200);
		card->setStyleSheet(
			QStringLiteral("QFrame#compareVariationCard{border:1px solid %1; border-radius:4px;}")
				.arg(k == input.clip.clipShown ? QStringLiteral("#22c55e")
							       : QStringLiteral("#464a52")));
		auto *body = new QVBoxLayout(card);
		auto *nameLabel = new QLabel(QStringLiteral("%1. %2").arg(k + 1).arg(name), card);
		nameLabel->setWordWrap(true);
		nameLabel->setToolTip(name);
		nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		body->addWidget(nameLabel);
		auto *preview = new ComparePreview(d_->size, card);
		preview->setObjectName(QStringLiteral("compareVariationPreview"));
		body->addWidget(preview, 1);
		auto *time = new QLabel(QStringLiteral("Source %1 → %2")
						.arg(timeText(choice.srcStartMs))
						.arg(timeText(choice.srcStartMs + input.clip.srcLenMs())),
					card);
		time->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		body->addWidget(time);
		d_->cards.append(card);
		d_->previews.append(preview);
	}
	d_->scroll->setWidget(content);
	layout->addWidget(d_->scroll, 1);
	d_->slider = new QSlider(Qt::Horizontal, this);
	d_->slider->setObjectName(QStringLiteral("compareVariationsScrub"));
	d_->slider->setRange(0, 10000);
	layout->addWidget(d_->slider);
	auto *controls = new QHBoxLayout;
	d_->play = new QPushButton(QStringLiteral("Pause"), this);
	d_->play->setObjectName(QStringLiteral("compareVariationsPlay"));
	d_->play->setFocusPolicy(Qt::NoFocus);
	d_->play->setToolTip(QStringLiteral("Play or pause every variation together (Space)"));
	controls->addWidget(d_->play);
	auto *restart = new QPushButton(QStringLiteral("Restart"), this);
	restart->setObjectName(QStringLiteral("compareVariationsRestart"));
	restart->setFocusPolicy(Qt::NoFocus);
	controls->addWidget(restart);
	d_->readout = new QLabel(QStringLiteral("Loading previews…"), this);
	controls->addWidget(d_->readout, 1);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	controls->addWidget(buttons);
	layout->addLayout(controls);
	connect(buttons, &QDialogButtonBox::rejected, this, &RandomClipCompareDialog::reject);
	connect(d_->play, &QPushButton::clicked, this, [this]() { setPlaying(!isPlaying()); });
	connect(restart, &QPushButton::clicked, this, [this]() { seekTo(0); });
	connect(d_->slider, &QSlider::sliderPressed, this, [this]() {
		d_->scrubbing = true;
		d_->timer.stop();
	});
	connect(d_->slider, &QSlider::valueChanged, this,
		[this](int value) { seekTo(qint64(double(value) / 10000.0 * double(d_->duration - 1))); });
	connect(d_->slider, &QSlider::sliderReleased, this, [this]() {
		d_->scrubbing = false;
		seekTo(d_->wantedMs);
	});
	d_->timer.setSingleShot(true);
	d_->timer.setTimerType(Qt::PreciseTimer);
	connect(&d_->timer, &QTimer::timeout, this,
		[this]() { requestFrame((d_->anchorMs + d_->clock.elapsed()) % d_->duration); });
	d_->renderer = new RandomClipCompareRenderer(input, d_->size);
	connect(d_->renderer, &RandomClipCompareRenderer::batchReady, this, &RandomClipCompareDialog::acceptBatch);
	// No parent owns a running QThread. Closing the window cancels it without
	// waiting on disk/codec work; it deletes itself after that work unwinds.
	connect(d_->renderer, &QThread::finished, d_->renderer, &QObject::deleteLater);
	if (auto *app = QCoreApplication::instance()) {
		connect(app, &QCoreApplication::aboutToQuit, d_->renderer, [renderer = d_->renderer]() {
			// Only application shutdown waits. Ordinary window closing is
			// asynchronous; globals must outlive any active render.
			if (renderer) {
				renderer->cancel();
				renderer->wait();
			}
		});
	}
	d_->renderer->start();
	arrangeTiles();
	d_->clock.start();
	requestFrame(0);
}

RandomClipCompareDialog::~RandomClipCompareDialog()
{
	stopRendering();
}
void RandomClipCompareDialog::stopRendering()
{
	d_->stopped = true;
	d_->playing = false;
	d_->timer.stop();
	if (d_->renderer) {
		d_->renderer->cancel();
	}
}
void RandomClipCompareDialog::done(int result)
{
	stopRendering();
	QDialog::done(result);
}
bool RandomClipCompareDialog::isPlaying() const
{
	return d_->playing;
}
qint64 RandomClipCompareDialog::playheadMs() const
{
	return d_->playMs;
}
qint64 RandomClipCompareDialog::durationMs() const
{
	return d_->duration;
}
int RandomClipCompareDialog::optionCount() const
{
	return int(d_->previews.size());
}
QSize RandomClipCompareDialog::renderSize() const
{
	return d_->size;
}
QImage RandomClipCompareDialog::previewImage(int option) const
{
	return option >= 0 && option < d_->previews.size() ? d_->previews[option]->image : QImage();
}
QString RandomClipCompareDialog::optionError(int option) const
{
	return option >= 0 && option < d_->previews.size() ? d_->previews[option]->error : QString();
}
void RandomClipCompareDialog::setPlaying(bool playing)
{
	if (d_->stopped || d_->playing == playing) {
		return;
	}
	d_->playing = playing;
	d_->play->setText(playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
	d_->play->setIcon(uiIcon(playing ? Glyph::Pause : Glyph::Play, 14));
	seekTo(d_->playMs);
}
void RandomClipCompareDialog::seekTo(qint64 localMs)
{
	if (d_->stopped) {
		return;
	}
	d_->anchorMs = std::clamp<qint64>(localMs, 0, d_->duration - 1);
	d_->clock.restart();
	requestFrame(d_->anchorMs);
}
void RandomClipCompareDialog::requestFrame(qint64 localMs)
{
	if (d_->stopped || !d_->renderer) {
		return;
	}
	d_->timer.stop();
	d_->wantedMs = localMs;
	++d_->revision;
	if (d_->busy) {
		return;
	}
	d_->busy = true;
	d_->renderClock.restart();
	d_->renderer->request(localMs, d_->revision);
}
void RandomClipCompareDialog::acceptBatch()
{
	if (d_->stopped || !d_->renderer) {
		return;
	}
	auto batch = d_->renderer->takeBatch();
	d_->busy = false;
	if (batch.revision != d_->revision) {
		// A newer scrub/pause arrived during this batch. Never flash an old
		// frame over the newly selected time, and never queue all scrubs.
		requestFrame(d_->wantedMs);
		return;
	}
	for (int k = 0; k < d_->previews.size(); ++k) {
		d_->previews[k]->image = batch.images.value(k);
		d_->previews[k]->error = batch.errors.value(k);
		d_->previews[k]->update();
	}
	d_->playMs = batch.localMs;
	if (!d_->haveFrames) {
		d_->haveFrames = true;
		d_->anchorMs = d_->playMs;
		d_->clock.restart();
	}
	if (!d_->scrubbing) {
		const QSignalBlocker block(d_->slider);
		d_->slider->setValue(int(double(d_->playMs) / std::max<qint64>(1, d_->duration - 1) * 10000.0));
	}
	d_->readout->setText(QStringLiteral("%1 / %2 · looping").arg(timeText(d_->playMs), timeText(d_->duration)));
	if (d_->playing && !d_->scrubbing) {
		d_->timer.start(
			std::max(1, d_->interval - int(std::min<qint64>(d_->renderClock.elapsed(), d_->interval))));
	}
}
void RandomClipCompareDialog::arrangeTiles()
{
	if (!d_->scroll) {
		return;
	}
	const int columns = std::clamp(d_->scroll->viewport()->width() / 240, 1, 4);
	if (columns == d_->columns) {
		return;
	}
	d_->columns = columns;
	while (QLayoutItem *item = d_->grid->takeAt(0)) {
		delete item;
	}
	for (int k = 0; k < d_->cards.size(); ++k) {
		d_->grid->addWidget(d_->cards[k], k / columns, k % columns);
	}
}
void RandomClipCompareDialog::resizeEvent(QResizeEvent *event)
{
	QDialog::resizeEvent(event);
	// The scroll viewport takes its new size after the dialog's layout pass.
	QTimer::singleShot(0, this, [this]() { arrangeTiles(); });
}
void RandomClipCompareDialog::keyPressEvent(QKeyEvent *event)
{
	if (event->key() == Qt::Key_Space) {
		setPlaying(!isPlaying());
		event->accept();
	} else if (event->key() == Qt::Key_Home) {
		seekTo(0);
		event->accept();
	} else {
		QDialog::keyPressEvent(event);
	}
}

} // namespace harpia
