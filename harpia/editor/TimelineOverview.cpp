#include "editor/TimelineOverview.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVariantAnimation>

#include <algorithm>

namespace harpia {

namespace {
constexpr int kInsetX = 10; // room for the rounded backdrop
constexpr int kInsetY = 7;
constexpr int kMinBoxW = 4; // see viewportRect: never invisible
constexpr int kRadius = 5;
} // namespace

TimelineOverview::TimelineOverview(QWidget *parent) : QWidget(parent)
{
	// Read-only by construction. Anything under it -- the preview, its transform
	// grips -- keeps every click, so this can float wherever it reads best
	// without becoming something to work around.
	setAttribute(Qt::WA_TransparentForMouseEvents, true);
	setFixedHeight(kHeight);
	setVisible(false);

	idle_ = new QTimer(this);
	idle_->setSingleShot(true);
	idle_->setInterval(kIdleHideMs);
	connect(idle_, &QTimer::timeout, this, [this] { startFade(0.0); });

	fade_ = new QVariantAnimation(this);
	fade_->setDuration(kFadeMs);
	connect(fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
		opacity_ = v.toDouble();
		update();
	});
	// Hiding belongs on FINISHED, not on every value. A fade IN starts at zero,
	// so a "hide when transparent" test in valueChanged fired on the animation's
	// very first tick and hid the widget it had just been asked to show -- and
	// nothing turned it back on, because showing happens in showFor().
	connect(fade_, &QVariantAnimation::finished, this, [this] {
		if (fade_->endValue().toDouble() <= 0.001)
			setVisible(false);
	});
}

TimelineOverview::~TimelineOverview() = default;

QRect TimelineOverview::barRect() const
{
	return rect().adjusted(kInsetX, kInsetY, -kInsetX, -kInsetY);
}

int TimelineOverview::xForMs(qint64 totalMs, qint64 ms, const QRect &bar)
{
	if (totalMs <= 0 || bar.width() <= 0)
		return bar.x();
	const double f = std::clamp(double(ms) / double(totalMs), 0.0, 1.0);
	return bar.x() + int(std::lround(f * bar.width()));
}

QRect TimelineOverview::viewportRect(qint64 totalMs, qint64 startMs, qint64 visibleMs, const QRect &bar)
{
	if (totalMs <= 0 || bar.width() <= 0 || bar.height() <= 0)
		return QRect();
	// Zoomed all the way out (or further): the viewport IS the project, and the
	// box covers the bar. Saying so is more useful than drawing nothing.
	if (visibleMs >= totalMs)
		return bar;
	if (visibleMs <= 0)
		return bar;
	const qint64 maxStart = totalMs - visibleMs;
	const qint64 s = std::clamp<qint64>(startMs, 0, maxStart);
	const int x0 = xForMs(totalMs, s, bar);
	const int x1 = xForMs(totalMs, s + visibleMs, bar);
	int w = std::max(kMinBoxW, x1 - x0);
	// Widening for the minimum must not push the box off the end, or the last
	// slice of a project would read as if it were not quite at the end.
	int x = x0;
	if (x + w > bar.right() + 1)
		x = bar.right() + 1 - w;
	x = std::max(x, bar.x());
	w = std::min(w, bar.width());
	return QRect(x, bar.y(), w, bar.height());
}

void TimelineOverview::showFor(qint64 totalMs, qint64 startMs, qint64 visibleMs, qint64 playheadMs)
{
	totalMs_ = totalMs;
	startMs_ = startMs;
	visibleMs_ = visibleMs;
	playheadMs_ = playheadMs;
	if (totalMs <= 0) {
		hideNow();
		return;
	}
	setVisible(true);
	raise();
	startFade(1.0);
	idle_->start(); // restart the countdown on every movement
	update();
}

void TimelineOverview::hideNow()
{
	idle_->stop();
	fade_->stop();
	opacity_ = 0.0;
	setVisible(false);
}

void TimelineOverview::startFade(double to)
{
	if (std::abs(opacity_ - to) < 0.001) {
		if (to <= 0.001)
			setVisible(false);
		return;
	}
	fade_->stop();
	fade_->setStartValue(opacity_);
	fade_->setEndValue(to);
	fade_->start();
}

void TimelineOverview::finishFadeForTest()
{
	if (fade_->state() == QAbstractAnimation::Running) {
		opacity_ = fade_->endValue().toDouble();
		fade_->stop();
	}
	// The same rule the finished() handler applies, so the test drives the real
	// end state rather than one only it can reach.
	if (opacity_ <= 0.001)
		setVisible(false);
	update();
}

void TimelineOverview::paintEvent(QPaintEvent *)
{
	if (opacity_ <= 0.001 || totalMs_ <= 0)
		return;
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setOpacity(opacity_);

	// A backdrop, because this floats over the picture and a bare bar on a light
	// frame would be unreadable exactly when it is needed.
	QPainterPath bg;
	bg.addRoundedRect(QRectF(rect()).adjusted(2, 1, -2, -1), kRadius, kRadius);
	p.fillPath(bg, QColor(0x10, 0x12, 0x16, 220));

	const QRect bar = barRect();
	if (bar.width() <= 0)
		return;

	// The whole project.
	p.fillRect(bar, QColor(0x2a, 0x2e, 0x36));
	p.setPen(QColor(0x3a, 0x3f, 0x49));
	p.drawRect(bar.adjusted(0, 0, -1, -1));

	// Where you are. Red, as asked for: a wash so the slice reads at a glance,
	// and a hard border so its EDGES are exact -- the edges are the information.
	const QRect vp = viewportRect(totalMs_, startMs_, visibleMs_, bar);
	if (!vp.isEmpty()) {
		p.fillRect(vp, QColor(0xe8, 0x3b, 0x3b, 60));
		p.setPen(QPen(QColor(0xf1, 0x4c, 0x4c), 2));
		// Inset by the pen so the stroke lands inside the box rather than
		// straddling its edge, which would read as a pixel of extra width.
		p.drawRect(QRectF(vp).adjusted(1.0, 1.0, -1.0, -1.0));
	}

	// The playhead, on top of both: it is the one point you are looking for.
	if (playheadMs_ >= 0) {
		const int px = xForMs(totalMs_, playheadMs_, bar);
		p.setPen(QPen(QColor(0xff, 0xff, 0xff, 210), 1));
		p.drawLine(px, bar.top() - 2, px, bar.bottom() + 2);
	}
}

} // namespace harpia
