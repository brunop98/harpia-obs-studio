#include "RegionDialogs.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace harpia {

// ---- RegionEditDialog ---------------------------------------------------

RegionEditDialog::RegionEditDialog(const SavedRegion &initial, QWidget *parent)
	: QDialog(parent), result_(initial)
{
	setWindowTitle(QStringLiteral("Region properties"));
	setModal(true);
	setMinimumWidth(320);

	auto *root = new QVBoxLayout(this);
	auto *form = new QFormLayout;

	nameEdit_ = new QLineEdit(QString::fromStdString(initial.name), this);
	nameEdit_->setPlaceholderText(QStringLiteral("e.g. Left monitor 1080p"));
	form->addRow(QStringLiteral("Name"), nameEdit_);

	auto makeSpin = [this](int value) {
		auto *s = new QSpinBox(this);
		s->setRange(0, 100000);
		s->setSuffix(QStringLiteral(" px"));
		s->setValue(value);
		return s;
	};
	xSpin_ = makeSpin(initial.x);
	ySpin_ = makeSpin(initial.y);
	wSpin_ = makeSpin(initial.width);
	hSpin_ = makeSpin(initial.height);
	wSpin_->setRange(16, 100000);
	hSpin_->setRange(16, 100000);
	form->addRow(QStringLiteral("X"), xSpin_);
	form->addRow(QStringLiteral("Y"), ySpin_);
	form->addRow(QStringLiteral("Width"), wSpin_);
	form->addRow(QStringLiteral("Height"), hSpin_);
	root->addLayout(form);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
	root->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, this, &RegionEditDialog::onAccept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void RegionEditDialog::onAccept()
{
	const QString name = nameEdit_->text().trimmed();
	if (name.isEmpty()) {
		QMessageBox::warning(this, windowTitle(), QStringLiteral("Please enter a name for the region."));
		return;
	}
	result_.name = name.toStdString();
	result_.x = xSpin_->value();
	result_.y = ySpin_->value();
	result_.width = wSpin_->value();
	result_.height = hSpin_->value();
	accept();
}

// ---- SavedRegionsDialog -------------------------------------------------

SavedRegionsDialog::SavedRegionsDialog(RegionStore &store, QWidget *parent)
	: QDialog(parent), store_(store)
{
	setWindowTitle(QStringLiteral("Saved regions"));
	setModal(true);
	setMinimumSize(380, 300);

	auto *root = new QVBoxLayout(this);
	root->addWidget(new QLabel(QStringLiteral("Manage your saved capture regions:"), this));

	list_ = new QListWidget(this);
	root->addWidget(list_, 1);

	auto *btns = new QHBoxLayout;
	auto *renameBtn = new QPushButton(QStringLiteral("Rename…"), this);
	auto *editBtn = new QPushButton(QStringLiteral("Edit…"), this);
	auto *deleteBtn = new QPushButton(QStringLiteral("Delete"), this);
	btns->addWidget(renameBtn);
	btns->addWidget(editBtn);
	btns->addWidget(deleteBtn);
	btns->addStretch(1);
	auto *closeBtn = new QPushButton(QStringLiteral("Close"), this);
	btns->addWidget(closeBtn);
	root->addLayout(btns);

	connect(renameBtn, &QPushButton::clicked, this, &SavedRegionsDialog::onRename);
	connect(editBtn, &QPushButton::clicked, this, &SavedRegionsDialog::onEdit);
	connect(deleteBtn, &QPushButton::clicked, this, &SavedRegionsDialog::onDelete);
	connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
	connect(list_, &QListWidget::itemDoubleClicked, this, &SavedRegionsDialog::onEdit);

	reload();
}

void SavedRegionsDialog::reload()
{
	list_->clear();
	for (const SavedRegion &r : store_.regions()) {
		auto *item = new QListWidgetItem(
			QStringLiteral("%1   (%2×%3 at %4,%5)")
				.arg(QString::fromStdString(r.name))
				.arg(r.width)
				.arg(r.height)
				.arg(r.x)
				.arg(r.y));
		item->setData(Qt::UserRole, QString::fromStdString(r.id));
		list_->addItem(item);
	}
}

const SavedRegion *SavedRegionsDialog::selected() const
{
	QListWidgetItem *item = list_->currentItem();
	if (!item)
		return nullptr;
	return store_.find(item->data(Qt::UserRole).toString().toStdString());
}

void SavedRegionsDialog::onRename()
{
	const SavedRegion *r = selected();
	if (!r)
		return;
	bool ok = false;
	const QString name = QInputDialog::getText(this, QStringLiteral("Rename region"),
						   QStringLiteral("New name:"), QLineEdit::Normal,
						   QString::fromStdString(r->name), &ok);
	if (!ok || name.trimmed().isEmpty())
		return;
	SavedRegion updated = *r;
	updated.name = name.trimmed().toStdString();
	store_.upsert(updated);
	reload();
}

void SavedRegionsDialog::onEdit()
{
	const SavedRegion *r = selected();
	if (!r)
		return;
	RegionEditDialog dlg(*r, this);
	if (dlg.exec() == QDialog::Accepted) {
		store_.upsert(dlg.result());
		reload();
	}
}

void SavedRegionsDialog::onDelete()
{
	const SavedRegion *r = selected();
	if (!r)
		return;
	if (QMessageBox::question(this, QStringLiteral("Delete region"),
				  QStringLiteral("Delete \"%1\"?").arg(QString::fromStdString(r->name))) !=
	    QMessageBox::Yes)
		return;
	store_.remove(r->id);
	reload();
}

} // namespace harpia
