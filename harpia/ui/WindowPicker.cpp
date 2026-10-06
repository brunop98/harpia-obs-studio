#include "WindowPicker.hpp"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QWidget>

#include <cmath>

namespace harpia {

// One screen's layer. Not a separate header: nothing but the picker uses it.
class WindowPickerLayer : public QWidget {
public:
	WindowPickerLayer(WindowPicker *owner, QScreen *screen, const QRect &physical)
		: QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool),
		  owner_(owner), physical_(physical)
	{
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_DeleteOnClose);
		setMouseTracking(true);
		setCursor(Qt::CrossCursor);
		setFocusPolicy(Qt::StrongFocus);
		if (screen) {
			setScreen(screen);
			setGeometry(screen->geometry());
			dpr_ = screen->devicePixelRatio();
		}
		if (physical_.isNull()) // no physical rectangle: assume the logical one, scaled
			physical_ = QRect(geometry().topLeft() * dpr_, geometry().size() * dpr_);
	}

	// A point on this layer (logical, local) as physical desktop px, and back.
	QPoint toPhysical(const QPointF &local) const
	{
		return physical_.topLeft() + QPoint(int(std::lround(local.x() * dpr_)), int(std::lround(local.y() * dpr_)));
	}
	QRectF toLocal(const QRect &phys) const
	{
		return QRectF((phys.x() - physical_.x()) / dpr_, (phys.y() - physical_.y()) / dpr_, phys.width() / dpr_,
			      phys.height() / dpr_);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		// A light veil over everything, cut away where the window is, so the
		// window being pointed at is the one thing left at full brightness.
		QPainterPath veil;
		veil.addRect(QRectF(rect()));
		const int h = owner_ ? owner_->hovered_ : -1;
		QRectF win;
		if (owner_ && h >= 0 && h < owner_->windows_.size()) {
			win = toLocal(owner_->windows_[h].bounds).intersected(QRectF(rect()));
			if (!win.isEmpty()) {
				QPainterPath hole;
				hole.addRect(win);
				veil = veil.subtracted(hole);
			}
		}
		p.fillPath(veil, QColor(0, 0, 0, 70));
		if (!win.isEmpty()) {
			p.setPen(QPen(QColor(0x3d, 0x7e, 0xff), 3.0));
			p.setBrush(QColor(0x3d, 0x7e, 0xff, 28));
			p.drawRect(win.adjusted(1.5, 1.5, -1.5, -1.5));
			// Name and size, in a tag at the top-left inside the window.
			const DesktopWindow &w = owner_->windows_[h];
			const QString name = w.title.isEmpty() ? QStringLiteral("Window") : w.title;
			const QString label = QStringLiteral("%1   %2 × %3")
						      .arg(fontMetrics().elidedText(name, Qt::ElideRight, 360))
						      .arg(w.bounds.width())
						      .arg(w.bounds.height());
			const QRect tag = fontMetrics().boundingRect(label).adjusted(-8, -4, 8, 4);
			QRectF at(win.topLeft() + QPointF(8, 8), QSizeF(tag.size()));
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0x3d, 0x7e, 0xff));
			p.drawRoundedRect(at, 4, 4);
			p.setPen(Qt::white);
			p.drawText(at, Qt::AlignCenter, label);
		}
		// What to do, at the top of each screen.
		const QString help = QStringLiteral("Click a window to record it   ·   Esc to cancel");
		QFont f = font();
		f.setPointSizeF(f.pointSizeF() * 1.15);
		f.setBold(true);
		p.setFont(f);
		const QRect box = QFontMetrics(f).boundingRect(help).adjusted(-14, -7, 14, 7);
		const QRectF hb(QPointF((width() - box.width()) / 2.0, 24), QSizeF(box.size()));
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(20, 22, 26, 220));
		p.drawRoundedRect(hb, 6, 6);
		p.setPen(QColor(0xe8, 0xea, 0xed));
		p.drawText(hb, Qt::AlignCenter, help);
	}

	void mouseMoveEvent(QMouseEvent *e) override
	{
		if (owner_)
			owner_->hoverPhysical(toPhysical(e->position()));
	}

	void mousePressEvent(QMouseEvent *e) override
	{
		if (!owner_)
			return;
		if (e->button() == Qt::LeftButton)
			owner_->pickPhysical(toPhysical(e->position()));
		else
			owner_->cancel();
	}

	void keyPressEvent(QKeyEvent *e) override
	{
		if (e->key() == Qt::Key_Escape && owner_)
			owner_->cancel();
		else
			QWidget::keyPressEvent(e);
	}

	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		activateWindow();
		setFocus();
	}

private:
	QPointer<WindowPicker> owner_;
	QRect physical_;
	double dpr_ = 1.0;
};

WindowPicker::WindowPicker(QObject *parent) : QObject(parent) {}

WindowPicker::~WindowPicker()
{
	close();
}

void WindowPicker::start()
{
	QVector<ScreenArea> screens;
	for (QScreen *s : QGuiApplication::screens())
		screens.push_back({s, window_list::monitorRect(s->name())});
	startWith(window_list::windows(), screens);
}

void WindowPicker::startWith(const QVector<DesktopWindow> &topmostFirst, const QVector<ScreenArea> &screens)
{
	close();
	windows_ = topmostFirst;
	hovered_ = -1;
	for (const ScreenArea &s : screens) {
		auto *layer = new WindowPickerLayer(this, s.screen, s.physical);
		layers_.push_back(layer);
		layer->show();
		layer->raise();
	}
	// Start with whatever is under the pointer already, so the first outline
	// does not wait for the first move.
	if (const auto c = window_list::cursorPos())
		hoverPhysical(*c);
}

QVector<QWidget *> WindowPicker::layersForTest() const
{
	QVector<QWidget *> out;
	for (const auto &l : layers_)
		if (l)
			out.push_back(l);
	return out;
}

void WindowPicker::hoverPhysical(const QPoint &p)
{
	const int h = window_region::windowAt(windows_, p);
	if (h == hovered_)
		return;
	hovered_ = h;
	for (const auto &l : layers_)
		if (l)
			l->update();
}

void WindowPicker::pickPhysical(const QPoint &p)
{
	const int h = window_region::windowAt(windows_, p);
	if (h < 0)
		return; // nothing there to pick: keep picking
	const DesktopWindow w = windows_[h];
	close();
	emit picked(w);
}

void WindowPicker::cancel()
{
	if (!active())
		return;
	close();
	emit cancelled();
}

void WindowPicker::close()
{
	for (const auto &l : layers_)
		if (l)
			l->close(); // WA_DeleteOnClose
	layers_.clear();
	hovered_ = -1;
}

} // namespace harpia
