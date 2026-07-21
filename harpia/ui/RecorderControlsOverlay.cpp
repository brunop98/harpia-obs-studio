#include "RecorderControlsOverlay.hpp"

#include <QApplication>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScreen>
#include <QSettings>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011 // Windows 10 2004+
#endif
#endif

namespace harpia {

namespace {
constexpr qreal kIdleOpacity = 0.4; // dimmed while the mouse is away
constexpr qreal kHoverOpacity = 1.0;

QSettings hudSettings()
{
	return QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
}
} // namespace

RecorderControlsOverlay::RecorderControlsOverlay(QWidget *parent) : QWidget(parent)
{
	// Frameless, always on top, non-activating so it never steals focus from the
	// app the user is recording.
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
		       Qt::WindowDoesNotAcceptFocus);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_ShowWithoutActivating);
	setCursor(Qt::SizeAllCursor); // the background is a drag handle
	setToolTip(QStringLiteral("Drag to move · stays out of the recording"));

	auto *row = new QHBoxLayout(this);
	row->setContentsMargins(12, 8, 12, 8);
	row->setSpacing(8);
	// Leave room on the left for the painted "recording" dot.
	row->addSpacing(10);

	const QString btnBase = QStringLiteral(
		"QPushButton{color:#f2f3f5;border:none;border-radius:9px;padding:6px 12px;font-weight:bold;}"
		"QPushButton:disabled{color:#7d828b;}");

	pauseButton_ = new QPushButton(QStringLiteral("⏸  Pause"), this);
	pauseButton_->setCursor(Qt::PointingHandCursor);
	pauseButton_->setStyleSheet(btnBase + QStringLiteral("QPushButton{background:#3a3f47;}"
							     "QPushButton:hover{background:#474d57;}"
							     "QPushButton:disabled{background:#2a2d33;}"));
	row->addWidget(pauseButton_);

	stopButton_ = new QPushButton(QStringLiteral("■  Stop"), this);
	stopButton_->setCursor(Qt::PointingHandCursor);
	stopButton_->setStyleSheet(btnBase + QStringLiteral("QPushButton{background:#e5484d;}"
							    "QPushButton:hover{background:#f05055;}"
							    "QPushButton:disabled{background:#5a2f31;}"));
	row->addWidget(stopButton_);

	connect(pauseButton_, &QPushButton::clicked, this, &RecorderControlsOverlay::pauseClicked);
	connect(stopButton_, &QPushButton::clicked, this, &RecorderControlsOverlay::stopClicked);

	// Hover tracking must include the child buttons, otherwise entering a button
	// would read as "left the panel" and fade it out.
	pauseButton_->installEventFilter(this);
	stopButton_->installEventFilter(this);

	fade_ = new QPropertyAnimation(this, "windowOpacity", this);
	fade_->setDuration(180);

	setWindowOpacity(kIdleOpacity);
	adjustSize();
}

void RecorderControlsOverlay::setState(bool paused, bool pauseEnabled, bool stopEnabled)
{
	paused_ = paused;
	pauseButton_->setText(paused ? QStringLiteral("▶  Resume") : QStringLiteral("⏸  Pause"));
	pauseButton_->setEnabled(pauseEnabled);
	stopButton_->setEnabled(stopEnabled);
}

void RecorderControlsOverlay::showControls()
{
	if (isVisible())
		return;
	restorePosition();
	setWindowOpacity(kIdleOpacity);
	show();
	raise();
	excludeFromCapture(); // needs a valid winId → after show()
	updateHover();        // in case it appears already under the cursor
}

void RecorderControlsOverlay::hideControls()
{
	hide();
}

void RecorderControlsOverlay::restorePosition()
{
	const QSettings s = hudSettings();
	const QVariant saved = s.value(QStringLiteral("floatingControls/pos"));
	adjustSize();
	const QSize sz = sizeHint();

	if (saved.isValid()) {
		const QPoint p = saved.toPoint();
		// Only accept the saved spot if it still lands on a connected screen
		// (monitor layout may have changed since last run).
		for (QScreen *sc : QGuiApplication::screens()) {
			if (sc->availableGeometry().contains(QRect(p, sz).center())) {
				move(p);
				return;
			}
		}
	}
	// Default: lower-right of the primary screen, clear of the taskbar.
	QScreen *sc = QGuiApplication::primaryScreen();
	if (sc) {
		const QRect a = sc->availableGeometry();
		move(a.right() - sz.width() - 24, a.bottom() - sz.height() - 24);
	}
}

void RecorderControlsOverlay::savePosition()
{
	QSettings s = hudSettings();
	s.setValue(QStringLiteral("floatingControls/pos"), pos());
}

void RecorderControlsOverlay::excludeFromCapture()
{
#if defined(_WIN32)
	// Visible on screen, but omitted from the screen capture so it never shows
	// up in the recording. Requires Windows 10 version 2004 or newer.
	SetWindowDisplayAffinity((HWND)winId(), WDA_EXCLUDEFROMCAPTURE);
#endif
}

void RecorderControlsOverlay::updateHover()
{
	const bool hover = frameGeometry().contains(QCursor::pos());
	animateOpacity(hover ? kHoverOpacity : kIdleOpacity);
}

void RecorderControlsOverlay::animateOpacity(qreal to)
{
	if (qFuzzyCompare(windowOpacity(), to))
		return;
	fade_->stop();
	fade_->setStartValue(windowOpacity());
	fade_->setEndValue(to);
	fade_->start();
}

void RecorderControlsOverlay::enterEvent(QEnterEvent *)
{
	updateHover();
}

void RecorderControlsOverlay::leaveEvent(QEvent *)
{
	updateHover();
}

bool RecorderControlsOverlay::eventFilter(QObject *obj, QEvent *event)
{
	// The buttons' own enter/leave drive the same hover logic so moving between
	// the background and a button never flickers the opacity.
	if ((obj == pauseButton_ || obj == stopButton_) &&
	    (event->type() == QEvent::Enter || event->type() == QEvent::Leave))
		updateHover();
	return QWidget::eventFilter(obj, event);
}

void RecorderControlsOverlay::mousePressEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton) {
		dragging_ = true;
		dragOffset_ = e->globalPosition().toPoint() - frameGeometry().topLeft();
		e->accept();
	}
}

void RecorderControlsOverlay::mouseMoveEvent(QMouseEvent *e)
{
	if (dragging_ && (e->buttons() & Qt::LeftButton)) {
		// Global coordinates span the whole virtual desktop, so the panel drags
		// freely from one monitor to another.
		move(e->globalPosition().toPoint() - dragOffset_);
		e->accept();
	}
}

void RecorderControlsOverlay::mouseReleaseEvent(QMouseEvent *e)
{
	if (dragging_ && e->button() == Qt::LeftButton) {
		dragging_ = false;
		savePosition();
		e->accept();
	}
}

void RecorderControlsOverlay::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);

	// Subtle rounded, semi-opaque pill.
	QRectF r = rect().adjusted(0.5, 0.5, -0.5, -0.5);
	QPainterPath path;
	path.addRoundedRect(r, 12, 12);
	p.fillPath(path, QColor(0x1f, 0x22, 0x27, 0xf2));
	p.setPen(QPen(QColor(0xff, 0xff, 0xff, 0x22), 1));
	p.drawPath(path);

	// Recording indicator dot on the left.
	const int cy = height() / 2;
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0xe5, 0x48, 0x4d));
	p.drawEllipse(QPoint(14, cy), 4, 4);
}

} // namespace harpia
