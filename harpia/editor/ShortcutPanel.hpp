#pragma once

// The floating Keyboard Shortcuts panel: search, filter, rebind, import/export.
//
// Non-modal and independent of the editor window, so it can sit open beside the
// timeline while you work — which is the point of a shortcut reference.
//
// Capturing a new binding needs a widget that swallows every key, including the
// ones Qt would otherwise treat as navigation (Tab, Enter, Esc, the arrows), or
// half the keyboard could never be bound. KeyCaptureEdit is that widget.

#include <QDialog>
#include <QKeySequence>
#include <QLineEdit>
#include <QString>

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace harpia {

class ShortcutRegistry;

// A one-shot key grabber. Shows "Press the new shortcut…" and reports the first
// combination pressed. Esc cancels; Backspace/Delete alone clears the binding.
class KeyCaptureEdit : public QLineEdit {
	Q_OBJECT
public:
	explicit KeyCaptureEdit(QWidget *parent = nullptr);

signals:
	void captured(const QKeySequence &k); // empty = "clear this binding"
	void cancelled();

protected:
	void keyPressEvent(QKeyEvent *e) override;
	// Tab and Backtab never reach keyPressEvent by default — they move focus.
	bool event(QEvent *e) override;
};

class ShortcutPanel : public QDialog {
	Q_OBJECT
public:
	ShortcutPanel(ShortcutRegistry *reg, QWidget *parent = nullptr);

protected:
	void closeEvent(QCloseEvent *e) override; // remember size + position

private:
	void rebuild();          // refill the tree from the registry + filters
	void beginCapture(QTreeWidgetItem *item);
	void applyCaptured(const QString &id, const QKeySequence &k, bool add);
	void showContextMenu(const QPoint &pos);
	QString selectedId() const;

	ShortcutRegistry *reg_ = nullptr;
	QLineEdit *search_ = nullptr;
	QComboBox *category_ = nullptr;
	QTreeWidget *tree_ = nullptr;
	QLabel *status_ = nullptr;
	QPushButton *editBtn_ = nullptr;
	QPushButton *addBtn_ = nullptr;
	QPushButton *resetBtn_ = nullptr;
	bool building_ = false;
};

} // namespace harpia
