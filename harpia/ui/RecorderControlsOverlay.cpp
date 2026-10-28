#include "../core/RecorderSettings.hpp"
#include "RecorderControlsOverlay.hpp"

#include "UiIcons.hpp"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>

#include <algorithm>

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
	return harpia::recorderSettings();
}

QIcon regionLockIcon(bool locked)
{
	QIcon icon;
	for (int scale : {1, 2, 3}) {
		QPixmap pixels(16 * scale, 16 * scale);
		pixels.setDevicePixelRatio(scale);
		pixels.fill(Qt::transparent);
		QPainter p(&pixels);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(QPen(QColor(QStringLiteral("#f2f3f5")), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.setBrush(Qt::NoBrush);
		const double offset = locked ? 0.0 : 3.0;
		QPainterPath shackle;
		shackle.moveTo(5 + offset, 7);
		shackle.lineTo(5 + offset, 4.5);
		shackle.cubicTo(5 + offset, 0.5, 11 + offset, 0.5, 11 + offset, 4.5);
		if (locked)
			shackle.lineTo(11, 7);
		p.drawPath(shackle);
		p.setBrush(QColor(QStringLiteral("#f2f3f5")));
		p.drawRoundedRect(QRectF(3, 7, 10, 7), 1.5, 1.5);
		p.setPen(QPen(QColor(QStringLiteral("#3a3f47")), 1.5, Qt::SolidLine, Qt::RoundCap));
		p.drawLine(QPointF(8, 9.5), QPointF(8, 11.5));
		p.end();
		icon.addPixmap(pixels);
	}
	return icon;
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

	// Icon-only, square buttons — no text.
	const QString btnBase = QStringLiteral(
		"QPushButton{color:#f2f3f5;border:none;border-radius:9px;font-size:15px;font-weight:bold;}"
		"QPushButton:disabled{color:#7d828b;}");

	startButton_ = new QPushButton(this);
	startButton_->setIcon(uiIcon(Glyph::Record, 14));
	startButton_->setCursor(Qt::PointingHandCursor);
	startButton_->setFixedSize(32, 32);
	startButton_->setToolTip(QStringLiteral("Start recording"));
	// The same green as the region frame's Record button and the main window's
	// Resume, so one colour means "go" everywhere.
	startButton_->setStyleSheet(btnBase + QStringLiteral("QPushButton{background:#3fb950;}"
							     "QPushButton:hover{background:#4ac462;}"
							     "QPushButton:disabled{background:#2f5c39;}"));
	row->addWidget(startButton_);

	pauseButton_ = new QPushButton(this);
	pauseButton_->setIcon(uiIcon(Glyph::Pause, 14));
	pauseButton_->setCursor(Qt::PointingHandCursor);
	pauseButton_->setFixedSize(32, 32);
	pauseButton_->setStyleSheet(btnBase + QStringLiteral("QPushButton{background:#3a3f47;}"
							     "QPushButton:hover{background:#474d57;}"
							     "QPushButton:disabled{background:#2a2d33;}"));
	row->addWidget(pauseButton_);

	stopButton_ = new QPushButton(this);
	stopButton_->setIcon(uiIcon(Glyph::Stop, 14));
	stopButton_->setCursor(Qt::PointingHandCursor);
	stopButton_->setFixedSize(32, 32);
	stopButton_->setStyleSheet(btnBase + QStringLiteral("QPushButton{background:#e5484d;}"
							    "QPushButton:hover{background:#f05055;}"
							    "QPushButton:disabled{background:#5a2f31;}"));
	row->addWidget(stopButton_);

	regionLockButton_ = new QPushButton(this);
	regionLockButton_->setObjectName(QStringLiteral("recordingRegionLock"));
	regionLockButton_->setCheckable(true);
	regionLockButton_->setCursor(Qt::PointingHandCursor);
	regionLockButton_->setFixedSize(32, 32);
	regionLockButton_->setStyleSheet(btnBase + QStringLiteral(
		"QPushButton{background:#3a3f47;}QPushButton:hover{background:#474d57;}"
		"QPushButton:checked{background:#3d65a4;}QPushButton:checked:hover{background:#4776bd;}"
		"QPushButton:disabled{background:#2a2d33;}"));
	updateRegionLockAppearance();
	row->addWidget(regionLockButton_);

	// The zoom chip lives at the right-hand end, after the buttons: it is a
	// readout, not a control, and putting it before them would shove the
	// buttons sideways every time the zoom went on or off.
	zoomChip_ = new QLabel(this);
	zoomChip_->setAlignment(Qt::AlignCenter);
	zoomChip_->setStyleSheet(QStringLiteral(
		"QLabel{color:#0d1117;background:#e5c04a;border-radius:9px;padding:0 8px;"
		"font-size:12px;font-weight:bold;}"));
	zoomChip_->setFixedHeight(32);
	zoomChip_->setVisible(false);
	zoomChip_->installEventFilter(this);
	row->addWidget(zoomChip_);

	spotChip_ = new QLabel(this);
	spotChip_->setAlignment(Qt::AlignCenter);
	spotChip_->setStyleSheet(QStringLiteral(
		"QLabel{color:#0d1117;background:#8ab4f8;border-radius:9px;padding:0 8px;"
		"font-size:12px;font-weight:bold;}"));
	spotChip_->setFixedHeight(32);
	spotChip_->setText(QStringLiteral("Spot"));
	spotChip_->setToolTip(QStringLiteral("The spotlight is on. Press the spotlight shortcut "
					     "again to turn it off."));
	spotChip_->setVisible(false);
	spotChip_->installEventFilter(this);
	row->addWidget(spotChip_);

	followChip_ = new QLabel(this);
	followChip_->setAlignment(Qt::AlignCenter);
	followChip_->setStyleSheet(QStringLiteral(
		"QLabel{color:#0d1117;background:#9aa3b0;border-radius:9px;padding:0 8px;"
		"font-size:12px;font-weight:bold;}"));
	followChip_->setFixedHeight(32);
	followChip_->setText(QStringLiteral("Follow off"));
	followChip_->setToolTip(QStringLiteral("The region has stopped following the mouse. Press "
					       "the follow shortcut again to resume."));
	followChip_->setVisible(false);
	followChip_->installEventFilter(this);
	row->addWidget(followChip_);

	connect(startButton_, &QPushButton::clicked, this, &RecorderControlsOverlay::startClicked);
	connect(pauseButton_, &QPushButton::clicked, this, &RecorderControlsOverlay::pauseClicked);
	connect(stopButton_, &QPushButton::clicked, this, &RecorderControlsOverlay::stopClicked);
	connect(regionLockButton_, &QPushButton::toggled, this, [this](bool locked) {
		updateRegionLockAppearance();
		emit regionLockToggled(locked);
	});

	// Hover tracking must include the child buttons, otherwise entering a button
	// would read as "left the panel" and fade it out.
	startButton_->installEventFilter(this);
	pauseButton_->installEventFilter(this);
	stopButton_->installEventFilter(this);
	regionLockButton_->installEventFilter(this);

	// Idle is the state it now opens in, so start there rather than showing all
	// three for the first instant.
	setState(false, false, false, false);

	fade_ = new QPropertyAnimation(this, "windowOpacity", this);
	fade_->setDuration(180);

	setWindowOpacity(kIdleOpacity);
	adjustSize();
}

void RecorderControlsOverlay::setState(bool recording, bool paused, bool pauseEnabled, bool stopEnabled)
{
	// This is driven from updateButtons(), which runs on a timer, so the
	// expensive half -- swapping which buttons exist and resizing the window
	// around them -- happens only when the answer actually changed. Otherwise
	// the panel would re-lay itself out several times a second forever.
	// The first call always applies: the buttons start out as freshly-built
	// children, which are neither shown nor hidden, so "nothing changed" would
	// leave all three of them on screen.
	const bool layoutChanged = !stateApplied_ || recording != recording_;
	const bool dotChanged = !stateApplied_ || recording != recording_;
	stateApplied_ = true;
	paused_ = paused;
	recording_ = recording;

	if (layoutChanged) {
		// One button when there is one thing to do. Hidden, not disabled: two
		// dead buttons parked on the desktop for as long as the app is running
		// would be clutter that never does anything.
		startButton_->setVisible(!recording);
		pauseButton_->setVisible(recording);
		stopButton_->setVisible(recording);
	}

	pauseButton_->setIcon(uiIcon(paused ? Glyph::Play : Glyph::Pause, 14));
	pauseButton_->setToolTip(paused ? QStringLiteral("Resume recording") : QStringLiteral("Pause recording"));
	pauseButton_->setEnabled(pauseEnabled);
	stopButton_->setToolTip(QStringLiteral("Stop recording"));
	stopButton_->setEnabled(stopEnabled);
	updateRegionLockButton();

	// The pill has to follow its contents, or it keeps the two-button width with
	// one button rattling around inside it.
	if (layoutChanged)
		adjustSize();
	if (dotChanged)
		update(); // the recording dot appears and disappears with the state
}

void RecorderControlsOverlay::setRegionLock(bool available, bool locked)
{
	regionLockAvailable_ = available;
	if (regionLockButton_->isChecked() != locked) {
		QSignalBlocker b(regionLockButton_);
		regionLockButton_->setChecked(locked);
		updateRegionLockAppearance();
	}
	updateRegionLockButton();
}

void RecorderControlsOverlay::updateRegionLockButton()
{
	const bool visible = recording_ && regionLockAvailable_;
	const bool changed = !regionLockButton_->isHidden() != visible;
	regionLockButton_->setVisible(visible);
	regionLockButton_->setEnabled(visible && stopButton_->isEnabled());
	if (changed) {
		adjustSize();
		keepRegionLockVisible();
	}
}

void RecorderControlsOverlay::keepRegionLockVisible()
{
	if (!isVisible() || regionLockButton_->isHidden())
		return;
	// Adding the third button can grow a pill parked against the right edge.
	// Keep it on that display without restoring or rewriting its saved spot.
	const QRect frame = frameGeometry();
	QScreen *targetScreen = QGuiApplication::screenAt(frame.topLeft());
	if (!targetScreen)
		targetScreen = screen();
	if (!targetScreen)
		return;
	const QRect available = targetScreen->availableGeometry();
	const QPoint clamped(std::clamp(frame.left(), available.left(),
					std::max(available.left(), available.right() - frame.width() + 1)),
			     std::clamp(frame.top(), available.top(),
					std::max(available.top(), available.bottom() - frame.height() + 1)));
	if (clamped != frame.topLeft())
		move(pos() + clamped - frame.topLeft());
}

void RecorderControlsOverlay::updateRegionLockAppearance()
{
	const bool locked = regionLockButton_->isChecked();
	static const QIcon openIcon = regionLockIcon(false);
	static const QIcon closedIcon = regionLockIcon(true);
	regionLockButton_->setIcon(locked ? closedIcon : openIcon);
	regionLockButton_->setAccessibleName(locked ? QStringLiteral("Unlock recording area")
						  : QStringLiteral("Lock recording area"));
	regionLockButton_->setToolTip(locked
		? QStringLiteral("Mouse following is locked. Drag the grip above the recording area to reposition it. "
				 "Click to unlock and resume following the mouse.")
		: QStringLiteral("Lock the recording area at its current position while you move the mouse."));
}

void RecorderControlsOverlay::setZoom(bool zoomed, int percent)
{
	if (!zoomChip_)
		return;
	// Driven from the same timer-backed update as setState, so guard on an
	// actual change: relaying the pill out sixty times a second would make it
	// twitch under the cursor.
	const QString text = QStringLiteral("%1×").arg(percent / 100.0, 0, 'g', 2);
	const bool changed = zoomChip_->isVisible() != zoomed || (zoomed && zoomChip_->text() != text);
	if (!changed)
		return;
	zoomChip_->setText(text);
	zoomChip_->setToolTip(QStringLiteral("The recording is zoomed to %1%. Press the zoom "
					     "shortcut again to zoom out.")
				      .arg(percent));
	zoomChip_->setVisible(zoomed);
	adjustSize();
}

void RecorderControlsOverlay::setSpotlight(bool lit)
{
	if (!spotChip_ || spotChip_->isVisible() == lit)
		return;
	spotChip_->setVisible(lit);
	adjustSize();
}

void RecorderControlsOverlay::setFollowPaused(bool paused)
{
	if (!followChip_ || followChip_->isVisible() == paused)
		return;
	followChip_->setVisible(paused);
	adjustSize();
}

void RecorderControlsOverlay::showControls()
{
	if (isVisible())
		return;
	restorePosition();
	setWindowOpacity(kIdleOpacity);
	show();
	raise();
	keepRegionLockVisible();
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
	if ((obj == startButton_ || obj == pauseButton_ || obj == stopButton_ || obj == regionLockButton_) &&
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

bool RecorderControlsOverlay::onlyWhileRecording() const
{
	return hudSettings().value(QStringLiteral("floatingControls/onlyWhileRecording"), false).toBool();
}

void RecorderControlsOverlay::contextMenuEvent(QContextMenuEvent *e)
{
	// The panel is on screen for as long as the app is, so there has to be a
	// way to put it away that is not "quit Harpia" -- a one-shot ("until next
	// recording") and a permanent policy ("only show while recording", which
	// is the pre-Record-button behaviour, remembered across sessions).
	QMenu menu;
	QAction *hideAct = menu.addAction(QStringLiteral("Hide until next recording"));
	QAction *onlyRec = menu.addAction(QStringLiteral("Only show while recording"));
	onlyRec->setCheckable(true);
	onlyRec->setChecked(onlyWhileRecording());
	QAction *chosen = menu.exec(e->globalPos());
	if (chosen == hideAct) {
		emit dismissed();
	} else if (chosen == onlyRec) {
		QSettings s = hudSettings();
		s.setValue(QStringLiteral("floatingControls/onlyWhileRecording"), onlyRec->isChecked());
		emit visibilityPolicyChanged();
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

	// Recording indicator dot on the left -- only while there is a recording to
	// indicate. A red dot sitting there while idle would say the opposite of the
	// truth, which for a recorder is the one thing this must never do. The space
	// it occupies stays reserved either way, so the pill does not jump.
	if (recording_) {
		const int cy = height() / 2;
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0xe5, 0x48, 0x4d));
		p.drawEllipse(QPoint(14, cy), 4, 4);
	}
}

} // namespace harpia
