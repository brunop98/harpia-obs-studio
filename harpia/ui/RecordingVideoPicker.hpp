#pragma once

#include "library/ThumbnailCache.hpp"

#include <QDialog>
#include <QHash>
#include <QSet>
#include <QStringList>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace harpia {

// Chooses a recorded video, with file actions on the thumbnail context menu.
// The caller opens its usual file dialog when Import from folder is chosen.
class RecordingVideoPicker : public QDialog {
	Q_OBJECT
public:
	explicit RecordingVideoPicker(const QStringList &libraryFolders, QWidget *parent = nullptr);

	QString selectedVideo() const;
	bool importFromFolderRequested() const { return importFromFolder_; }
	void setProtectedPaths(const QStringList &paths);

signals:
	void recordingsDeleted(const QStringList &paths);

private:
	void acceptSelection();
	void updateThumbnail(const QString &path);
	void showContextMenu(const QPoint &pos);
	void updateRecordingCount();

	QListWidget *grid_ = nullptr;
	QLabel *status_ = nullptr;
	QPushButton *addButton_ = nullptr;
	ThumbnailCache thumbnails_;
	QHash<QString, QListWidgetItem *> itemByPath_;
	QSet<QString> protectedPaths_;
	bool importFromFolder_ = false;
};

} // namespace harpia
