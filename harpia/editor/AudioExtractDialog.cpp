#include "AudioExtractDialog.hpp"

#include "AudioPreview.hpp"
#include "EditorColors.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace harpia {

namespace {
constexpr int kHandleGrabPx = 7; // how near a handle counts as grabbing it
constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr int kWavePeaks = 1200; // buckets; more than any sane window is wide
} // namespace

// ---- AudioWaveform ---------------------------------------------------------

AudioWaveform::AudioWaveform(QWidget *parent) : QWidget(parent)
{
	setMinimumHeight(110);
	setMouseTracking(true); // so the handles can light up before you press
	setCursor(Qt::PointingHandCursor);
}

void AudioWaveform::setPeaks(const QVector<float> &peaks, qint64 durationMs)
{
	peaks_ = peaks;
	durationMs_ = std::max<qint64>(0, durationMs);
	startMs_ = 0;
	endMs_ = durationMs_;
	update();
}

void AudioWaveform::setTrim(qint64 startMs, qint64 endMs)
{
	startMs_ = std::clamp<qint64>(startMs, 0, durationMs_);
	endMs_ = std::clamp<qint64>(endMs, startMs_, durationMs_);
	update();
}

void AudioWaveform::setPlayhead(qint64 ms)
{
	if (playMs_ == ms)
		return;
	playMs_ = ms;
	update();
}

qint64 AudioWaveform::msAtX(int x) const
{
	if (width() <= 1 || durationMs_ <= 0)
		return 0;
	return std::clamp<qint64>(qint64(x) * durationMs_ / (width() - 1), 0, durationMs_);
}

int AudioWaveform::xForMs(qint64 ms) const
{
	if (durationMs_ <= 0)
		return 0;
	return int(std::clamp<qint64>(ms, 0, durationMs_) * (width() - 1) / durationMs_);
}

AudioWaveform::Grab AudioWaveform::grabAt(int x) const
{
	const int sx = xForMs(startMs_), ex = xForMs(endMs_);
	// The nearer handle wins when they are close together, so a tight trim can
	// still be adjusted from either end.
	const int ds = std::abs(x - sx), de = std::abs(x - ex);
	if (ds <= kHandleGrabPx && ds <= de)
		return Grab::Start;
	if (de <= kHandleGrabPx)
		return Grab::End;
	return Grab::None;
}

void AudioWaveform::mousePressEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;
	dragging_ = grabAt(e->pos().x());
	if (dragging_ == Grab::None) {
		emit scrubbed(msAtX(e->pos().x())); // a click in the body moves the playhead
		return;
	}
	mouseMoveEvent(e);
}

void AudioWaveform::mouseMoveEvent(QMouseEvent *e)
{
	const int x = e->pos().x();
	if (dragging_ == Grab::None) {
		const Grab h = grabAt(x);
		if (h != hover_) {
			hover_ = h;
			setCursor(h == Grab::None ? Qt::PointingHandCursor : Qt::SizeHorCursor);
			update();
		}
		return;
	}
	const qint64 ms = msAtX(x);
	// The handles cannot cross. Left at that, a fast drag past the other one
	// silently inverts the selection and the export comes out empty.
	if (dragging_ == Grab::Start)
		startMs_ = std::min(ms, endMs_);
	else
		endMs_ = std::max(ms, startMs_);
	update();
	emit trimChanged(startMs_, endMs_);
}

void AudioWaveform::mouseReleaseEvent(QMouseEvent *)
{
	dragging_ = Grab::None;
}

void AudioWaveform::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, false);
	const QRect r = rect();
	p.fillRect(r, QColor(0x16, 0x19, 0x1f));

	if (peaks_.isEmpty() || durationMs_ <= 0) {
		p.setPen(QColor(0x8a, 0x8f, 0x98));
		p.drawText(r, Qt::AlignCenter, QStringLiteral("Reading audio…"));
		return;
	}

	// The waveform, mirrored about the middle.
	const int mid = r.height() / 2;
	p.setPen(QColor(0x4a, 0x90, 0xe2));
	for (int x = 0; x < r.width(); ++x) {
		const int i = peaks_.size() * x / std::max(1, r.width());
		const float v = peaks_.at(std::clamp(i, 0, int(peaks_.size()) - 1));
		const int h = int(v * (mid - 4));
		p.drawLine(x, mid - h, x, mid + h);
	}

	// Shade what will be thrown away, rather than only outlining what is kept:
	// the discarded part is the thing you want to see shrinking as you drag.
	const int sx = xForMs(startMs_), ex = xForMs(endMs_);
	const QColor shade(0x0d, 0x11, 0x17, 190);
	if (sx > 0)
		p.fillRect(QRect(0, 0, sx, r.height()), shade);
	if (ex < r.width())
		p.fillRect(QRect(ex, 0, r.width() - ex, r.height()), shade);

	// The handles.
	for (const auto &pair : {std::make_pair(sx, hover_ == Grab::Start),
				 std::make_pair(ex, hover_ == Grab::End)}) {
		p.fillRect(QRect(pair.first - 1, 0, 3, r.height()),
			   pair.second ? QColor(0xff, 0xd5, 0x4a) : QColor(0xe8, 0xc0, 0x50));
	}

	if (playMs_ >= 0) {
		p.setPen(QPen(QColor(0x3f, 0xb9, 0x50), 1));
		const int px = xForMs(playMs_);
		p.drawLine(px, 0, px, r.height());
	}

	p.setPen(QColor(0x3a, 0x40, 0x4b));
	p.drawRect(r.adjusted(0, 0, -1, -1));
}

// ---- AudioExtractDialog ----------------------------------------------------

bool AudioExtractDialog::fileHasAudio(const QString &path, QString *why)
{
	// Decoding the whole file only to discover there is no audio would mean a
	// long wait before an error; this asks the container instead. It costs one
	// header read.
	QString err;
	const std::vector<float> probe = decodeAudioToPcm(path, kRate, kChannels, &err);
	if (!probe.empty())
		return true;
	if (why)
		*why = err == QStringLiteral("no-audio")
			       ? QStringLiteral("%1 has no audio track — there is nothing to extract.")
					 .arg(QFileInfo(path).fileName())
			       : err;
	return false;
}

AudioExtractDialog::AudioExtractDialog(const QString &sourcePath, QWidget *parent)
	: QDialog(parent), sourcePath_(sourcePath)
{
	setWindowTitle(QStringLiteral("Extract Audio — %1").arg(QFileInfo(sourcePath).fileName()));
	setModal(true);
	resize(760, 460);
	setSizeGripEnabled(true);

	auto *v = new QVBoxLayout(this);
	v->setContentsMargins(16, 14, 16, 12);
	v->setSpacing(10);

	wave_ = new AudioWaveform(this);
	v->addWidget(wave_, 1);

	auto *transport = new QHBoxLayout;
	transport->setSpacing(8);
	playBtn_ = new QPushButton(QStringLiteral("Play"), this);
	playBtn_->setFixedWidth(90);
	transport->addWidget(playBtn_);
	summary_ = new QLabel(this);
	summary_->setStyleSheet(QStringLiteral("color:#8a8f98;"));
	transport->addWidget(summary_, 1);
	v->addLayout(transport);

	auto *form = new QFormLayout;
	form->setHorizontalSpacing(14);
	form->setVerticalSpacing(8);

	auto *fadeRow = new QHBoxLayout;
	fadeInSpin_ = new QSpinBox(this);
	fadeInSpin_->setRange(0, 30000);
	fadeInSpin_->setSingleStep(100);
	fadeInSpin_->setSuffix(QStringLiteral(" ms"));
	fadeOutSpin_ = new QSpinBox(this);
	fadeOutSpin_->setRange(0, 30000);
	fadeOutSpin_->setSingleStep(100);
	fadeOutSpin_->setSuffix(QStringLiteral(" ms"));
	fadeRow->addWidget(new QLabel(QStringLiteral("in"), this));
	fadeRow->addWidget(fadeInSpin_);
	fadeRow->addSpacing(12);
	fadeRow->addWidget(new QLabel(QStringLiteral("out"), this));
	fadeRow->addWidget(fadeOutSpin_);
	fadeRow->addStretch(1);
	auto *fadeWrap = new QWidget(this);
	fadeWrap->setLayout(fadeRow);
	form->addRow(QStringLiteral("Fade"), fadeWrap);

	auto *gainRow = new QHBoxLayout;
	gainSlider_ = new QSlider(Qt::Horizontal, this);
	gainSlider_->setRange(-24, 18); // dB
	gainSlider_->setValue(0);
	gainLabel_ = new QLabel(QStringLiteral("0 dB"), this);
	gainLabel_->setMinimumWidth(56);
	auto *normBtn = new QPushButton(QStringLiteral("Normalise"), this);
	normBtn->setToolTip(QStringLiteral("Raise the level so the loudest moment sits just under "
					   "clipping."));
	gainRow->addWidget(gainSlider_, 1);
	gainRow->addWidget(gainLabel_);
	gainRow->addWidget(normBtn);
	auto *gainWrap = new QWidget(this);
	gainWrap->setLayout(gainRow);
	form->addRow(QStringLiteral("Volume"), gainWrap);

	speedSpin_ = new QDoubleSpinBox(this);
	speedSpin_->setRange(AudioEdit::kMinSpeed, AudioEdit::kMaxSpeed);
	speedSpin_->setSingleStep(0.05);
	speedSpin_->setValue(1.0);
	speedSpin_->setSuffix(QStringLiteral("×"));
	speedSpin_->setToolTip(QStringLiteral("Pitch-corrected, so a faster voice does not turn "
					      "into a chipmunk."));
	form->addRow(QStringLiteral("Speed"), speedSpin_);

	auto *outRow = new QHBoxLayout;
	formatCombo_ = new QComboBox(this);
	// Only what this build can actually write -- offering a format that fails
	// at the last step is worse than not offering it.
	for (AudioFormat f : {AudioFormat::Mp3, AudioFormat::M4a, AudioFormat::Wav})
		if (audioFormatAvailable(f))
			formatCombo_->addItem(QString::fromLatin1(audioFormatName(f)), int(f));
	qualityCombo_ = new QComboBox(this);
	qualityCombo_->addItem(QStringLiteral("128 kbps"), 128);
	qualityCombo_->addItem(QStringLiteral("192 kbps"), 192);
	qualityCombo_->addItem(QStringLiteral("256 kbps"), 256);
	qualityCombo_->addItem(QStringLiteral("320 kbps"), 320);
	qualityCombo_->setCurrentIndex(1);
	outRow->addWidget(formatCombo_);
	outRow->addWidget(qualityCombo_);
	outRow->addStretch(1);
	auto *outWrap = new QWidget(this);
	outWrap->setLayout(outRow);
	form->addRow(QStringLiteral("Format"), outWrap);
	v->addLayout(form);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
	exportBtn_ = buttons->addButton(QStringLiteral("Export…"), QDialogButtonBox::AcceptRole);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	v->addWidget(buttons);

	// ---- decode, once ----
	QString err;
	pcm_ = decodeAudioToPcm(sourcePath_, kRate, kChannels, &err);
	rate_ = kRate;
	channels_ = kChannels;
	durationMs_ = pcm_.empty() ? 0 : qint64(pcm_.size() / channels_) * 1000 / rate_;
	if (pcm_.empty()) {
		summary_->setText(err == QStringLiteral("no-audio")
					  ? QStringLiteral("This file has no audio track.")
					  : err);
		playBtn_->setEnabled(false);
		exportBtn_->setEnabled(false);
	} else {
		const std::vector<float> pk = peaksFromPcm(pcm_, channels_, kWavePeaks);
		wave_->setPeaks(QVector<float>(pk.begin(), pk.end()), durationMs_);
		wave_->setTrim(0, durationMs_);
	}

	preview_ = new AudioPreview(this);
	playTimer_ = new QTimer(this);
	playTimer_->setInterval(33);
	rebuildTimer_ = new QTimer(this);
	rebuildTimer_->setSingleShot(true);
	rebuildTimer_->setInterval(180);

	connect(playBtn_, &QPushButton::clicked, this, &AudioExtractDialog::onPlayPause);
	connect(exportBtn_, &QPushButton::clicked, this, &AudioExtractDialog::onExport);
	connect(normBtn, &QPushButton::clicked, this, &AudioExtractDialog::onNormalise);
	connect(rebuildTimer_, &QTimer::timeout, this, &AudioExtractDialog::rebuildPreviewBuffer);

	const auto touched = [this]() {
		refreshSummary();
		// Playing while a control moves: rebuild so what you hear matches what
		// you just changed. Stopped, defer it until Play is pressed.
		if (preview_ && preview_->isPlaying())
			rebuildTimer_->start();
	};
	connect(wave_, &AudioWaveform::trimChanged, this, [this, touched](qint64, qint64) { touched(); });
	connect(wave_, &AudioWaveform::scrubbed, this, [this](qint64 ms) { wave_->setPlayhead(ms); });
	connect(fadeInSpin_, &QSpinBox::valueChanged, this, [touched](int) { touched(); });
	connect(fadeOutSpin_, &QSpinBox::valueChanged, this, [touched](int) { touched(); });
	connect(speedSpin_, &QDoubleSpinBox::valueChanged, this, [touched](double) { touched(); });
	connect(gainSlider_, &QSlider::valueChanged, this, [this, touched](int db) {
		gainLabel_->setText(QStringLiteral("%1%2 dB").arg(db > 0 ? QStringLiteral("+") : QString()).arg(db));
		touched();
	});
	connect(formatCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
		// WAV is uncompressed: a bitrate picker beside it would be a lie.
		const bool lossy = formatCombo_->currentData().toInt() != int(AudioFormat::Wav);
		qualityCombo_->setEnabled(lossy);
		refreshSummary();
	});
	connect(playTimer_, &QTimer::timeout, this, [this]() {
		if (!preview_ || !preview_->isPlaying())
			return;
		// The preview plays the EDITED buffer, whose clock starts at the trim
		// point and runs at the edited speed -- so the playhead has to be
		// mapped back into source time or it crawls out of the selection.
		const AudioEdit e = currentEdit();
		qint64 s = 0, en = 0;
		resolveTrim(e, durationMs_, &s, &en);
		wave_->setPlayhead(s + qint64(preview_->positionMs() * e.speed));
	});
	connect(preview_, &AudioPreview::finished, this, [this]() {
		playTimer_->stop();
		playBtn_->setText(QStringLiteral("Play"));
		wave_->setPlayhead(-1);
	});

	qualityCombo_->setEnabled(formatCombo_->currentData().toInt() != int(AudioFormat::Wav));
	refreshSummary();
}

AudioExtractDialog::~AudioExtractDialog() = default;

AudioEdit AudioExtractDialog::currentEdit() const
{
	AudioEdit e;
	e.trimStartMs = wave_->trimStartMs();
	e.trimEndMs = wave_->trimEndMs();
	e.fadeInMs = fadeInSpin_->value();
	e.fadeOutMs = fadeOutSpin_->value();
	e.gain = dbToGain(gainSlider_->value());
	e.speed = speedSpin_->value();
	return e;
}

void AudioExtractDialog::refreshSummary()
{
	if (pcm_.empty())
		return;
	const AudioEdit e = currentEdit();
	const qint64 outMs = extractDurationMs(e, durationMs_);
	const int kbps = qualityCombo_->currentData().toInt();
	const bool wav = formatCombo_->currentData().toInt() == int(AudioFormat::Wav);
	// WAV is rate x channels x 2 bytes; lossy is just the bitrate. Both are
	// close enough to set expectations, which is all a summary line is for.
	const double bytes = wav ? double(outMs) / 1000.0 * rate_ * channels_ * 2
				 : double(outMs) / 1000.0 * kbps * 1000.0 / 8.0;
	summary_->setText(QStringLiteral("%1.%2 s  ·  about %3 MB")
				  .arg(outMs / 1000)
				  .arg((outMs % 1000) / 100)
				  .arg(bytes / (1024.0 * 1024.0), 0, 'f', 1));
	exportBtn_->setEnabled(outMs > 0);
}

void AudioExtractDialog::rebuildPreviewBuffer()
{
	if (pcm_.empty() || !preview_)
		return;
	const AudioEdit e = currentEdit();
	std::vector<float> buf = applyAudioEdit(pcm_, channels_, rate_, e);
	if (std::abs(e.speed - 1.0) > 0.001) {
		QString err;
		buf = retimePcm(buf, rate_, channels_, e.speed, &err);
	}
	const bool wasPlaying = preview_->isPlaying();
	preview_->setBuffer(std::move(buf));
	if (wasPlaying)
		preview_->start(0);
}

void AudioExtractDialog::onPlayPause()
{
	if (!preview_ || pcm_.empty())
		return;
	if (preview_->isPlaying()) {
		preview_->stop();
		playTimer_->stop();
		playBtn_->setText(QStringLiteral("Play"));
		wave_->setPlayhead(-1);
		return;
	}
	if (!AudioPreview::available()) {
		QMessageBox::information(this, windowTitle(),
					 QStringLiteral("No audio output device is available, so this "
							"cannot be previewed. Exporting still works."));
		return;
	}
	rebuildPreviewBuffer();
	preview_->start(0);
	playTimer_->start();
	playBtn_->setText(QStringLiteral("Stop"));
}

void AudioExtractDialog::onNormalise()
{
	if (pcm_.empty())
		return;
	// Measured on the TRIMMED audio, not the whole file: normalising to a peak
	// that has been cut away would leave the kept part quiet, which is the
	// opposite of what the button says.
	AudioEdit probe = currentEdit();
	probe.gain = 1.0;
	probe.fadeInMs = probe.fadeOutMs = 0; // fades would drag the measured peak down
	const std::vector<float> cut = applyAudioEdit(pcm_, channels_, rate_, probe);
	const double g = normaliseGainFor(cut);
	gainSlider_->setValue(int(std::lround(std::clamp(gainToDb(g), -24.0, 18.0))));
}

void AudioExtractDialog::onExport()
{
	if (pcm_.empty())
		return;
	const AudioFormat fmt = AudioFormat(formatCombo_->currentData().toInt());
	const QString ext = QString::fromLatin1(audioFormatExtension(fmt));
	const QFileInfo src(sourcePath_);
	const QString suggested = src.dir().filePath(src.completeBaseName() + QLatin1Char('.') + ext);
	const QString out = QFileDialog::getSaveFileName(
		this, QStringLiteral("Export audio"), suggested,
		QStringLiteral("%1 audio (*.%2)").arg(QString::fromLatin1(audioFormatName(fmt)), ext));
	if (out.isEmpty())
		return;

	if (preview_ && preview_->isPlaying()) {
		preview_->stop();
		playTimer_->stop();
		playBtn_->setText(QStringLiteral("Play"));
	}

	QApplication::setOverrideCursor(Qt::WaitCursor);
	const AudioEdit e = currentEdit();
	std::vector<float> buf = applyAudioEdit(pcm_, channels_, rate_, e);
	QString err;
	if (std::abs(e.speed - 1.0) > 0.001)
		buf = retimePcm(buf, rate_, channels_, e.speed, &err);
	const bool ok = encodeAudioFile(out, fmt, buf, rate_, channels_,
					qualityCombo_->currentData().toInt(), &err);
	QApplication::restoreOverrideCursor();

	if (!ok) {
		QMessageBox::warning(this, windowTitle(),
				     err.isEmpty() ? QStringLiteral("The export failed.") : err);
		return;
	}
	exportedPath_ = out;
	accept();
}

} // namespace harpia
