#pragma once

#include <QApplication>
#include <QKeyEvent>
#include <QMenu>

namespace harpia::test_menu {

// Navigate the same one-level submenu a user opens before choosing an action.
inline QMenu *openSubmenu(QMenu *menu, const QString &title)
{
	for (QAction *action : menu->actions())
		if (QMenu *submenu = action->menu(); submenu && submenu->title() == title) {
			menu->setActiveAction(action);
			QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
			QApplication::sendEvent(menu, &right);
			return submenu;
		}
	return menu;
}

inline void dismiss(QMenu *menu)
{
	while (auto *parent = qobject_cast<QMenu *>(menu->parentWidget()))
		menu = parent;
	menu->close();
}

} // namespace harpia::test_menu
