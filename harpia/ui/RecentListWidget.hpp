#pragma once

#include <QListWidget>
#include <QMimeData>
#include <QUrl>

namespace harpia {

// Path stored on each item so drags can carry the real file.
inline constexpr int kClipPathRole = Qt::UserRole + 1;

// A QListWidget whose items can be dragged out into other applications (file
// managers, editors, chat apps) as real files. It does this by exposing the
// selected items' paths as a text/uri-list (file:// URLs) — the format the OS
// drag-and-drop machinery expects for "here is a file".
class RecentListWidget : public QListWidget {
	Q_OBJECT
public:
	explicit RecentListWidget(QWidget *parent = nullptr) : QListWidget(parent)
	{
		setDragEnabled(true);
		setDragDropMode(QAbstractItemView::DragOnly);
		setSelectionMode(QAbstractItemView::ExtendedSelection);
	}

protected:
	QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override
	{
		QList<QUrl> urls;
		for (const QListWidgetItem *item : items) {
			const QString path = item->data(kClipPathRole).toString();
			if (!path.isEmpty())
				urls << QUrl::fromLocalFile(path);
		}
		auto *mime = new QMimeData();
		mime->setUrls(urls);
		return mime;
	}
};

} // namespace harpia
