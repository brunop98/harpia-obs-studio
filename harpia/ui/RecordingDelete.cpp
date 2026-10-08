#include "ui/RecordingDelete.hpp"

#include "ui/RecentListWidget.hpp"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QTextEdit>

namespace harpia::recording_delete {

QStringList selectedPaths(const QListWidget *list)
{
	QStringList paths;
	if (!list)
		return paths;
	// selectedItems() is in selection order; the list's own order reads better
	// in "Move 3 recordings ..." and is what a test can predict.
	for (int i = 0; i < list->count(); ++i) {
		const QListWidgetItem *item = list->item(i);
		if (!item->isSelected() || item->isHidden())
			continue;
		const QString p = item->data(kClipPathRole).toString();
		if (!p.isEmpty())
			paths << p;
	}
	return paths;
}

bool focusEditsText(const QWidget *focus)
{
	if (!focus)
		return false;
	if (const auto *le = qobject_cast<const QLineEdit *>(focus))
		return !le->isReadOnly();
	if (const auto *cb = qobject_cast<const QComboBox *>(focus))
		return cb->isEditable();
	return qobject_cast<const QAbstractSpinBox *>(focus) || qobject_cast<const QPlainTextEdit *>(focus) ||
	       qobject_cast<const QTextEdit *>(focus) || qobject_cast<const QKeySequenceEdit *>(focus);
}

QString confirmText(const QStringList &paths)
{
	if (paths.size() == 1)
		return QStringLiteral("Move \"%1\" to the recycle bin?").arg(QFileInfo(paths.front()).fileName());
	return QStringLiteral("Move %1 recordings to the recycle bin?").arg(paths.size());
}

bool recycle(const QString &path)
{
	QFile f(path);
	if (!f.exists())
		return true; // already gone: nothing left to fail at
	if (f.moveToTrash())
		return true;
	return f.remove();
}

QStringList confirmAndRecycle(QWidget *parent, const QStringList &paths)
{
	if (paths.isEmpty())
		return {};
	if (QMessageBox::question(parent, QStringLiteral("Delete"), confirmText(paths)) != QMessageBox::Yes)
		return {};
	QStringList gone, failed;
	for (const QString &p : paths)
		(recycle(p) ? gone : failed) << p;
	if (!failed.isEmpty()) {
		QStringList names;
		for (const QString &p : failed)
			names << QFileInfo(p).fileName();
		// Usually a player or another program still has the file open.
		QMessageBox::warning(parent, QStringLiteral("Delete"),
				     QStringLiteral("Could not delete:\n%1\n\nIs it open in another program?")
					     .arg(names.join(QLatin1Char('\n'))));
	}
	return gone;
}

QShortcut *installDeleteKey(QWidget *window, QListWidget *list,
			    std::function<void(const QStringList &)> onDelete)
{
	auto *sc = new QShortcut(QKeySequence(QKeySequence::Delete), window);
	// The whole window, not just the list: a card stays selected while you go
	// and click something else, and Delete should still mean that card.
	sc->setContext(Qt::WindowShortcut);
	QObject::connect(sc, &QShortcut::activated, window, [list, onDelete = std::move(onDelete)]() {
		// Text fields normally claim Delete before a shortcut sees it
		// (ShortcutOverride); checked anyway, as removing a recording while
		// the user is editing a name is not a mistake worth risking.
		if (focusEditsText(QApplication::focusWidget()))
			return;
		const QStringList paths = selectedPaths(list);
		if (!paths.isEmpty() && onDelete)
			onDelete(paths);
	});
	return sc;
}

} // namespace harpia::recording_delete
