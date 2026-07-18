#pragma once

#include "library/ClipLibrary.hpp"
#include "library/ThumbnailCache.hpp"

#include <QWidget>

class QListWidget;
class QListWidgetItem;

namespace harpia {

class PresetStore;

// A standalone window listing every recording that still exists on disk, as a
// thumbnail grid. Each entry shows a preview, how long ago it was recorded, its
// size, and the preset that produced it. Right-click for Open/Copy/Rename/Delete;
// the Delete key removes the selected clips. Clips can be dragged into other
// applications.
class ClipLibraryWindow : public QWidget {
	Q_OBJECT
public:
	explicit ClipLibraryWindow(PresetStore &store, QWidget *parent = nullptr);

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

private:
	QStringList folders() const;
	ClipLibrary::PresetByFolder presetByFolder() const;
	QStringList selectedPaths() const;

	PresetStore &store_;
	QListWidget *grid_ = nullptr;
	ThumbnailCache thumbnails_;
};

} // namespace harpia
