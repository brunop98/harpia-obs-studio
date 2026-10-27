#pragma once

#include "library/ThumbnailCache.hpp"

#include <QDialog>
#include <QHash>
#include <QStringList>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace harpia {

// Chooses a recorded video without modifying the library. The caller opens
// its usual file dialog when Import from folder accepts this picker instead.
class RecordingVideoPicker : public QDialog {
	Q_OBJECT
public:
	explicit RecordingVideoPicker(const QStringList &libraryFolders, QWidget *parent = nullptr);

	QString selectedVideo() const;
	bool importFromFolderRequested() const { return importFromFolder_; }

private:
	void acceptSelection();
	void updateThumbnail(const QString &path);

	QListWidget *grid_ = nullptr;
	QLabel *status_ = nullptr;
	QPushButton *addButton_ = nullptr;
	ThumbnailCache thumbnails_;
	QHash<QString, QListWidgetItem *> itemByPath_;
	bool importFromFolder_ = false;
};

} // namespace harpia
