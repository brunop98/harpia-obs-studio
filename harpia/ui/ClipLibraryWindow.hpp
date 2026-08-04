#pragma once

#include "library/ClipLibrary.hpp"
#include "library/ThumbnailCache.hpp"

#include <QHash>
#include <QSet>
#include <QWidget>

class QListWidget;
class QListWidgetItem;
class QLabel;
class QSlider;
class QAbstractItemDelegate;

namespace harpia {

class PresetStore;

// A standalone window listing every recording that still exists on disk as a
// grid of cards. Each card shows a 16:9 thumbnail with the duration overlaid, a
// GIF badge for GIFs, a favorite star, a recently-viewed heart, the (elided)
// file name, the exact date + size, and the originating preset. A header shows
// totals; a slider scales the cards Small/Medium/Large. Multi-select with
// Ctrl/Shift, drag out to a file manager, right-click for Open/Copy/Rename/
// Delete, and the Delete key removes the selected clips.
class ClipLibraryWindow : public QWidget {
	Q_OBJECT
public:
	explicit ClipLibraryWindow(PresetStore &store, QWidget *parent = nullptr);

signals:
	// "Extract Audio Only". Raised rather than handled here: the main window
	// already owns the one launcher, so both file menus reach the same code and
	// cannot drift apart on what the item does.
	void extractAudioRequested(const QString &path);

public slots:
	void refresh();

protected:
	void showEvent(QShowEvent *event) override;

private slots:
	void showContextMenu(const QPoint &pos);
	void openSelected();
	void copySelected();
	void renameSelected();
	void deleteSelected();
	void toggleFavoriteSelected();
	void onThumbnailReady(const QString &path);
	void onCardScaleChanged(int level);

private:
	QStringList folders() const;
	ClipLibrary::PresetByFolder presetByFolder() const;
	QStringList selectedPaths() const;
	QSize thumbSize() const;    // 16:9 thumbnail at the current scale
	void applyCardMetrics();    // push icon/grid sizes for the current scale
	void markViewed(const QString &path);
	void probeDurationAsync(const QString &path);
	void loadPersistentState();
	void savePersistentState();

	PresetStore &store_;
	QListWidget *grid_ = nullptr;
	QAbstractItemDelegate *cardDelegate_ = nullptr; // owned by grid_; a ClipCardDelegate
	QLabel *totalsLabel_ = nullptr;
	QSlider *sizeSlider_ = nullptr;
	ThumbnailCache thumbnails_;
	QHash<QString, QListWidgetItem *> itemByPath_;

	int cardScale_ = 1;                 // 0=Small, 1=Medium, 2=Large
	QSet<QString> favorites_;           // absolute file paths
	QSet<QString> recentlyViewed_;      // absolute file paths
	QHash<QString, qint64> durationMs_; // path -> probed duration (ms)
	QSet<QString> durationInFlight_;    // paths currently being probed
};

} // namespace harpia
