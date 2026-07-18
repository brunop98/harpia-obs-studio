#pragma once

#include <QDialog>

class QListWidget;

namespace harpia {

class PresetStore;

// Manage the collection of presets: create, edit, duplicate, and delete. Edits
// are persisted immediately through PresetStore. A sensible default output
// folder is supplied for brand-new presets.
class PresetManagerDialog : public QDialog {
	Q_OBJECT
public:
	PresetManagerDialog(PresetStore &store, QString defaultFolder, QWidget *parent = nullptr);

private slots:
	void addPreset();
	void editPreset();
	void duplicatePreset();
	void deletePreset();

private:
	void reloadList();
	QString selectedId() const;

	PresetStore &store_;
	QString defaultFolder_;
	QListWidget *list_ = nullptr;
};

} // namespace harpia
