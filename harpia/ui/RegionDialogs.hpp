#pragma once

#include "model/RegionStore.hpp"

#include <QDialog>

class QLineEdit;
class QSpinBox;
class QListWidget;

namespace harpia {

// Edit a saved region's name, position and size. Used both when saving a new
// region and when editing an existing one.
class RegionEditDialog : public QDialog {
	Q_OBJECT
public:
	RegionEditDialog(const SavedRegion &initial, QWidget *parent = nullptr);
	SavedRegion result() const { return result_; }

private slots:
	void onAccept();

private:
	SavedRegion result_;
	QLineEdit *nameEdit_ = nullptr;
	QSpinBox *xSpin_ = nullptr;
	QSpinBox *ySpin_ = nullptr;
	QSpinBox *wSpin_ = nullptr;
	QSpinBox *hSpin_ = nullptr;
};

// Manage the list of saved regions: rename, edit properties, or delete. Edits
// are applied to the store immediately; the caller refreshes its UI on close.
class SavedRegionsDialog : public QDialog {
	Q_OBJECT
public:
	SavedRegionsDialog(RegionStore &store, QWidget *parent = nullptr);

private slots:
	void onRename();
	void onEdit();
	void onDelete();

private:
	void reload();
	const SavedRegion *selected() const;

	RegionStore &store_;
	QListWidget *list_ = nullptr;
};

} // namespace harpia
