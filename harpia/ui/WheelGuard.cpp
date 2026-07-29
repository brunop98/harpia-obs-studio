#include "WheelGuard.hpp"

#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QWheelEvent>
#include <QWidget>

namespace harpia {

WheelGuard::WheelGuard(QWidget *root, QObject *parent) : QObject(parent), root_(root)
{
	if (qApp)
		qApp->installEventFilter(this);
}

namespace {

// A control the wheel must not edit. Sliders, spin boxes and combo boxes all
// treat the wheel as a value change; a scroll bar does NOT, and must keep
// working -- dragging the panel's own scroll bar with the wheel over it is the
// obvious thing to do.
bool isValueControl(QWidget *w)
{
	if (qobject_cast<QAbstractSlider *>(w) && !w->inherits("QScrollBar"))
		return true;
	return qobject_cast<QAbstractSpinBox *>(w) || qobject_cast<QComboBox *>(w);
}

} // namespace

bool WheelGuard::eventFilter(QObject *watched, QEvent *event)
{
	if (event->type() != QEvent::Wheel || !root_)
		return QObject::eventFilter(watched, event);
	auto *w = qobject_cast<QWidget *>(watched);
	if (!w || !isValueControl(w))
		return QObject::eventFilter(watched, event);
	// Only inside the guarded panel. The same spin box in a dialog keeps its
	// ordinary behaviour, and the preview's wheel-to-zoom is untouched.
	if (!root_->isAncestorOf(w))
		return QObject::eventFilter(watched, event);

	// The nearest enclosing scroll area is the one that should move. Nearest,
	// not outermost: a control inside a nested list scrolls that list.
	QWidget *viewport = nullptr;
	for (QWidget *a = w->parentWidget(); a; a = a->parentWidget()) {
		if (auto *sa = qobject_cast<QAbstractScrollArea *>(a)) {
			viewport = sa->viewport();
			break;
		}
	}

	// Hand the scroll on so the gesture still does what the user meant --
	// swallowing it outright would leave the wheel dead over half the panel.
	//
	// Positioned at the viewport's own centre rather than mapped from the
	// control: a scroll area reads only the delta, and mapFrom() requires an
	// ancestor relationship that is not guaranteed for every widget Qt puts
	// under the pointer (it segfaulted on the spin box's internal editor).
	if (viewport) {
		auto *we = static_cast<QWheelEvent *>(event);
		const QPointF at(viewport->rect().center());
		QWheelEvent fwd(at, viewport->mapToGlobal(at), we->pixelDelta(), we->angleDelta(),
				we->buttons(), we->modifiers(), we->phase(), we->inverted(),
				we->source());
		QApplication::sendEvent(viewport, &fwd);
	}
	return true; // handled: the control never sees it
}

} // namespace harpia
