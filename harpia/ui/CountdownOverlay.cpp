#include "CountdownOverlay.hpp"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTimer>

#include <algorithm>

namespace harpia {

CountdownOverlay::CountdownOverlay(QWidget *parent) : QWidget(parent)
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_DeleteOnClose, false);

	timer_ = new QTimer(this);
	timer_->setInterval(1000);
	connect(timer_, &QTimer::timeout, this, &CountdownOverlay::onTimeout);
}

void CountdownOverlay::start(int seconds, QScreen *screen)
{
	if (seconds < 1)
		seconds = 1;
	remaining_ = seconds;

	if (!screen)
		screen = QGuiApplication::primaryScreen();
	if (screen)
		setGeometry(screen->geometry());

	show();
	raise();
	activateWindow();
	setFocus();
	update();
	timer_->start();
}

void CountdownOverlay::stop()
{
	timer_->stop();
	hide();
}

void CountdownOverlay::onTimeout()
{
	--remaining_;
	if (remaining_ <= 0) {
		stop();
		emit finished();
		return;
	}
	emit tick(remaining_);
	update();
}

void CountdownOverlay::keyPressEvent(QKeyEvent *event)
{
	if (event->key() == Qt::Key_Escape) {
		stop();
		emit cancelled();
		return;
	}
	QWidget::keyPressEvent(event);
}

void CountdownOverlay::mousePressEvent(QMouseEvent *)
{
	// Clicking anywhere cancels (acts as the Cancel affordance).
	stop();
	emit cancelled();
}

void CountdownOverlay::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	// Dim the screen so the number stands out.
	p.fillRect(rect(), QColor(0, 0, 0, 140));

	// Big centered number.
	QFont numFont = font();
	numFont.setBold(true);
	numFont.setPointSize(std::max(64, height() / 5));
	p.setFont(numFont);
	p.setPen(QColor(0xff, 0xff, 0xff));
	p.drawText(rect(), Qt::AlignCenter, QString::number(remaining_));

	// Hint.
	QFont hintFont = font();
	hintFont.setPointSize(14);
	p.setFont(hintFont);
	p.setPen(QColor(0xd0, 0xd0, 0xd0));
	QRect hintRect = rect();
	hintRect.setTop(rect().center().y() + height() / 10);
	p.drawText(hintRect, Qt::AlignHCenter | Qt::AlignTop,
		   QStringLiteral("Recording starts in %1s — press Esc or click to cancel").arg(remaining_));
}

} // namespace harpia
