// Two things on one key: the rule, and the dialog that enforces it.
//
// A duplicate keyboard shortcut is the quietest possible broken setting. The
// second feature simply never fires, and where both bindings are window-scoped
// Qt fires NEITHER rather than guessing -- so one duplicate can kill two
// features and report nothing. Which is why the rule is not "warn about it" but
// "it cannot be saved", and why that has to be tested rather than trusted.
//
// The checks below cover both halves:
//   * shortcutConflicts(), the pure rule -- including the two cases a naive
//     implementation gets wrong: unassigned bindings are not "all the same
//     key", and a sequence parsed from a string must equal the same sequence
//     captured from the keyboard;
//   * ShortcutConflictDialog, which must refuse to be applied while a duplicate
//     is on screen and must stop refusing the moment one is changed -- with the
//     CONTROL that a dialog built from a clean set is applyable immediately, so
//     "OK is disabled" is not just its permanent state.
#include "core/ShortcutConflicts.hpp"
#include "ui/ShortcutConflictDialog.hpp"
#include <QPushButton>
#include <QKeySequenceEdit>

#include <QApplication>
#include <QKeySequence>
#include <QSet>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

static ShortcutBinding sb(const char *id, const char *label, const char *key)
{
	return ShortcutBinding{QString::fromLatin1(id), QString::fromLatin1(label),
			       QKeySequence(QString::fromLatin1(key))};
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	std::printf("\n-- the rule --\n");
	{
		QVector<ShortcutBinding> clean{sb("record", "Start / stop", "F9"),
					       sb("pause", "Pause", "F10"),
					       sb("zoom", "Zoom", "Ctrl+Shift+Z"),
					       sb("spot", "Spotlight", "Ctrl+Shift+S")};
		ok(!hasShortcutConflict(clean), "four different keys is not a conflict");
		ok(shortcutConflicts(clean).isEmpty(), "and no groups come out of it");

		QVector<ShortcutBinding> clash = clean;
		clash[3].key = QKeySequence(QStringLiteral("Ctrl+Shift+Z"));
		ok(hasShortcutConflict(clash), "two on one key IS a conflict");
		const auto groups = shortcutConflicts(clash);
		ok(groups.size() == 1, "reported as one contested key");
		ok(groups.size() == 1 && groups[0].size() == 2, "with both holders named");
		ok(groups.size() == 1 && groups[0][0] == 2 && groups[0][1] == 3,
		   "in ascending order, so the dialog lists them the same way every time");
	}

	std::printf("\n-- unassigned is not a key --\n");
	{
		// The failure mode: an empty QKeySequence compares equal to every other
		// empty one, so a naive grouping reports every unbound action as
		// clashing with every other and the dialog becomes unpassable.
		QVector<ShortcutBinding> mostlyEmpty{sb("a", "A", ""), sb("b", "B", ""),
						     sb("c", "C", ""), sb("d", "D", "F9")};
		ok(!hasShortcutConflict(mostlyEmpty),
		   "three actions with no shortcut do not all conflict with each other");
		// CONTROL: give two of them the SAME real key and it is caught, so the
		// check above is not just "this function always says no".
		mostlyEmpty[0].key = QKeySequence(QStringLiteral("F9"));
		ok(hasShortcutConflict(mostlyEmpty), "CONTROL: give one of them F9 and it is caught");
	}

	std::printf("\n-- the same key, arrived at two ways --\n");
	{
		// One parsed from a stored preset string, one built from a key
		// combination the way a capture widget produces it. They must count as
		// the same key, because the operating system will treat them as one.
		ShortcutBinding parsed = sb("zoom", "Zoom", "Ctrl+Shift+Z");
		ShortcutBinding captured{QStringLiteral("spot"), QStringLiteral("Spotlight"),
					 QKeySequence(QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier,
								      Qt::Key_Z))};
		std::printf("     parsed=\"%s\"  captured=\"%s\"\n",
			    qUtf8Printable(shortcutKeyId(parsed.key)),
			    qUtf8Printable(shortcutKeyId(captured.key)));
		ok(hasShortcutConflict({parsed, captured}),
		   "a stored key and a captured key that mean the same thing collide");
	}

	std::printf("\n-- who owns a key --\n");
	{
		const QVector<ShortcutBinding> all{sb("record", "Start / stop", "F9"),
						   sb("pause", "Pause", "F10")};
		ok(shortcutOwner(all, QKeySequence(QStringLiteral("F9"))) ==
			   QStringLiteral("Start / stop"),
		   "a taken key names its holder");
		ok(shortcutOwner(all, QKeySequence(QStringLiteral("F11"))).isEmpty(),
		   "a free key names nobody");
		ok(shortcutOwner(all, QKeySequence(QStringLiteral("F9")), QStringLiteral("record"))
			   .isEmpty(),
		   "and a binding does not clash with itself when it keeps its own key");
		ok(shortcutOwner(all, QKeySequence()).isEmpty(), "asking about no key at all is free");
	}

	std::printf("\n-- the dialog will not let a duplicate through --\n");
	{
		QVector<ShortcutBinding> clash{sb("zoom", "Zoom in / out", "Ctrl+Shift+Z"),
					       sb("spot", "Spotlight on / off", "Ctrl+Shift+Z")};
		ShortcutConflictDialog dlg(clash, QSet<QString>());
		ok(!dlg.resolvedForTest(), "it opens unresolved, so Apply is not available");
		ok(dlg.result().size() == 2, "and it carries both actions, not just the loser");
		ok(dlg.result()[0].label == QStringLiteral("Zoom in / out") &&
			   dlg.result()[1].label == QStringLiteral("Spotlight on / off"),
		   "both are named — the point is being able to move EITHER one");

		// CONTROL: the same dialog built from a clean pair is immediately
		// applyable, so "not resolved" is a verdict rather than a constant.
		QVector<ShortcutBinding> fine{sb("zoom", "Zoom in / out", "Ctrl+Shift+Z"),
					      sb("spot", "Spotlight on / off", "Ctrl+Shift+S")};
		ShortcutConflictDialog okDlg(fine, QSet<QString>());
		ok(okDlg.resolvedForTest(), "CONTROL: a clean pair is resolved from the start");
	}

	std::printf("\n-- resolving it --\n");
	{
		// Clearing one side is a legitimate fix: an action with no shortcut is
		// not in anybody's way.
		QVector<ShortcutBinding> cleared{sb("zoom", "Zoom", "Ctrl+Shift+Z"),
						 sb("spot", "Spotlight", "")};
		ok(!hasShortcutConflict(cleared), "clearing one of them resolves it");

		// So is moving it, and moving it onto a THIRD action's key does not.
		QVector<ShortcutBinding> moved{sb("record", "Start / stop", "F9"),
					       sb("zoom", "Zoom", "Ctrl+Shift+Z"),
					       sb("spot", "Spotlight", "F9")};
		ok(hasShortcutConflict(moved),
		   "moving the clash onto a third action is still a clash, not a fix");
		moved[2].key = QKeySequence(QStringLiteral("Ctrl+Shift+S"));
		ok(!hasShortcutConflict(moved), "moving it somewhere genuinely free is");
	}

	std::printf("\n-- three on one key --\n");
	{
		// The case a two-way "replace it?" prompt cannot express at all.
		QVector<ShortcutBinding> three{sb("a", "A", "F9"), sb("b", "B", "F9"),
					       sb("c", "C", "F9"), sb("d", "D", "F10")};
		const auto groups = shortcutConflicts(three);
		ok(groups.size() == 1 && groups[0].size() == 3, "all three are reported in one group");
		ShortcutConflictDialog dlg(three, QSet<QString>());
		ok(!dlg.resolvedForTest(), "and the dialog holds out until all three are settled");
	}

	std::printf("\n-- typing into the dialog --\n");
	{
		// The bug this screen actually had: the keys it asks you to press were
		// registered system-wide, so the OS ate them before the field saw
		// anything and no new shortcut could be entered. That half is fixed in
		// MainWindow (the keys are released while a key field has focus) and
		// cannot be tested without an OS. What CAN be tested is the second
		// half: after Clear, the field must still be the thing the keyboard is
		// talking to.
		QVector<ShortcutBinding> two{sb("zoom", "Zoom", "F9"), sb("spot", "Spotlight", "F9")};
		ShortcutConflictDialog dlg(two, QSet<QString>());
		dlg.show();
		QApplication::processEvents();

		const QList<QKeySequenceEdit *> edits = dlg.findChildren<QKeySequenceEdit *>();
		const QList<QPushButton *> buttons = dlg.findChildren<QPushButton *>();
		ok(edits.size() == 2, "one key field per contested action");

		QPushButton *clear = nullptr;
		for (QPushButton *b : buttons)
			if (b->text() == QStringLiteral("Clear")) {
				clear = b;
				break;
			}
		ok(clear != nullptr, "and a Clear beside it");
		if (clear && edits.size() == 2) {
			clear->click();
			QApplication::processEvents();
			ok(edits[0]->keySequence().isEmpty(), "Clear empties the field");
			// Without this the next keypress goes to the button, and the user
			// concludes the dialog is broken -- which is how this started.
			ok(edits[0]->hasFocus(),
			   "and leaves the keyboard talking to the field, not the button");
			ok(dlg.resolvedForTest(),
			   "clearing one of two identical keys settles the conflict");
		}
	}

	std::printf("\n%s (%d failures)\n",
		    failures ? "FAILURES" : "all shortcut-conflict checks passed", failures);
	return failures ? 1 : 0;
}
