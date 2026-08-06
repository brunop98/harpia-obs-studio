#pragma once

#include "core/ShortcutConflicts.hpp"

#include <QDialog>
#include <QVector>

class QKeySequenceEdit;
class QLabel;
class QPushButton;

namespace harpia {

// "This key is assigned to two things." Shows every action involved, lets any
// of them be re-keyed or cleared, and will not let you leave until no two of
// them share a key.
//
// The point of showing BOTH sides rather than asking "replace it?" is that the
// right answer is often to move the OTHER one -- you know what you just pressed
// and why, and being told only that something unnamed will lose its key is not
// enough to decide. It also means one dialog handles the case where three
// things somehow ended up on the same key, which a two-way "replace?" cannot.
//
// The rule it enforces (a duplicate can never be saved) lives in
// core/ShortcutConflicts.hpp and is shared with the recorder's hotkeys and the
// editor's command list, so both refuse the same things.
class ShortcutConflictDialog : public QDialog {
	Q_OBJECT
public:
	// `entries` is every action that has to be settled: the contested ones,
	// plus any others whose keys must be respected so a fix cannot simply move
	// the clash somewhere else. `editable` names the ids the user may change
	// here; anything else is shown greyed as context. Empty means all of them.
	ShortcutConflictDialog(QVector<ShortcutBinding> entries, QSet<QString> editable,
			       QWidget *parent = nullptr);
	~ShortcutConflictDialog() override;

	// The bindings as the user left them. Guaranteed conflict-free: OK cannot
	// be pressed otherwise.
	QVector<ShortcutBinding> result() const { return entries_; }

	// The validation the OK button is gated on, exposed so a test can drive it
	// without a visible window.
	bool resolvedForTest() const;

private:
	void refresh(); // re-mark the rows and enable/disable OK

	QVector<ShortcutBinding> entries_;
	QVector<QKeySequenceEdit *> edits_;
	QVector<QLabel *> marks_;
	QLabel *status_ = nullptr;
	QPushButton *okButton_ = nullptr;
};

} // namespace harpia
