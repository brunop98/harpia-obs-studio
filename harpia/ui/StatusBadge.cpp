#include "StatusBadge.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QTimer>

#include <cmath>

namespace harpia {

namespace {
constexpr int kDotRadius = 4;
constexpr int kPadX = 10; // left/right inner padding
constexpr int kGap = 8;   // dot -> text gap
} // namespace

StatusBadge::StatusBadge(QWidget *parent) : QWidget(parent)
{
	timer_ = new QTimer(this);
	timer_->setInterval(50);
	connect(timer_, &QTimer::timeout, this, [this]() {
		phase_ += 0.18;
		update();
	});
	timer_->start();
}

void StatusBadge::setStatus(const QString &text, const QColor &color, bool pulse)
{
	// Called every UI tick — skip the relayout/repaint when nothing changed.
	if (text == text_ && color == color_ && pulse == pulse_)
		return;
	text_ = text;
	color_ = color;
	pulse_ = pulse;
	if (pulse && !timer_->isActive())
		timer_->start();
	else if (!pulse && timer_->isActive())
		timer_->stop();
	updateGeometry();
	update();
}

QSize StatusBadge::sizeHint() const
{
	const QFontMetrics fm(font());
	// Reserve at least the widest routine label so flipping between states
	// (Ready → Recording → Stopping…) doesn't shift the centered control row.
	const int textW = std::max(fm.horizontalAdvance(text_),
				   fm.horizontalAdvance(QStringLiteral("Starting in 10s")));
	const int w = kPadX + (kDotRadius * 2) + kGap + textW + kPadX;
	const int h = std::max(22, fm.height() + 6);
	return QSize(w, h);
}

void StatusBadge::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	const QRectF r = rect().adjusted(0.5, 0.5, -0.5, -0.5);

	// Faint tinted pill — informational, not a raised button.
	QColor fill = color_;
	fill.setAlpha(30);
	p.setPen(Qt::NoPen);
	p.setBrush(fill);
	p.drawRoundedRect(r, r.height() / 2.0, r.height() / 2.0);

	const double cy = rect().center().y() + 0.5;
	const double cx = kPadX + kDotRadius;

	// Pulsing glow around the dot for live states.
	if (pulse_) {
		const double t = 0.5 + 0.5 * std::sin(phase_);
		QColor glow = color_;
		glow.setAlpha((int)(90 * t));
		p.setBrush(glow);
		const double gr = kDotRadius + 2.0 + 2.0 * t;
		p.drawEllipse(QPointF(cx, cy), gr, gr);
	}

	// The dot itself.
	p.setBrush(color_);
	p.drawEllipse(QPointF(cx, cy), (double)kDotRadius, (double)kDotRadius);

	// Label.
	p.setPen(QColor(0xe6, 0xe6, 0xe6));
	const QRect textRect(int(cx + kDotRadius + kGap), 0,
			     width() - int(cx + kDotRadius + kGap) - kPadX, height());
	p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text_);
}

} // namespace harpia
