#pragma once

// A searchable popup, in the shape of Unity's Add Component menu.
//
// Opens with a search field and, under it, the categories as folders. Click
// a folder (or press Enter on it) to see what is in it, with a "‹ Back" row
// at the top. Start typing and the folders give way to one flat list of
// everything that matches, best match first (FuzzyMatch.hpp). Up and Down
// move the highlight without leaving the field; Enter picks the highlighted
// row, or the first one; Escape, or clicking away, closes with nothing.
//
// Generic on purpose: it is given items with an id, a name and a category,
// and hands back an id. The Inspector's Add Component and the effect lane's
// Add effect both use it, and anything else that offers a list by name can.

#include "FuzzyMatch.hpp"

#include <QPoint>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class QLineEdit;
class QListWidget;
class QListWidgetItem;

namespace harpia {

class SearchPicker : public QWidget {
public:
	explicit SearchPicker(const QVector<PickItem> &items, QWidget *parent = nullptr);

	// Show at a screen position and block until something is picked or the
	// popup closes. Returns the picked id, or "" for nothing. The one call the
	// two menus need.
	static QString pick(const QVector<PickItem> &items, const QPoint &globalPos, QWidget *parent);

	// Non-blocking form: the callback fires once, with the id or "".
	void setOnPicked(std::function<void(const QString &)> fn) { onPicked_ = std::move(fn); }
	void showAt(const QPoint &globalPos);

	// What the list shows right now, top to bottom -- for tests, and for the
	// same reason as elsewhere: a test that rebuilt the ranking itself would
	// be checking its own arithmetic.
	QStringList visibleNames() const;
	QLineEdit *searchField() const { return search_; }
	QListWidget *listWidget() const { return list_; }
	// The row the highlight is on, or -1.
	int highlightedRow() const;

protected:
	bool eventFilter(QObject *watched, QEvent *e) override;
	void hideEvent(QHideEvent *e) override;

private:
	enum class Row { Category, Item, Back };
	void rebuild();
	void activate(QListWidgetItem *it);
	void finish(const QString &id);
	void moveHighlight(int delta);

	QVector<PickItem> items_;
	QString folder_;   // the category being browsed, "" = the folder list
	bool picked_ = false;
	std::function<void(const QString &)> onPicked_;
	QLineEdit *search_ = nullptr;
	QListWidget *list_ = nullptr;
};

} // namespace harpia
