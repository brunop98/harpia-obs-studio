#include "ShortcutPanel.hpp"

#include "ShortcutRegistry.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace harpia {

namespace {
constexpr int kIdRole = Qt::UserRole + 1;
QSettings panelSettings()
{
	return QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
}
} // namespace

// ---- KeyCaptureEdit ---------------------------------------------------------

KeyCaptureEdit::KeyCaptureEdit(QWidget *parent) : QLineEdit(parent)
{
	setReadOnly(true);
	setPlaceholderText(QStringLiteral("Press the new shortcut…"));
	setFocusPolicy(Qt::StrongFocus);
}

bool KeyCaptureEdit::event(QEvent *e)
{
	// Tab, Backtab and the shortcut-override pass all bypass keyPressEvent.
	// Without this, Tab could never be bound and any key that is already a
	// shortcut somewhere would be swallowed before it got here.
	if (e->type() == QEvent::KeyPress || e->type() == QEvent::ShortcutOverride) {
		auto *ke = static_cast<QKeyEvent *>(e);
		if (e->type() == QEvent::ShortcutOverride) {
			ke->accept();
			return true;
		}
		keyPressEvent(ke);
		return true;
	}
	return QLineEdit::event(e);
}

void KeyCaptureEdit::keyPressEvent(QKeyEvent *e)
{
	const int key = e->key();
	// A modifier on its own is not a shortcut; wait for the real key.
	if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt ||
	    key == Qt::Key_Meta || key == Qt::Key_unknown)
		return;
	if (key == Qt::Key_Escape && e->modifiers() == Qt::NoModifier) {
		emit cancelled();
		return;
	}
	if ((key == Qt::Key_Backspace || key == Qt::Key_Delete) && e->modifiers() == Qt::NoModifier) {
		emit captured(QKeySequence()); // clear
		return;
	}
	emit captured(QKeySequence(e->keyCombination()));
}

// ---- ShortcutPanel ----------------------------------------------------------

ShortcutPanel::ShortcutPanel(ShortcutRegistry *reg, QWidget *parent) : QDialog(parent), reg_(reg)
{
	setWindowTitle(QStringLiteral("Keyboard Shortcuts"));
	setWindowFlag(Qt::Window, true);
	setModal(false); // stays open beside the editor, which is the point
	setSizeGripEnabled(true);

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(12, 12, 12, 12);
	outer->setSpacing(8);

	auto *top = new QHBoxLayout;
	top->setSpacing(8);
	search_ = new QLineEdit(this);
	search_->setPlaceholderText(QStringLiteral("Search commands, keys or categories…"));
	search_->setClearButtonEnabled(true);
	top->addWidget(search_, 1);
	category_ = new QComboBox(this);
	top->addWidget(category_);
	outer->addLayout(top);
	connect(search_, &QLineEdit::textChanged, this, [this]() { rebuild(); });
	connect(category_, &QComboBox::currentIndexChanged, this, [this]() { rebuild(); });

	tree_ = new QTreeWidget(this);
	tree_->setColumnCount(3);
	tree_->setHeaderLabels({QStringLiteral("Command"), QStringLiteral("Shortcut"),
				QStringLiteral("Category")});
	tree_->setRootIsDecorated(false);
	tree_->setAlternatingRowColors(true);
	tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
	tree_->setContextMenuPolicy(Qt::CustomContextMenu);
	tree_->setUniformRowHeights(true);
	// A dense reference list is hard to scan; give the rows room.
	tree_->setStyleSheet(QStringLiteral("QTreeView::item { padding: 4px 2px; }"));
	tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	outer->addWidget(tree_, 1);
	connect(tree_, &QTreeWidget::customContextMenuRequested, this,
		&ShortcutPanel::showContextMenu);
	// Double-clicking a row edits it, as asked.
	connect(tree_, &QTreeWidget::itemDoubleClicked, this,
		[this](QTreeWidgetItem *it, int) { beginCapture(it); });
	connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this]() {
		const QString id = selectedId();
		const ShortcutCommand *c = id.isEmpty() ? nullptr : reg_->command(id);
		const bool editable = c && !c->mouseOnly;
		editBtn_->setEnabled(editable);
		addBtn_->setEnabled(editable);
		resetBtn_->setEnabled(editable);
	});

	status_ = new QLabel(this);
	status_->setWordWrap(true);
	status_->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	outer->addWidget(status_);

	auto *row = new QHBoxLayout;
	row->setSpacing(6);
	editBtn_ = new QPushButton(QStringLiteral("Edit…"), this);
	connect(editBtn_, &QPushButton::clicked, this,
		[this]() { beginCapture(tree_->currentItem()); });
	row->addWidget(editBtn_);
	addBtn_ = new QPushButton(QStringLiteral("Add another"), this);
	addBtn_->setToolTip(QStringLiteral("Give this command a second shortcut."));
	connect(addBtn_, &QPushButton::clicked, this, [this]() {
		QTreeWidgetItem *it = tree_->currentItem();
		if (!it)
			return;
		it->setData(0, kIdRole + 1, true); // mark: the capture should ADD
		beginCapture(it);
	});
	row->addWidget(addBtn_);
	resetBtn_ = new QPushButton(QStringLiteral("Reset"), this);
	connect(resetBtn_, &QPushButton::clicked, this, [this]() {
		const QString id = selectedId();
		if (!id.isEmpty())
			reg_->resetToDefault(id);
	});
	row->addWidget(resetBtn_);
	row->addStretch(1);

	auto *importBtn = new QPushButton(QStringLiteral("Import…"), this);
	connect(importBtn, &QPushButton::clicked, this, [this]() {
		const QString f = QFileDialog::getOpenFileName(
			this, QStringLiteral("Import shortcuts"), QString(),
			QStringLiteral("Shortcut profile (*.json)"));
		if (f.isEmpty())
			return;
		QFile file(f);
		if (!file.open(QIODevice::ReadOnly)) {
			QMessageBox::warning(this, QStringLiteral("Import shortcuts"),
					     QStringLiteral("Could not read that file."));
			return;
		}
		QString err;
		if (!reg_->importProfile(file.readAll(), &err))
			QMessageBox::warning(this, QStringLiteral("Import shortcuts"), err);
	});
	row->addWidget(importBtn);
	auto *exportBtn = new QPushButton(QStringLiteral("Export…"), this);
	connect(exportBtn, &QPushButton::clicked, this, [this]() {
		QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Export shortcuts"),
							 QStringLiteral("harpia-shortcuts.json"),
							 QStringLiteral("Shortcut profile (*.json)"));
		if (f.isEmpty())
			return;
		if (!f.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
			f += QStringLiteral(".json");
		QFile file(f);
		if (!file.open(QIODevice::WriteOnly)) {
			QMessageBox::warning(this, QStringLiteral("Export shortcuts"),
					     QStringLiteral("Could not write that file."));
			return;
		}
		file.write(reg_->exportProfile());
	});
	row->addWidget(exportBtn);
	auto *factoryBtn = new QPushButton(QStringLiteral("Restore defaults"), this);
	connect(factoryBtn, &QPushButton::clicked, this, [this]() {
		if (QMessageBox::question(this, QStringLiteral("Restore defaults"),
					  QStringLiteral("Put every shortcut back to the shipped "
							 "default?")) == QMessageBox::Yes)
			reg_->resetAllToDefaults();
	});
	row->addWidget(factoryBtn);
	auto *closeBtn = new QPushButton(QStringLiteral("Close"), this);
	connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
	row->addWidget(closeBtn);
	outer->addLayout(row);

	// Follow the registry: an import, a reset or an edit made elsewhere all
	// refresh the list through the same signal.
	connect(reg_, &ShortcutRegistry::bindingsChanged, this, [this]() { rebuild(); });

	// Categories, plus "All" first.
	category_->addItem(QStringLiteral("All"));
	for (const QString &c : reg_->categories())
		category_->addItem(c);

	// Come back the size and place it was left, and remember what was being
	// searched for while the panel stays open.
	QSettings s = panelSettings();
	s.beginGroup(QStringLiteral("shortcutPanel"));
	const QRect geo = s.value(QStringLiteral("geometry")).toRect();
	s.endGroup();
	if (geo.isValid() && geo.width() > 200 && geo.height() > 150)
		setGeometry(geo);
	else
		resize(720, 520);

	rebuild();
}

void ShortcutPanel::closeEvent(QCloseEvent *e)
{
	QSettings s = panelSettings();
	s.beginGroup(QStringLiteral("shortcutPanel"));
	s.setValue(QStringLiteral("geometry"), geometry());
	s.endGroup();
	QDialog::closeEvent(e);
}

QString ShortcutPanel::selectedId() const
{
	QTreeWidgetItem *it = tree_->currentItem();
	return it ? it->data(0, kIdRole).toString() : QString();
}

void ShortcutPanel::rebuild()
{
	if (building_)
		return;
	building_ = true;
	const QString keep = selectedId();
	const QString needle = search_->text().trimmed();
	const QString cat = category_->currentIndex() <= 0 ? QString() : category_->currentText();

	tree_->clear();
	int shown = 0, conflicts = 0;
	// A key bound to more than one command: flagged on every row involved.
	QHash<QString, int> keyUse;
	for (const ShortcutCommand &c : reg_->commands())
		for (const QKeySequence &k : reg_->bindings(c.id))
			keyUse[k.toString(QKeySequence::PortableText)]++;

	QTreeWidgetItem *restore = nullptr;
	for (const ShortcutCommand &c : reg_->commands()) {
		if (!cat.isEmpty() && c.category != cat)
			continue;
		const QString keys = reg_->displayText(c.id);
		// Search matches the name, the keys OR the category, as asked.
		if (!needle.isEmpty() && !c.label.contains(needle, Qt::CaseInsensitive) &&
		    !keys.contains(needle, Qt::CaseInsensitive) &&
		    !c.category.contains(needle, Qt::CaseInsensitive))
			continue;

		auto *it = new QTreeWidgetItem(tree_);
		it->setText(0, c.label);
		it->setText(1, keys.isEmpty() ? QStringLiteral("—") : keys);
		it->setText(2, c.category);
		it->setData(0, kIdRole, c.id);
		if (c.mouseOnly) {
			it->setForeground(1, QColor(0x9a, 0x9f, 0xa8));
			it->setToolTip(0, QStringLiteral("Handled by the mouse — not rebindable."));
		} else if (reg_->isModified(c.id)) {
			// Recently changed rows stand out from the shipped defaults.
			QFont f = it->font(0);
			f.setBold(true);
			it->setFont(0, f);
			it->setForeground(1, QColor(0x00, 0xae, 0xef));
			it->setToolTip(1, QStringLiteral("Changed from the default (%1).")
						  .arg(reg_->defaults(c.id).isEmpty()
							       ? QStringLiteral("none")
							       : reg_->defaults(c.id)
									 .first()
									 .toString(QKeySequence::NativeText)));
		}
		bool clash = false;
		for (const QKeySequence &k : reg_->bindings(c.id))
			if (keyUse.value(k.toString(QKeySequence::PortableText)) > 1)
				clash = true;
		if (clash) {
			++conflicts;
			it->setForeground(1, QColor(0xe5, 0x48, 0x4d));
			it->setToolTip(1, QStringLiteral("This key is assigned to more than one "
							 "command — only one of them will fire."));
		}
		if (c.id == keep)
			restore = it;
		++shown;
	}
	if (restore)
		tree_->setCurrentItem(restore);
	else if (tree_->topLevelItemCount() > 0)
		tree_->setCurrentItem(tree_->topLevelItem(0));

	status_->setText(
		conflicts > 0
			? QStringLiteral("%1 command%2 shown · %3 with a duplicate key (shown in red)")
				  .arg(shown)
				  .arg(shown == 1 ? QString() : QStringLiteral("s"))
				  .arg(conflicts)
			: QStringLiteral("%1 command%2 shown · double-click a row to rebind it")
				  .arg(shown)
				  .arg(shown == 1 ? QString() : QStringLiteral("s")));
	building_ = false;
}

void ShortcutPanel::beginCapture(QTreeWidgetItem *item)
{
	if (!item)
		return;
	const QString id = item->data(0, kIdRole).toString();
	const ShortcutCommand *c = reg_->command(id);
	if (!c || c->mouseOnly)
		return;
	const bool add = item->data(0, kIdRole + 1).toBool();
	item->setData(0, kIdRole + 1, false);

	QDialog dlg(this);
	dlg.setWindowTitle(add ? QStringLiteral("Add a shortcut") : QStringLiteral("Set a shortcut"));
	dlg.setModal(true);
	dlg.setMinimumWidth(360);
	auto *v = new QVBoxLayout(&dlg);
	v->setContentsMargins(18, 16, 18, 14);
	v->setSpacing(10);
	auto *what = new QLabel(QStringLiteral("<b>%1</b>").arg(c->label.toHtmlEscaped()), &dlg);
	v->addWidget(what);
	auto *prompt = new QLabel(QStringLiteral("Press the new shortcut…"), &dlg);
	prompt->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	v->addWidget(prompt);
	auto *cap = new KeyCaptureEdit(&dlg);
	v->addWidget(cap);
	auto *hint = new QLabel(QStringLiteral("Esc cancels · Backspace clears the binding"), &dlg);
	hint->setStyleSheet(QStringLiteral("color:#7f858e;"));
	v->addWidget(hint);
	cap->setFocus();

	QKeySequence got;
	bool cleared = false;
	connect(cap, &KeyCaptureEdit::cancelled, &dlg, [&dlg]() { dlg.done(0); });
	connect(cap, &KeyCaptureEdit::captured, &dlg, [&](const QKeySequence &k) {
		got = k;
		cleared = k.isEmpty();
		dlg.done(1);
	});
	if (dlg.exec() != 1)
		return;
	if (cleared) {
		reg_->setBindings(id, {});
		return;
	}
	applyCaptured(id, got, add);
}

void ShortcutPanel::applyCaptured(const QString &id, const QKeySequence &k, bool add)
{
	const QString owner = reg_->conflict(k, id);
	if (!owner.isEmpty()) {
		const ShortcutCommand *other = reg_->command(owner);
		const QMessageBox::StandardButton r = QMessageBox::question(
			this, QStringLiteral("Shortcut already assigned"),
			QStringLiteral("<b>%1</b><br><br>Already assigned to:<br><b>%2</b><br><br>"
				       "Replace it? The other command loses this shortcut.")
				.arg(k.toString(QKeySequence::NativeText),
				     other ? other->label.toHtmlEscaped() : owner),
			QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
		if (r != QMessageBox::Yes)
			return;
		reg_->removeBinding(owner, k);
	}
	if (add)
		reg_->addBinding(id, k);
	else
		reg_->setBindings(id, {k});
}

void ShortcutPanel::showContextMenu(const QPoint &pos)
{
	QTreeWidgetItem *it = tree_->itemAt(pos);
	if (!it)
		return;
	tree_->setCurrentItem(it);
	const QString id = it->data(0, kIdRole).toString();
	const ShortcutCommand *c = reg_->command(id);
	if (!c)
		return;

	QMenu menu(this);
	QAction *edit = menu.addAction(QStringLiteral("Edit…"));
	QAction *addOne = menu.addAction(QStringLiteral("Add another"));
	QAction *remove = menu.addAction(QStringLiteral("Remove"));
	QAction *reset = menu.addAction(QStringLiteral("Reset to default"));
	menu.addSeparator();
	QAction *copy = menu.addAction(QStringLiteral("Copy shortcut"));
	for (QAction *a : {edit, addOne, remove, reset})
		a->setEnabled(!c->mouseOnly);
	remove->setEnabled(!c->mouseOnly && !reg_->bindings(id).isEmpty());

	QAction *chosen = menu.exec(tree_->viewport()->mapToGlobal(pos));
	if (chosen == edit) {
		beginCapture(it);
	} else if (chosen == addOne) {
		it->setData(0, kIdRole + 1, true);
		beginCapture(it);
	} else if (chosen == remove) {
		reg_->setBindings(id, {});
	} else if (chosen == reset) {
		reg_->resetToDefault(id);
	} else if (chosen == copy) {
		QApplication::clipboard()->setText(reg_->displayText(id));
	}
}

} // namespace harpia
