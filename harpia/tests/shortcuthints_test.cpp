// Visible shortcut hints must describe the active binding, including when a
// command is rebound or unbound while its options window remains open.
#include "core/RecorderSettings.hpp"
#include "editor/ShortcutRegistry.hpp"

#include <QApplication>
#include <QLabel>
#include <QSettings>
#include <QWidget>

#include <cstdio>

using namespace harpia;

static int failures = 0;
static void check(bool condition, const char *message)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
	if (!condition)
		++failures;
}


int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	harpia::recorderSettings().remove(QStringLiteral("editorShortcuts"));
	{
		QWidget host;
		QWidget nested(&host);
		ShortcutRegistry registry(&host, &host);
		ShortcutCommand undo;
		undo.id = QStringLiteral("edit.undo");
		undo.label = QStringLiteral("Undo");
		undo.category = QStringLiteral("Editing");
		undo.defaults = {QKeySequence::Undo};
		registry.addCommand(undo);
		registry.load();
		QLabel status(&nested);
		registry.bindHint(&status, "text", QStringLiteral("Use {edit.undo} to restore the clip."));
		check(ShortcutRegistry::forWidget(&nested) == &registry, "nested dialogs find their editor's registry");
		const QString nativeUndo = QKeySequence(QKeySequence::Undo).toString(QKeySequence::NativeText);
		check(status.text() == QStringLiteral("Use Undo (%1) to restore the clip.").arg(nativeUndo),
			"default hint uses Qt's platform-native standard Undo shortcut");
		const QKeySequence custom(Qt::ALT | Qt::Key_U);
		registry.setBindings(undo.id, {custom});
		check(status.text().contains(custom.toString(QKeySequence::NativeText)) && !status.text().contains(nativeUndo),
			"rebinding replaces the old key in an already visible hint");
		registry.setBindings(undo.id, {});
		check(status.text() == QStringLiteral("Use Undo to restore the clip."),
			"unbinding keeps the command name without an empty or stale shortcut");
		registry.resetToDefault(undo.id);
		check(status.text().contains(nativeUndo), "reset restores the native default immediately");
		const QByteArray imported = R"({"format":"harpia-shortcuts","bindings":{"edit.undo":["Alt+U","Alt+Shift+U"]}})";
		check(registry.importProfile(imported), "shortcut profile imports");
		check(status.text().contains(QKeySequence(QStringLiteral("Alt+Shift+U")).toString(QKeySequence::NativeText)),
			"profile import refreshes all configured alternatives in the hint");
		status.setText(QStringLiteral("Loading a new project…"));
		registry.resetAllToDefaults();
		check(status.text() == QStringLiteral("Loading a new project…"),
			"a rebind cannot restore old success text over a newer operation's status");
		registry.bindHint(&status, "text", QStringLiteral("Use {edit.undo} to reverse the new edit."));
		registry.setBindings(undo.id, {});
		check(status.text() == QStringLiteral("Use Undo to reverse the new edit."), "reusing a status label replaces its hint template");
		auto *temporary = new QLabel(&nested);
		registry.bindHint(temporary, "text", QStringLiteral("{edit.undo}"));
		delete temporary;
		registry.resetAllToDefaults();
		check(true, "destroying a hinted menu or dialog is safe before a rebind");
	}
	harpia::recorderSettings().remove(QStringLiteral("editorShortcuts"));
	check(!ShortcutRegistry::modifierText(Qt::ControlModifier).isEmpty() &&
		!ShortcutRegistry::modifierText(Qt::AltModifier).isEmpty(), "native mouse modifier names are available");
	std::printf("     native mouse modifiers: %s, %s\n", qPrintable(ShortcutRegistry::modifierText(Qt::ControlModifier)),
		qPrintable(ShortcutRegistry::modifierText(Qt::AltModifier)));
	std::printf("%s\n", failures ? "FAILURES" : "ALL PASSED");
	return failures ? 1 : 0;
}
