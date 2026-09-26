#pragma once

// A list you can drag files out of.
//
// QListWidget's own drag carries an internal item encoding that nothing
// outside the list understands. The timeline and the editor window accept
// dropped FILES (text/uri-list), the same thing Explorer hands them -- so a
// list of sources or library clips only needs to say which file each row is,
// and dragging a row becomes the same gesture as dragging the file from a
// folder. The path lives in `PathRole` on the item.

#include <QListWidget>
#include <QMimeData>
#include <QUrl>

namespace harpia {

class UrlDragList : public QListWidget {
public:
	static constexpr int PathRole = Qt::UserRole + 100;

	explicit UrlDragList(QWidget *parent = nullptr) : QListWidget(parent)
	{
		setDragEnabled(true);
		setDragDropMode(QAbstractItemView::DragOnly);
		setDefaultDropAction(Qt::CopyAction);
	}

protected:
	QStringList mimeTypes() const override { return {QStringLiteral("text/uri-list")}; }

	QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override
	{
		QList<QUrl> urls;
		for (const QListWidgetItem *it : items) {
			const QString p = it->data(PathRole).toString();
			if (!p.isEmpty())
				urls << QUrl::fromLocalFile(p);
		}
		if (urls.isEmpty())
			return nullptr; // nothing to drag: Qt then starts no drag at all
		auto *md = new QMimeData;
		md->setUrls(urls);
		return md;
	}
};

} // namespace harpia
