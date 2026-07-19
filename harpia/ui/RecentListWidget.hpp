#pragma once

#include <QApplication>
#include <QDrag>
#include <QIcon>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPixmap>
#include <QPoint>
#include <QUrl>

namespace harpia {

// Path stored on each item so drags can carry the real file.
inline constexpr int kClipPathRole = Qt::UserRole + 1;

// A QListWidget whose items can be dragged out into other applications (file
// managers, editors, chat apps) as real files. It exposes the selected items'
// paths as a text/uri-list (file:// URLs) — the format the OS drag-and-drop
// machinery expects for "here is a file" — and starts the drag explicitly so it
// works reliably in IconMode with a custom delegate, as a Copy action, and with
// a drag preview pixmap.
class RecentListWidget : public QListWidget {
	Q_OBJECT
public:
	explicit RecentListWidget(QWidget *parent = nullptr) : QListWidget(parent)
	{
		setDragEnabled(true);
		setDragDropMode(QAbstractItemView::DragOnly);
		setDefaultDropAction(Qt::CopyAction);
		setSelectionMode(QAbstractItemView::ExtendedSelection);
	}

protected:
	// Build a text/uri-list of the selected recordings.
	QMimeData *mimeForItems(const QList<QListWidgetItem *> &items) const
	{
		QList<QUrl> urls;
		for (const QListWidgetItem *item : items) {
			const QString path = item->data(kClipPathRole).toString();
			if (!path.isEmpty())
				urls << QUrl::fromLocalFile(path);
		}
		if (urls.isEmpty())
			return nullptr;
		auto *mime = new QMimeData();
		mime->setUrls(urls);
		return mime;
	}

	QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override
	{
		QMimeData *mime = mimeForItems(items);
		return mime ? mime : new QMimeData();
	}

	// Record where a left-press landed so we can start a drag once the pointer
	// moves past the drag threshold.
	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton)
			pressPos_ = event->pos();
		QListWidget::mousePressEvent(event);
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (!(event->buttons() & Qt::LeftButton) || pressPos_.isNull()) {
			QListWidget::mouseMoveEvent(event);
			return;
		}
		if ((event->pos() - pressPos_).manhattanLength() < QApplication::startDragDistance()) {
			QListWidget::mouseMoveEvent(event);
			return;
		}
		// Only drag when the press landed on an item.
		if (!itemAt(pressPos_)) {
			QListWidget::mouseMoveEvent(event);
			return;
		}
		startDrag(Qt::CopyAction);
		pressPos_ = QPoint();
	}

	// Explicit, robust file drag (used instead of the view's default path).
	void startDrag(Qt::DropActions) override
	{
		const QList<QListWidgetItem *> items = selectedItems();
		QMimeData *mime = mimeForItems(items);
		if (!mime)
			return;

		auto *drag = new QDrag(this);
		drag->setMimeData(mime);
		if (QListWidgetItem *cur = currentItem()) {
			const QIcon ic = cur->icon();
			if (!ic.isNull())
				drag->setPixmap(ic.pixmap(120, 68));
		}
		drag->exec(Qt::CopyAction, Qt::CopyAction);
	}

private:
	QPoint pressPos_;
};

} // namespace harpia
