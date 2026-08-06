#include "ShortcutConflictDialog.hpp"

#include <QDialogButtonBox>
#include <QGridLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>

namespace harpia {

ShortcutConflictDialog::ShortcutConflictDialog(QVector<ShortcutBinding> entries,
					       QSet<QString> editable, QWidget *parent)
	: QDialog(parent), entries_(std::move(entries))
{
	setWindowTitle(QStringLiteral("Shortcut conflict"));
	setModal(true);

	auto *v = new QVBoxLayout(this);
	v->setContentsMargins(18, 16, 18, 14);
	v->setSpacing(10);

	auto *head = new QLabel(
		QStringLiteral("<b>The same key is assigned to more than one action.</b>"), this);
	head->setWordWrap(true);
	v->addWidget(head);

	auto *why = new QLabel(
		QStringLiteral("Only one of them can work — and where both are handled by the window "
			       "rather than system-wide, neither of them fires. Give one of them a "
			       "different key, or clear it, before continuing."),
		this);
	why->setWordWrap(true);
	why->setStyleSheet(QStringLiteral("color:#8a8f98;"));
	v->addWidget(why);

	auto *grid = new QGridLayout;
	grid->setHorizontalSpacing(12);
	grid->setVerticalSpacing(8);
	grid->setColumnStretch(1, 1);
	for (int i = 0; i < entries_.size(); ++i) {
		const bool canEdit = editable.isEmpty() || editable.contains(entries_[i].id);

		auto *name = new QLabel(entries_[i].label, this);
		if (!canEdit)
			name->setStyleSheet(QStringLiteral("color:#8a8f98;"));
		grid->addWidget(name, i, 0);

		auto *edit = new QKeySequenceEdit(entries_[i].key, this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
		// One chord: these keys end up in RegisterHotKey, which knows nothing
		// about sequences.
		edit->setMaximumSequenceLength(1);
#endif
		edit->setEnabled(canEdit);
		// Editing a row updates that row's binding immediately, so the marks
		// and the OK button track what is on screen rather than what was on
		// screen when the dialog opened.
		connect(edit, &QKeySequenceEdit::keySequenceChanged, this,
			[this, i](const QKeySequence &k) {
				entries_[i].key = k;
				refresh();
			});
		edits_.append(edit);
		grid->addWidget(edit, i, 1);

		auto *clear = new QPushButton(QStringLiteral("Clear"), this);
		clear->setEnabled(canEdit);
		clear->setToolTip(QStringLiteral("Leave this action with no shortcut."));
		// Focus goes back to the field, not to the button that was clicked.
		// Clearing a key is almost always the first half of "and now type a
		// different one", and a QKeySequenceEdit only records what it has focus
		// for -- so leaving focus on the button meant the next keypress went
		// nowhere.
		connect(clear, &QPushButton::clicked, this, [edit]() {
			edit->clear();
			edit->setFocus(Qt::OtherFocusReason);
		});
		grid->addWidget(clear, i, 2);

		auto *mark = new QLabel(this);
		marks_.append(mark);
		grid->addWidget(mark, i, 3);
	}
	v->addLayout(grid);

	status_ = new QLabel(this);
	status_->setWordWrap(true);
	v->addWidget(status_);

	auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	okButton_ = box->button(QDialogButtonBox::Ok);
	okButton_->setText(QStringLiteral("Apply"));
	// Cancel means "throw away the whole save", not "keep the duplicate" --
	// there is no path out of this dialog that leaves two things on one key.
	box->button(QDialogButtonBox::Cancel)->setToolTip(
		QStringLiteral("Go back without saving any of these changes."));
	connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
	v->addWidget(box);

	refresh();
	if (!edits_.isEmpty())
		edits_.first()->setFocus();
}

ShortcutConflictDialog::~ShortcutConflictDialog()
{
	// Cut the key fields loose before anything is torn down.
	//
	// QKeySequenceEdit emits keySequenceChanged from focusOutEvent -- it
	// finalises whatever was typed when it loses focus -- and closing this
	// dialog moves focus off it. By then the destructor chain has already run
	// this class's part: entries_, edits_ and marks_ are gone. QObject only
	// severs connections in ~QObject, which runs last, so the slot fires in
	// between and refresh() walks a QVector that no longer exists.
	//
	// A debug Qt catches it as an assertion; a release build reads freed memory
	// and carries on until the heap notices. Either way the dialog could take
	// the app down on its way out.
	for (QKeySequenceEdit *e : edits_)
		if (e)
			e->disconnect(this);
}

bool ShortcutConflictDialog::resolvedForTest() const
{
	return !hasShortcutConflict(entries_);
}

void ShortcutConflictDialog::refresh()
{
	const QVector<QVector<int>> groups = shortcutConflicts(entries_);
	QSet<int> clashing;
	for (const QVector<int> &g : groups)
		for (int i : g)
			clashing.insert(i);

	for (int i = 0; i < marks_.size(); ++i) {
		const bool bad = clashing.contains(i);
		marks_[i]->setText(bad ? QStringLiteral("⚠") : QString());
		marks_[i]->setStyleSheet(QStringLiteral("color:#e5484d; font-weight:bold;"));
		marks_[i]->setToolTip(bad ? QStringLiteral("Still shares this key with another action.")
					  : QString());
		if (i < edits_.size())
			edits_[i]->setStyleSheet(
				bad ? QStringLiteral("border:1px solid #e5484d; border-radius:3px;")
				    : QString());
	}

	const bool ok = groups.isEmpty();
	if (okButton_)
		okButton_->setEnabled(ok);
	if (status_) {
		status_->setText(ok ? QStringLiteral("No duplicates — this can be applied.")
				    : QStringLiteral("%1 key%2 still assigned twice.")
					      .arg(groups.size())
					      .arg(groups.size() == 1 ? QString()
								      : QStringLiteral("s")));
		status_->setStyleSheet(ok ? QStringLiteral("color:#3fb950;")
					  : QStringLiteral("color:#e5484d;"));
	}
}

} // namespace harpia
