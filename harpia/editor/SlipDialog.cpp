#include "SlipDialog.hpp"

#include "AudioPreview.hpp"
#include "FrameSeeker.hpp"
#include "TimelineAudio.hpp"
#include "timeline/TimelineCompositor.hpp"
#include "ui/UiIcons.hpp"

#include <QApplication>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace harpia {

namespace {

// "0:05.2", or "1:02:05.2" past an hour.
QString timeText(qint64 ms)
{
	ms = std::max<qint64>(0, ms);
	const qint64 tenths = (ms + 50) / 100;
	const qint64 s = tenths / 10, h = s / 3600, m = (s / 60) % 60, sec = s % 60;
	const QString tail = QStringLiteral("%1.%2").arg(sec, 2, 10, QLatin1Char('0')).arg(tenths % 10);
	return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(tail)
		     : QStringLiteral("%1:%2").arg(m).arg(tail);
}

QString signedSeconds(qint64 ms)
{
	return (ms > 0 ? QStringLiteral("+") : ms < 0 ? QStringLiteral("−") : QString()) +
	       QString::number(std::abs(ms) / 1000.0, 'f', 1) + QStringLiteral(" s");
}

} // namespace

// ---- the preview ---------------------------------------------------------------

class SlipPreview : public QWidget {
public:
	explicit SlipPreview(QWidget *parent) : QWidget(parent)
	{
		setMinimumSize(320, 180);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	}
	void setImage(const QImage &img)
	{
		img_ = img;
		update();
	}
	const QImage &image() const { return img_; }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.fillRect(rect(), QColor(0x0b, 0x0d, 0x10));
		if (img_.isNull())
			return;
		const QSize fit = img_.size().scaled(size(), Qt::KeepAspectRatio);
		const QRect r(QPoint((width() - fit.width()) / 2, (height() - fit.height()) / 2), fit);
		p.setRenderHint(QPainter::SmoothPixmapTransform, true);
		p.drawImage(r, img_);
	}

private:
	QImage img_;
};

// ---- the track bar -------------------------------------------------------------

SlipTrackBar::SlipTrackBar(QWidget *parent) : QWidget(parent)
{
	setMinimumHeight(84);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setCursor(Qt::OpenHandCursor);
	setToolTip(QStringLiteral("Drag the strip to pick another moment: the window keeps the clip's length"));
}

void SlipTrackBar::setSource(const QVector<QImage> &thumbs, const QVector<float> &peaks, qint64 sourceMs)
{
	thumbs_ = thumbs;
	peaks_ = peaks;
	sourceMs_ = sourceMs;
	update();
}

void SlipTrackBar::setWindow(qint64 baseStartMs, qint64 lenMs, qint64 loMs, qint64 hiMs)
{
	baseStart_ = baseStartMs;
	len_ = std::max<qint64>(1, lenMs);
	lo_ = std::min<qint64>(0, loMs);
	hi_ = std::max<qint64>(0, hiMs);
	delta_ = clampDelta(delta_);
	update();
}

void SlipTrackBar::setDelta(qint64 d)
{
	delta_ = clampDelta(d);
	update();
}

void SlipTrackBar::setPlayFraction(double f)
{
	if (f == playFrac_)
		return;
	playFrac_ = f;
	update();
}

qint64 SlipTrackBar::clampDelta(qint64 d) const
{
	return std::clamp(d, lo_, hi_);
}

QRect SlipTrackBar::windowRect() const
{
	const int top = 18, h = std::max(24, height() - 36);
	// As wide as the clip is a share of its source, so a short clip of a long
	// recording reads as a short window -- but never too thin to see or too
	// wide to leave footage either side of it.
	const qint64 total = sourceMs_ > 0 ? sourceMs_ : baseStart_ + len_ + hi_;
	const double share = total > 0 ? double(len_) / double(total) : 0.4;
	const int minW = std::max(64, width() / 5);
	const int maxW = std::max(minW, width() * 3 / 5);
	const int w = std::clamp(int(std::lround(width() * 0.8 * share)), minW, maxW);
	return QRect((width() - w) / 2, top, w, h);
}

double SlipTrackBar::pxPerMs() const
{
	return double(windowRect().width()) / double(len_);
}

double SlipTrackBar::xOf(qint64 srcMs) const
{
	return windowRect().x() + double(srcMs - (baseStart_ + delta_)) * pxPerMs();
}

void SlipTrackBar::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.fillRect(rect(), QColor(0x15, 0x17, 0x1a));
	const QRect win = windowRect();
	const qint64 total = sourceMs_ > 0 ? sourceMs_ : baseStart_ + len_ + hi_;
	const double x0 = xOf(0), x1 = xOf(total);
	const QRectF strip(x0, win.top(), x1 - x0, win.height());

	// The source: its filmstrip, or its waveform for sound.
	p.save();
	p.setClipRect(strip.intersected(QRectF(rect())));
	p.fillRect(strip, QColor(0x2a, 0x2f, 0x37));
	if (!thumbs_.isEmpty() && total > 0) {
		const QImage &first = thumbs_.first();
		const double aspect = first.isNull() || first.height() <= 0 ? 16.0 / 9.0
									    : double(first.width()) / first.height();
		const int tileW = std::max(12, int(std::lround(win.height() * aspect)));
		const int n = int(thumbs_.size());
		const int k0 = std::max(0, int(std::floor((0.0 - x0) / tileW)));
		for (int k = k0;; ++k) {
			const double x = x0 + double(k) * tileW;
			if (x >= std::min<double>(x1, width()))
				break;
			const double centreMs = (x + tileW / 2.0 - x0) / pxPerMs();
			const int idx = std::clamp(int(centreMs / double(total) * n), 0, n - 1);
			if (!thumbs_[idx].isNull())
				p.drawImage(QRectF(x, win.top(), tileW, win.height()), thumbs_[idx]);
		}
	} else if (!peaks_.isEmpty() && total > 0) {
		p.setPen(QPen(QColor(0x5e, 0xa8, 0xd8), 1));
		const int n = int(peaks_.size());
		const double mid = win.center().y() + 0.5;
		for (int x = std::max(0, int(x0)); x < std::min<double>(x1, width()); ++x) {
			const double ms = (x - x0) / pxPerMs();
			const float a = peaks_[std::clamp(int(ms / double(total) * n), 0, n - 1)];
			const double hh = std::clamp<double>(a, 0.0, 1.0) * (win.height() / 2.0 - 2);
			p.drawLine(QPointF(x, mid - hh), QPointF(x, mid + hh));
		}
	}
	p.restore();

	// Everything outside the window is the footage the clip does not show.
	QPainterPath outside;
	outside.addRect(QRectF(rect()));
	outside.addRoundedRect(QRectF(win), 5, 5);
	p.fillPath(outside, QColor(0x0b, 0x0d, 0x10, 160));

	p.setRenderHint(QPainter::Antialiasing, true);
	const QColor accent(0x22, 0xc5, 0x5e);
	p.setPen(QPen(accent, 3));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(QRectF(win).adjusted(1.5, 1.5, -1.5, -1.5), 5, 5);
	// Brackets at both ends, like a trim handle that does not trim.
	p.setPen(Qt::NoPen);
	p.setBrush(accent);
	p.drawRoundedRect(QRectF(win.left() - 3, win.top(), 7, win.height()), 3, 3);
	p.drawRoundedRect(QRectF(win.right() - 3, win.top(), 7, win.height()), 3, 3);
	if (playFrac_ >= 0.0) {
		const double x = win.left() + playFrac_ * win.width();
		p.setPen(QPen(Qt::white, 2));
		p.drawLine(QPointF(x, win.top() - 3), QPointF(x, win.bottom() + 3));
	}

	// The moment in the window, above it; where the source starts and ends,
	// under the strip when they are in view.
	QFont f = font();
	f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1));
	p.setFont(f);
	p.setPen(QColor(0xe9, 0xff, 0xef));
	const qint64 start = baseStart_ + delta_;
	p.drawText(QRect(win.left(), 0, win.width(), win.top() - 2), Qt::AlignLeft | Qt::AlignBottom, timeText(start));
	p.drawText(QRect(win.left(), 0, win.width(), win.top() - 2), Qt::AlignRight | Qt::AlignBottom,
		   timeText(start + len_));
	p.setPen(QColor(0x8a, 0x90, 0x9a));
	const int below = win.bottom() + 2;
	if (x0 >= 0 && x0 < width())
		p.drawText(QRect(int(x0), below, 80, height() - below), Qt::AlignLeft | Qt::AlignTop, timeText(0));
	if (sourceMs_ > 0 && x1 > 0 && x1 <= width())
		p.drawText(QRect(int(x1) - 80, below, 80, height() - below), Qt::AlignRight | Qt::AlignTop,
			   timeText(sourceMs_));
}

void SlipTrackBar::mousePressEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;
	dragging_ = true;
	pressX_ = int(e->position().x());
	pressDelta_ = delta_;
	setCursor(Qt::ClosedHandCursor);
	if (changed)
		changed(delta_, true);
}

void SlipTrackBar::mouseMoveEvent(QMouseEvent *e)
{
	if (!dragging_)
		return;
	// The footage follows the pointer: drag left and later footage slides
	// into the window, drag right and earlier footage does.
	const double moved = e->position().x() - pressX_;
	const qint64 d = clampDelta(pressDelta_ - qint64(std::llround(moved / pxPerMs())));
	if (d == delta_)
		return;
	delta_ = d;
	update();
	if (changed)
		changed(delta_, true);
}

void SlipTrackBar::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton || !dragging_)
		return;
	dragging_ = false;
	setCursor(Qt::OpenHandCursor);
	if (changed)
		changed(delta_, false);
}

void SlipTrackBar::wheelEvent(QWheelEvent *e)
{
	// A tenth of the clip a notch: down (or right) is later footage.
	const QPoint a = e->angleDelta();
	const int notches = (std::abs(a.y()) >= std::abs(a.x()) ? -a.y() : a.x()) / 120;
	if (notches == 0)
		return;
	const qint64 step = std::max<qint64>(33, len_ / 10);
	const qint64 d = clampDelta(delta_ + notches * step);
	e->accept();
	if (d == delta_)
		return;
	delta_ = d;
	update();
	if (changed)
		changed(delta_, false);
}

// ---- the window ------------------------------------------------------------------

struct SlipDialog::Impl {
	// This clip's frames, from its own decoder: the window never asks the
	// editor's preview for a picture, so nothing else on the timeline can
	// show up in it.
	struct Provider : TimelineCompositor::FrameProvider {
		FrameSeeker seeker;
		QSize original;
		QSize bound;
		QSize sourceSize(int) override { return original; }
		QImage frameFor(int, qint64 srcMs) override
		{
			return seeker.isOpen() ? seeker.frameAt(srcMs, bound.width(), bound.height()) : QImage();
		}
	} provider;
	QSize canvas;
	QTimer timer;
	QElapsedTimer clock;
	AudioPreview *audio = nullptr;
	bool resumeAfterDrag = false;
};

SlipDialog::SlipDialog(const Input &in, QWidget *parent) : QDialog(parent), in_(in), d_(std::make_unique<Impl>())
{
	setWindowTitle(QStringLiteral("Slip — %1").arg(in_.title));
	setMinimumSize(560, 460);
	resize(820, 600);
	const double speed = in_.clip.speed > 0.01 ? in_.clip.speed : 1.0;
	outDurMs_ = std::max<qint64>(1, qint64(std::llround(double(in_.clip.srcLenMs()) / speed)));

	// What the preview is drawn at: the clip's own shape (its crop, else the
	// source), at most 960x540 -- big enough to judge a moment by.
	QSize shape = in_.clip.crop.isValid() && in_.clip.crop.width() > 1 ? in_.clip.crop.size() : in_.sourceSize;
	if (shape.isEmpty())
		shape = QSize(1920, 1080);
	QSize canvas = shape.scaled(QSize(960, 540), Qt::KeepAspectRatio);
	canvas = QSize(std::max(2, canvas.width() & ~1), std::max(2, canvas.height() & ~1));
	d_->canvas = canvas;
	d_->provider.original = in_.sourceSize;
	// Decoded big enough that a crop still fills the canvas sharply.
	double head = 1.0;
	if (!in_.sourceSize.isEmpty() && in_.clip.crop.isValid() && in_.clip.crop.width() > 1)
		head = std::max(double(in_.sourceSize.width()) / in_.clip.crop.width(),
				double(in_.sourceSize.height()) / in_.clip.crop.height());
	d_->provider.bound = QSize(int(canvas.width() * head), int(canvas.height() * head));
	if (!in_.soundOnly && !in_.decodePath.isEmpty())
		d_->provider.seeker.open(in_.decodePath);

	auto *v = new QVBoxLayout(this);
	v->setSpacing(8);
	preview_ = new SlipPreview(this);
	v->addWidget(preview_, 1);

	readout_ = new QLabel(this);
	readout_->setObjectName(QStringLiteral("slipReadout"));
	v->addWidget(readout_);

	bar_ = new SlipTrackBar(this);
	bar_->setSource(in_.thumbs, in_.soundOnly || in_.thumbs.isEmpty() ? in_.clip.peaks : QVector<float>(),
			in_.sourceMs);
	bar_->setWindow(in_.clip.srcStartMs, in_.clip.srcLenMs(), in_.loMs, in_.hiMs);
	v->addWidget(bar_);

	auto *row = new QHBoxLayout;
	playBtn_ = new QPushButton(QStringLiteral("Play"), this);
	playBtn_->setObjectName(QStringLiteral("slipPlay"));
	playBtn_->setIcon(uiIcon(Glyph::Play, 14));
	playBtn_->setToolTip(QStringLiteral("Play the chosen moment, on a loop, with its sound (Space)"));
	playBtn_->setFocusPolicy(Qt::NoFocus);
	auto *reset = new QPushButton(QStringLiteral("Reset"), this);
	reset->setObjectName(QStringLiteral("slipReset"));
	reset->setToolTip(QStringLiteral("Back to the moment the clip showed"));
	reset->setFocusPolicy(Qt::NoFocus);
	auto *hint = new QLabel(QStringLiteral("Drag the strip · ← → a frame, Shift a second"), this);
	hint->setStyleSheet(QStringLiteral("color:#8a909a;"));
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	for (QAbstractButton *b : buttons->buttons())
		b->setFocusPolicy(Qt::NoFocus);
	row->addWidget(playBtn_);
	row->addWidget(reset);
	row->addSpacing(8);
	row->addWidget(hint, 1);
	row->addWidget(buttons);
	v->addLayout(row);

	connect(buttons, &QDialogButtonBox::accepted, this, &SlipDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &SlipDialog::reject);
	connect(playBtn_, &QPushButton::clicked, this, [this]() { setPlaying(!playing_); });
	connect(reset, &QPushButton::clicked, this, [this]() { setDeltaMs(0); });

	bar_->changed = [this](qint64 delta, bool dragging) {
		delta_ = delta;
		syncReadout();
		if (dragging) {
			// While the strip moves, the preview shows where the moment now
			// starts; playing waits for the pointer to let go.
			if (playing_ && d_->timer.isActive()) {
				d_->resumeAfterDrag = true;
				d_->timer.stop();
				if (d_->audio)
					d_->audio->stop();
				bar_->setPlayFraction(-1.0);
			}
			render(0);
			return;
		}
		render(0);
		if (playing_) {
			d_->resumeAfterDrag = false;
			playMs_ = 0;
			d_->clock.restart();
			d_->timer.start();
			restartAudio();
		}
	};

	d_->timer.setTimerType(Qt::PreciseTimer);
	d_->timer.setInterval(std::max(8, int(1000.0 / std::clamp(in_.fps, 10.0, 60.0))));
	connect(&d_->timer, &QTimer::timeout, this, [this]() { tick(); });
	if (AudioPreview::available()) {
		d_->audio = new AudioPreview(this);
		// The end of the moment: round again, picture and sound together.
		connect(d_->audio, &AudioPreview::finished, this, [this]() {
			if (!playing_ || !d_->timer.isActive())
				return;
			playMs_ = 0;
			d_->clock.restart();
			d_->audio->start(0);
		});
	}

	setFocusPolicy(Qt::StrongFocus);
	syncReadout();
	render(0);
}

SlipDialog::~SlipDialog()
{
	stopAll();
}

void SlipDialog::stopAll()
{
	playing_ = false;
	d_->timer.stop();
	if (d_->audio)
		d_->audio->stop();
	++audioGen_; // a mix still on its way is for nobody now
}

void SlipDialog::accept()
{
	stopAll();
	QDialog::accept();
}

void SlipDialog::reject()
{
	stopAll();
	QDialog::reject();
}

void SlipDialog::setDeltaMs(qint64 d)
{
	bar_->setDelta(d);
	if (bar_->delta() == delta_)
		return;
	delta_ = bar_->delta();
	if (bar_->changed)
		bar_->changed(delta_, false);
}

TlClip SlipDialog::slipped(const TlClip &c) const
{
	TlClip o = c;
	o.srcStartMs += delta_;
	o.srcEndMs += delta_;
	return o;
}

TimelineModel SlipDialog::momentModel(bool withPartners) const
{
	// This clip, alone, from 0: its look (crop, colour, its components) but
	// not its pose, keyframes, script or fades -- the moment is what is shown,
	// filling the preview, from its first frame at full strength.
	TlClip v = slipped(in_.clip);
	const qint64 base = v.outStartMs;
	const TlClip plain;
	v.outStartMs = 0;
	v.setBaseTransform(plain.baseTransform());
	v.keys.clear();
	v.scripts.clear();
	v.fadeInMs = v.fadeOutMs = 0;
	v.transition.enabled = false;
	v.link = 0;
	v.clipOptions.clear();
	v.clipShown = 0;
	TlTrack t;
	t.kind = in_.soundOnly ? TlTrack::Kind::Audio : TlTrack::Kind::Video;
	t.clips = {v};
	TimelineModel m;
	m.tracks = {t};
	if (withPartners) {
		// Its detached sound, where it sits against the clip.
		for (const TlClip &pc : in_.partners) {
			TlClip q = slipped(pc);
			q.outStartMs -= base;
			if (q.outStartMs < 0) { // starts before the clip: from the clip's start
				const double sp = q.speed > 0.01 ? q.speed : 1.0;
				q.srcStartMs += qint64(std::llround(double(-q.outStartMs) * sp));
				q.outStartMs = 0;
			}
			if (q.srcStartMs >= q.srcEndMs)
				continue;
			q.fadeInMs = q.fadeOutMs = 0;
			q.transition.enabled = false;
			q.link = 0;
			TlTrack a;
			a.kind = TlTrack::Kind::Audio;
			a.clips = {q};
			m.tracks.append(a);
		}
	}
	return m;
}

void SlipDialog::render(qint64 outMs)
{
	if (in_.soundOnly) {
		// No picture: the moment's waveform, with the playhead.
		QImage img(d_->canvas.isEmpty() ? QSize(640, 360) : d_->canvas, QImage::Format_RGB32);
		img.fill(QColor(0x0b, 0x0d, 0x10));
		QPainter p(&img);
		const QVector<float> &pk = in_.clip.peaks;
		const qint64 total = in_.sourceMs > 0 ? in_.sourceMs : in_.clip.srcEndMs + in_.hiMs;
		const TlClip c = slipped(in_.clip);
		if (!pk.isEmpty() && total > 0) {
			p.setPen(QPen(QColor(0x5e, 0xa8, 0xd8), 1));
			const int n = int(pk.size());
			const double mid = img.height() / 2.0;
			for (int x = 0; x < img.width(); ++x) {
				const double ms = c.srcStartMs + double(x) / img.width() * c.srcLenMs();
				const float a = pk[std::clamp(int(ms / double(total) * n), 0, n - 1)];
				const double hh = std::clamp<double>(a, 0.0, 1.0) * (mid - 8);
				p.drawLine(QPointF(x, mid - hh), QPointF(x, mid + hh));
			}
		}
		p.setPen(QColor(0x8a, 0x90, 0x9a));
		p.drawText(img.rect().adjusted(12, 8, -12, -8), Qt::AlignLeft | Qt::AlignTop, QStringLiteral("Sound only"));
		if (playing_) {
			const double x = double(outMs) / double(outDurMs_) * img.width();
			p.setPen(QPen(Qt::white, 2));
			p.drawLine(QPointF(x, 0), QPointF(x, img.height()));
		}
		p.end();
		preview_->setImage(img);
		return;
	}
	const TimelineModel m = momentModel(false);
	const QImage img = TimelineCompositor::compose(m, std::clamp<qint64>(outMs, 0, outDurMs_ - 1), d_->canvas,
						       d_->provider, nullptr, in_.fps);
	preview_->setImage(img);
}

void SlipDialog::tick()
{
	if (!playing_)
		return;
	qint64 t = -1;
	if (audioReady_ && d_->audio && d_->audio->isPlaying())
		t = d_->audio->positionMs(); // the sound is the clock, when there is sound
	if (t < 0)
		t = d_->clock.elapsed();
	if (t >= outDurMs_) {
		// Round again.
		t = 0;
		d_->clock.restart();
		if (audioReady_ && d_->audio)
			d_->audio->start(0);
	}
	playMs_ = t;
	render(t);
	bar_->setPlayFraction(double(t) / double(outDurMs_));
}

void SlipDialog::restartAudio()
{
	audioReady_ = false;
	const int gen = ++audioGen_;
	if (!d_->audio || !in_.mixAudio)
		return;
	d_->audio->stop();
	// Decoding is too slow for the GUI thread (the first time a source is
	// heard, all of it): the picture plays at once and the sound joins it,
	// both from the top, when the mix lands.
	const TimelineModel m = momentModel(true);
	const auto mix = in_.mixAudio;
	const qint64 frames = TimelineAudio::frameForMs(outDurMs_) * TimelineAudio::kChannels;
	QPointer<SlipDialog> guard(this);
	QThreadPool::globalInstance()->start([guard, gen, m, mix, frames]() {
		std::vector<float> pcm = mix(m);
		if (qint64(pcm.size()) > frames)
			pcm.resize(size_t(frames)); // one pass of the moment, no more
		QMetaObject::invokeMethod(
			qApp,
			[guard, gen, pcm = std::move(pcm)]() mutable {
				if (!guard || gen != guard->audioGen_ || !guard->playing_ || !guard->d_->audio)
					return;
				guard->audioReady_ = true;
				if (pcm.empty())
					return; // a silent clip: the picture keeps its own clock
				guard->d_->audio->setBuffer(std::move(pcm));
				if (!guard->d_->timer.isActive())
					return; // mid-drag: it starts on release
				guard->playMs_ = 0;
				guard->d_->clock.restart();
				guard->d_->audio->start(0);
			},
			Qt::QueuedConnection);
	});
}

void SlipDialog::setPlaying(bool on)
{
	if (on == playing_)
		return;
	playing_ = on;
	playBtn_->setText(on ? QStringLiteral("Pause") : QStringLiteral("Play"));
	playBtn_->setIcon(uiIcon(on ? Glyph::Pause : Glyph::Play, 14));
	if (on) {
		playMs_ = 0;
		d_->clock.start();
		d_->timer.start();
		restartAudio();
		render(0);
	} else {
		stopAll();
		playMs_ = 0;
		bar_->setPlayFraction(-1.0);
		render(0);
	}
}

QImage SlipDialog::previewImage() const
{
	return preview_->image();
}

QString SlipDialog::readout() const
{
	return readout_->text();
}

void SlipDialog::syncReadout()
{
	const TlClip c = slipped(in_.clip);
	QString text = QStringLiteral("Shows %1 – %2").arg(timeText(c.srcStartMs), timeText(c.srcEndMs));
	if (in_.sourceMs > 0)
		text += QStringLiteral(" of %1").arg(timeText(in_.sourceMs));
	text += delta_ == 0 ? QStringLiteral("   ·   as it was")
			    : QStringLiteral("   ·   moved %1").arg(signedSeconds(delta_));
	if (delta_ == in_.loMs && in_.loMs < 0)
		text += QStringLiteral("   ·   at the start of the footage");
	else if (delta_ == in_.hiMs && in_.hiMs > 0)
		text += QStringLiteral("   ·   at the end of the footage");
	readout_->setText(text);
}

void SlipDialog::keyPressEvent(QKeyEvent *e)
{
	const qint64 frame = std::max<qint64>(1, qint64(std::llround(1000.0 / std::max(1.0, in_.fps))));
	const qint64 step = (e->modifiers() & Qt::ShiftModifier) ? 1000 : frame;
	switch (e->key()) {
	case Qt::Key_Left:
		setDeltaMs(delta_ - step);
		return;
	case Qt::Key_Right:
		setDeltaMs(delta_ + step);
		return;
	case Qt::Key_Home:
		setDeltaMs(in_.loMs);
		return;
	case Qt::Key_End:
		setDeltaMs(in_.hiMs);
		return;
	case Qt::Key_Space:
		setPlaying(!playing_);
		return;
	default:
		break;
	}
	QDialog::keyPressEvent(e);
}

} // namespace harpia
