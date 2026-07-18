#include "PresetManagerDialog.hpp"

#include "PresetEditorDialog.hpp"
#include "model/PresetStore.hpp"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QUuid>
#include <QVBoxLayout>

namespace harpia {

namespace {
constexpr int kIdRole = Qt::UserRole + 1;

std::string newPresetId()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}
} // namespace

PresetManagerDialog::PresetManagerDialog(PresetStore &store, QString defaultFolder, QWidget *parent)
	: QDialog(parent), store_(store), defaultFolder_(std::move(defaultFolder))
{
	setWindowTitle(QStringLiteral("Presets"));
	setModal(true);
	resize(360, 360);

	list_ = new QListWidget(this);
	connect(list_, &QListWidget::itemDoubleClicked, this, &PresetManagerDialog::editPreset);

	auto *newBtn = new QPushButton(QStringLiteral("New…"), this);
	auto *editBtn = new QPushButton(QStringLiteral("Edit…"), this);
	auto *dupBtn = new QPushButton(QStringLiteral("Duplicate"), this);
	auto *delBtn = new QPushButton(QStringLiteral("Delete"), this);
	connect(newBtn, &QPushButton::clicked, this, &PresetManagerDialog::addPreset);
	connect(editBtn, &QPushButton::clicked, this, &PresetManagerDialog::editPreset);
	connect(dupBtn, &QPushButton::clicked, this, &PresetManagerDialog::duplicatePreset);
	connect(delBtn, &QPushButton::clicked, this, &PresetManagerDialog::deletePreset);

	auto *btnCol = new QVBoxLayout;
	btnCol->addWidget(newBtn);
	btnCol->addWidget(editBtn);
	btnCol->addWidget(dupBtn);
	btnCol->addWidget(delBtn);
	btnCol->addStretch(1);

	auto *row = new QHBoxLayout;
	row->addWidget(list_, 1);
	row->addLayout(btnCol);

	auto *closeBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(closeBox, &QDialogButtonBox::rejected, this, &QDialog::accept);
	connect(closeBox, &QDialogButtonBox::accepted, this, &QDialog::accept);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(row, 1);
	layout->addWidget(closeBox);

	reloadList();
}

void PresetManagerDialog::reloadList()
{
	list_->clear();
	for (const Preset &p : store_.presets()) {
		auto *item = new QListWidgetItem(QString::fromStdString(p.name));
		item->setData(kIdRole, QString::fromStdString(p.id));
		item->setToolTip(QStringLiteral("%1 · %2 fps · %3")
					 .arg(QString::fromStdString(formatToString(p.format)).toUpper())
					 .arg(p.fps)
					 .arg(QString::fromStdString(p.outputFolder)));
		list_->addItem(item);
	}
	if (list_->count() > 0 && !list_->currentItem())
		list_->setCurrentRow(0);
}

QString PresetManagerDialog::selectedId() const
{
	QListWidgetItem *item = list_->currentItem();
	return item ? item->data(kIdRole).toString() : QString();
}

void PresetManagerDialog::addPreset()
{
	Preset base = Preset::makeDefault(defaultFolder_.toStdString());
	base.id = newPresetId();
	base.name = "New preset";

	PresetEditorDialog dlg(base, this);
	if (dlg.exec() == QDialog::Accepted) {
		store_.upsert(dlg.result());
		reloadList();
	}
}

void PresetManagerDialog::editPreset()
{
	const QString id = selectedId();
	if (id.isEmpty())
		return;
	const Preset *p = store_.find(id.toStdString());
	if (!p)
		return;

	PresetEditorDialog dlg(*p, this);
	if (dlg.exec() == QDialog::Accepted) {
		store_.upsert(dlg.result());
		reloadList();
	}
}

void PresetManagerDialog::duplicatePreset()
{
	const QString id = selectedId();
	if (id.isEmpty())
		return;
	const Preset *p = store_.find(id.toStdString());
	if (!p)
		return;

	Preset copy = *p;
	copy.id = newPresetId();
	copy.name = p->name + " (copy)";
	store_.upsert(copy);
	reloadList();
}

void PresetManagerDialog::deletePreset()
{
	const QString id = selectedId();
	if (id.isEmpty())
		return;
	if (store_.presets().size() <= 1) {
		QMessageBox::information(this, windowTitle(),
					QStringLiteral("At least one preset must remain."));
		return;
	}
	const Preset *p = store_.find(id.toStdString());
	const QString name = p ? QString::fromStdString(p->name) : id;
	if (QMessageBox::question(this, windowTitle(),
				  QStringLiteral("Delete preset \"%1\"?").arg(name)) != QMessageBox::Yes)
		return;

	store_.remove(id.toStdString());
	reloadList();
}

} // namespace harpia
