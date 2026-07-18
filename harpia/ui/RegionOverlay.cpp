#include "RegionOverlay.hpp"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>

namespace harpia {

// ------------------------- RegionSelectDialog -------------------------

RegionSelectDialog::RegionSelectDialog(QScreen *screen, QWidget *parent) : QDialog(parent)
{
	setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
	setCursor(Qt::CrossCursor);

	if (!screen)
		screen = QGuiApplication::primaryScreen();
	if (screen) {
		dpr_ = screen->devicePixelRatio();
		// Position/size the dialog to exactly cover the chosen screen; event
		// coordinates are then relative to that screen's top-left.
		setGeometry(screen->geometry());
	}
}

QRect RegionSelectDialog::selectionRectLogical() const
{
	return QRect(origin_, current_).normalized();
}

void RegionSelectDialog::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	// Dim the whole screen; punch the selection out so the user sees what will
	// be captured.
	p.fillRect(rect(), QColor(0, 0, 0, 110));

	if (selecting_) {
		const QRect sel = selectionRectLogical();
		p.setCompositionMode(QPainter::CompositionMode_Clear);
		p.fillRect(sel, Qt::transparent);
		p.setCompositionMode(QPainter::CompositionMode_SourceOver);

		QPen pen(QColor(0, 174, 239), 2);
		p.setPen(pen);
		p.drawRect(sel);

		const QString label = QStringLiteral("%1 × %2")
					      .arg(int(sel.width() * dpr_))
					      .arg(int(sel.height() * dpr_));
		p.setPen(Qt::white);
		p.drawText(sel.adjusted(4, 4, -4, -4), Qt::AlignTop | Qt::AlignLeft, label);
	} else {
		p.setPen(Qt::white);
		p.drawText(rect(), Qt::AlignCenter,
			   QStringLiteral("Drag to select a recording region · Esc to cancel"));
	}
}

void RegionSelectDialog::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton) {
		origin_ = event->pos();
		current_ = event->pos();
		selecting_ = true;
		update();
	}
}

void RegionSelectDialog::mouseMoveEvent(QMouseEvent *event)
{
	if (selecting_) {
		current_ = event->pos();
		update();
	}
}

void RegionSelectDialog::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton || !selecting_)
		return;

	const QRect sel = selectionRectLogical();
	if (sel.width() < 8 || sel.height() < 8) {
		// Too small — treat as a cancel.
		reject();
		return;
	}

	// Convert logical widget coordinates to device pixels for the capture canvas.
	region_.enabled = true;
	region_.x = int(sel.x() * dpr_);
	region_.y = int(sel.y() * dpr_);
	region_.width = int(sel.width() * dpr_);
	region_.height = int(sel.height() * dpr_);
	accept();
}

void RegionSelectDialog::keyPressEvent(QKeyEvent *event)
{
	if (event->key() == Qt::Key_Escape)
		reject();
	else
		QDialog::keyPressEvent(event);
}

// ---------------------------- RegionOverlay ----------------------------

RegionOverlay::RegionOverlay(QWidget *parent) : QWidget(parent)
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
		       Qt::WindowTransparentForInput);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_TransparentForMouseEvents);
}

void RegionOverlay::setRegion(const CaptureRegion &region, QScreen *screen)
{
	if (!region.enabled || region.width <= 0 || region.height <= 0 || !screen) {
		hide();
		return;
	}

	const qreal dpr = screen->devicePixelRatio();
	const QPoint origin = screen->geometry().topLeft(); // logical global coords

	// Device pixels (relative to the screen) -> logical global coordinates, with
	// a small margin so the border sits just outside the captured area.
	const int margin = 2;
	const int x = origin.x() + int(region.x / dpr) - margin;
	const int y = origin.y() + int(region.y / dpr) - margin;
	const int w = int(region.width / dpr) + margin * 2;
	const int h = int(region.height / dpr) + margin * 2;
	setGeometry(x, y, w, h);
	show();
	update();
}

void RegionOverlay::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	QPen pen(QColor(0, 174, 239), 2);
	p.setPen(pen);
	p.drawRect(rect().adjusted(1, 1, -1, -1));
}

} // namespace harpia
