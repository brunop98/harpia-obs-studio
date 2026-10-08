#pragma once

// Deleting recordings, the one way: from the main window's Recent strip, the
// Clip Library grid, their right-click menus and the Delete key alike.
//
// The strip's menu used to delete outright ("cannot be undone") while the
// library moved files to the recycle bin; the same file got two different
// fates depending on which window you were looking at. Both go to the recycle
// bin now (deleted outright only where there is no bin), behind one question.
//
// The Delete key works whenever recordings are SELECTED in a window -- the
// keyboard can be on a button, a slider or the list itself -- except while you
// are typing, where Delete belongs to the text.

#include <QStringList>

#include <functional>

class QListWidget;
class QShortcut;
class QWidget;

namespace harpia::recording_delete {

// The selected recordings' paths (kClipPathRole), in list order.
QStringList selectedPaths(const QListWidget *list);

// Whether `focus` edits text, so a Delete pressed there is the text's.
bool focusEditsText(const QWidget *focus);

// "Move X to the recycle bin?" for one file, "Move N recordings ..." for more.
QString confirmText(const QStringList &paths);

// One file to the recycle bin, or deleted where there is no bin. False when
// it is still there afterwards.
bool recycle(const QString &path);

// Ask, then recycle each file. Returns the paths that are gone (empty when the
// question was declined); any that could not be removed are named in one
// warning.
QStringList confirmAndRecycle(QWidget *parent, const QStringList &paths);

// The Delete key for `window`: with recordings selected in `list`, wherever the
// keyboard is in that window (but not while typing), calls `onDelete` with
// their paths. The shortcut is owned by `window`.
QShortcut *installDeleteKey(QWidget *window, QListWidget *list,
			    std::function<void(const QStringList &)> onDelete);

} // namespace harpia::recording_delete
