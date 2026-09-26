#include "SearchPicker.hpp"

#include <QApplication>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QScreen>
#include <QVBoxLayout>

namespace harpia {

namespace {
constexpr int kRowKind = Qt::UserRole;     // Row
constexpr int kRowId = Qt::UserRole + 1;   // PickItem::id, or the category name
constexpr int kWidth = 300;
constexpr int kMaxRows = 14;
} // namespace

SearchPicker::SearchPicker(const QVector<PickItem> &items, QWidget *parent)
	: QWidget(parent, Qt::Popup | Qt::FramelessWindowHint), items_(items)
{
	setAttribute(Qt::WA_DeleteOnClose);
	auto *lay = new QVBoxLayout(this);
	lay->setContentsMargins(6, 6, 6, 6);
	lay->setSpacing(4);
	search_ = new QLineEdit(this);
	search_->setPlaceholderText(QStringLiteral("Search…"));
	search_->setClearButtonEnabled(true);
	search_->installEventFilter(this);
	lay->addWidget(search_);
	list_ = new QListWidget(this);
	list_->setSelectionMode(QAbstractItemView::SingleSelection);
	list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	list_->setFocusPolicy(Qt::NoFocus); // the field keeps the keys; see eventFilter
	lay->addWidget(list_, 1);
	setStyleSheet(QStringLiteral(
		"SearchPicker{background:#1e2126;border:1px solid #3a3f47;}"
		"QLineEdit{background:#15171a;color:#e8eaed;border:1px solid #2b2f36;padding:4px 6px;}"
		"QListWidget{background:#1e2126;color:#e8eaed;border:none;outline:0;}"
		"QListWidget::item{padding:5px 8px;}"
		"QListWidget::item:selected{background:#2f6fed;color:#ffffff;}"));
	setFixedWidth(kWidth);

	connect(search_, &QLineEdit::textChanged, this, [this](const QString &) { rebuild(); });
	connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) { activate(it); });
	rebuild();
}

QString SearchPicker::pick(const QVector<PickItem> &items, const QPoint &globalPos, QWidget *parent)
{
	auto *p = new SearchPicker(items, parent);
	QString result;
	QEventLoop loop;
	p->setOnPicked([&](const QString &id) {
		result = id;
		loop.quit();
	});
	p->showAt(globalPos);
	loop.exec();
	return result;
}

void SearchPicker::showAt(const QPoint &globalPos)
{
	// Keep the whole popup on the screen it was opened on.
	QPoint at = globalPos;
	if (const QScreen *s = QGuiApplication::screenAt(globalPos)) {
		const QRect avail = s->availableGeometry();
		at.setX(std::min(at.x(), avail.right() - width()));
		at.setY(std::min(at.y(), avail.bottom() - sizeHint().height()));
		at.setX(std::max(at.x(), avail.left()));
		at.setY(std::max(at.y(), avail.top()));
	}
	move(at);
	show();
	search_->setFocus();
}

void SearchPicker::rebuild()
{
	list_->clear();
	const QString q = search_->text().trimmed();
	auto addRow = [this](const QString &text, Row kind, const QString &id) {
		auto *it = new QListWidgetItem(text, list_);
		it->setData(kRowKind, int(kind));
		it->setData(kRowId, id);
		return it;
	};
	if (!q.isEmpty()) {
		// Typing: one flat list, best first, the category in grey after the name.
		for (const PickItem &pi : rankItems(items_, q))
			addRow(pi.name, Row::Item, pi.id)
				->setToolTip(pi.category);
		if (list_->count() == 0)
			addRow(QStringLiteral("Nothing matches"), Row::Back, QString())->setFlags(Qt::NoItemFlags);
	} else if (!folder_.isEmpty()) {
		// Inside a folder: a way back, then its items in the order given.
		addRow(QStringLiteral("‹  ") + folder_, Row::Back, QString());
		for (const PickItem &pi : items_)
			if (pi.category == folder_ && !pi.pinned)
				addRow(pi.name, Row::Item, pi.id);
	} else {
		// The folders. Pinned items first: they are the "do this now" entries.
		for (const PickItem &pi : items_)
			if (pi.pinned)
				addRow(pi.name, Row::Item, pi.id);
		QStringList cats;
		for (const PickItem &pi : items_) {
			const QString c = pi.category.isEmpty() ? QStringLiteral("Other") : pi.category;
			if (!pi.pinned && !cats.contains(c))
				cats << c;
		}
		for (const QString &c : cats)
			addRow(c + QStringLiteral("  ›"), Row::Category, c);
	}
	// The first pickable row is highlighted, so Enter always does something.
	for (int i = 0; i < list_->count(); ++i) {
		if (list_->item(i)->flags() & Qt::ItemIsEnabled) {
			list_->setCurrentRow(i);
			break;
		}
	}
	const int rows = std::max(1, std::min(list_->count(), kMaxRows));
	const int rowH = list_->sizeHintForRow(0) > 0 ? list_->sizeHintForRow(0) : 26;
	list_->setFixedHeight(rows * rowH + 4);
	adjustSize();
}

QStringList SearchPicker::visibleNames() const
{
	QStringList out;
	for (int i = 0; i < list_->count(); ++i)
		out << list_->item(i)->text();
	return out;
}

int SearchPicker::highlightedRow() const
{
	return list_->currentRow();
}

void SearchPicker::activate(QListWidgetItem *it)
{
	if (!it || !(it->flags() & Qt::ItemIsEnabled))
		return;
	switch (Row(it->data(kRowKind).toInt())) {
	case Row::Category:
		folder_ = it->data(kRowId).toString();
		if (folder_ == QStringLiteral("Other"))
			for (PickItem &pi : items_)
				if (pi.category.isEmpty())
					pi.category = folder_; // so the folder view finds them
		rebuild();
		break;
	case Row::Back:
		folder_.clear();
		rebuild();
		break;
	case Row::Item:
		finish(it->data(kRowId).toString());
		break;
	}
}

void SearchPicker::finish(const QString &id)
{
	if (picked_)
		return;
	picked_ = true;
	const auto fn = onPicked_;
	hide();
	close();
	if (fn)
		fn(id);
}

void SearchPicker::moveHighlight(int delta)
{
	const int n = list_->count();
	if (n == 0)
		return;
	int row = list_->currentRow();
	for (int tries = 0; tries < n; ++tries) {
		row = (row + delta + n) % n;
		if (list_->item(row)->flags() & Qt::ItemIsEnabled)
			break;
	}
	list_->setCurrentRow(row);
	list_->scrollToItem(list_->item(row));
}

bool SearchPicker::eventFilter(QObject *watched, QEvent *e)
{
	if (watched == search_ && e->type() == QEvent::KeyPress) {
		auto *ke = static_cast<QKeyEvent *>(e);
		switch (ke->key()) {
		case Qt::Key_Down:
			moveHighlight(+1);
			return true;
		case Qt::Key_Up:
			moveHighlight(-1);
			return true;
		case Qt::Key_Return:
		case Qt::Key_Enter:
			activate(list_->currentItem());
			return true;
		case Qt::Key_Escape:
			finish(QString());
			return true;
		case Qt::Key_Backspace:
			// An empty field inside a folder: Backspace steps back out, the
			// way it does in a file browser.
			if (search_->text().isEmpty() && !folder_.isEmpty()) {
				folder_.clear();
				rebuild();
				return true;
			}
			break;
		default:
			break;
		}
	}
	return QWidget::eventFilter(watched, e);
}

void SearchPicker::hideEvent(QHideEvent *e)
{
	QWidget::hideEvent(e);
	// Clicked away, or closed by the window manager: nothing was picked, and
	// whoever is waiting has to be told or a blocking pick() never returns.
	if (!picked_)
		finish(QString());
}

} // namespace harpia
