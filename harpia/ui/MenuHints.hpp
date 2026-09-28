#pragma once

// Why is this menu entry greyed?
//
// A disabled entry that says nothing is a dead end: the command is right
// there, it will not go, and the menu offers no clue. These helpers make a
// greyed entry answer when it is clicked (and, more slowly, when hovered):
// a small tooltip at the pointer saying why it is off and what would turn
// it on.
//
//   QMenu menu(this);
//   QAction *fit = menu.addAction("Fit to whole timeline");
//   disableBecause(fit, end > 0, "There is no video or audio clip to measure the "
//                                "timeline by. Add footage first.");
//   explainDisabled(&menu);
//   menu.exec(pos);
//
// disableBecause() is setEnabled() with a reason attached; the reason is
// only shown while the entry is off. explainDisabled() installs the click
// handler on the menu and turns hover tooltips on.

#include <QAction>
#include <QEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QObject>
#include <QToolTip>
#include <QVariant>

namespace harpia {

inline const char *kWhyDisabledProp = "harpiaWhyDisabled";

// setEnabled(enabled), and when disabled, remember why. The reason becomes
// the entry's tooltip so it shows on hover as well as on click.
inline void disableBecause(QAction *a, bool enabled, const QString &why)
{
	if (!a)
		return;
	a->setEnabled(enabled);
	if (!enabled) {
		a->setProperty(kWhyDisabledProp, why);
		a->setToolTip(why);
	}
}

// Watches a menu: a click on a greyed entry shows its reason at the pointer.
// A disabled QAction cannot be triggered, so the menu stays open and the
// tooltip sits next to the entry it explains.
class MenuHints : public QObject {
public:
	explicit MenuHints(QMenu *menu) : QObject(menu), menu_(menu) {}

	bool eventFilter(QObject *o, QEvent *e) override
	{
		if (o == menu_ && (e->type() == QEvent::MouseButtonPress ||
				   e->type() == QEvent::MouseButtonRelease)) {
			auto *me = static_cast<QMouseEvent *>(e);
			QAction *a = menu_->actionAt(me->pos());
			if (a && !a->isSeparator() && !a->isEnabled()) {
				QString why = a->property(kWhyDisabledProp).toString();
				if (why.isEmpty())
					why = a->toolTip();
				if (why.isEmpty())
					why = QStringLiteral("This is not available right now.");
				if (e->type() == QEvent::MouseButtonRelease)
					QToolTip::showText(me->globalPosition().toPoint(), why, menu_,
							   menu_->actionGeometry(a), 8000);
				return true; // the click is answered, not swallowed by the menu
			}
		}
		return QObject::eventFilter(o, e);
	}

private:
	QMenu *menu_;
};

inline void explainDisabled(QMenu *menu)
{
	if (!menu)
		return;
	menu->setToolTipsVisible(true);
	menu->installEventFilter(new MenuHints(menu));
}

} // namespace harpia
