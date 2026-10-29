#pragma once

#include "library/ThumbnailCache.hpp"
#include "library/VideoFolderScanner.hpp"

#include <QDialog>
#include <QPoint>
#include <QHash>
#include <QSet>
#include <QStringList>

class QLabel;
class QComboBox;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QProgressBar;
class QTabBar;
class QTimer;

namespace harpia {

// Chooses a video from recordings, Downloads or Videos. Folder discovery is
// recursive and asynchronous; thumbnails are requested as cards come into view.
// The caller opens its usual file dialog when Import from folder is chosen.
class RecordingVideoPicker : public QDialog {
	Q_OBJECT
public:
	explicit RecordingVideoPicker(const QStringList &libraryFolders, QWidget *parent = nullptr);
	~RecordingVideoPicker() override;

	QString selectedVideo() const;
	bool importFromFolderRequested() const { return importFromFolder_; }
	void setProtectedPaths(const QStringList &paths);

signals:
	void recordingsDeleted(const QStringList &paths);

protected:
	void showEvent(QShowEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void startScan();
	void appendVideos(const QVector<ClipInfo> &clips);
	void sortVideos();
	void scheduleThumbnails();
	void loadVisibleThumbnails();
	void acceptSelection();
	void updateThumbnail(const QString &path);
	void showContextMenu(const QPoint &pos);
	void updateRecordingCount();

	QPoint openingPosition_;
	bool placed_ = false;
	QListWidget *grid_ = nullptr;
	QTabBar *tabs_ = nullptr;
	QComboBox *orderBy_ = nullptr;
	QLabel *status_ = nullptr;
	QProgressBar *progress_ = nullptr;
	QPushButton *addButton_ = nullptr;
	QTimer *thumbnailTimer_ = nullptr;
	QStringList libraryFolders_;
	ThumbnailCache thumbnails_;
	VideoFolderScanner scanner_;
	QHash<QString, QListWidgetItem *> itemByPath_;
	QSet<QString> checkedThumbnails_;
	QSet<QString> attemptedThumbnails_;
	QSet<QString> pendingThumbnails_;
	QSet<QString> deletedPaths_;
	QSet<QString> protectedPaths_;
	int unavailableRoots_ = 0;
	bool importFromFolder_ = false;
};

} // namespace harpia
