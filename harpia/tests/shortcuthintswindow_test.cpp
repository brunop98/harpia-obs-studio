// Native/configured shortcut hints through the real editor and options window.
#include "core/RecorderSettings.hpp"
#include "editor/ShortcutRegistry.hpp"
#include "editor/VideoEditorWindow.hpp"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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

static QPushButton *button(QWidget &window, const QString &text)
{
	for (auto *candidate : window.findChildren<QPushButton *>())
		if (candidate->text() == text)
			return candidate;
	return nullptr;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	harpia::recorderSettings().remove(QStringLiteral("editorShortcuts"));
	VideoEditorWindow window(QString{});
	auto *registry = window.findChild<ShortcutRegistry *>();
	check(registry != nullptr, "real editor exposes its active shortcut registry");
	if (!registry)
		return 1;
	auto *full = button(window, QStringLiteral("Full Editing"));
	if (full)
		full->click();
	auto *randomize = button(window, QStringLiteral("Randomize"));
	auto *console = button(window, QStringLiteral("Console"));
	auto *snap = button(window, QStringLiteral("Snap on"));
	if (!snap)
		snap = button(window, QStringLiteral("Snap off"));
	check(randomize && console && snap, "toolbar controls are present");
	if (!randomize || !console || !snap)
		return 1;
	check(randomize->toolTip().contains(registry->displayText(QStringLiteral("timeline.randomize"))),
		"Randomize toolbar tip starts with the actual native binding");
	randomize->click();
	auto *randomButton = window.findChild<QPushButton *>(QStringLiteral("randomizeClipsButton"));
	auto *randomStatus = window.findChild<QLabel *>(QStringLiteral("randomizeStatus"));
	check(randomButton && randomStatus, "Randomize options display live command and Undo hints");
	if (!randomButton || !randomStatus)
		return 1;
	const QKeySequence customRandom(Qt::ALT | Qt::Key_R);
	const QKeySequence customUndo(Qt::ALT | Qt::Key_U);
	registry->setBindings(QStringLiteral("timeline.randomize"), {customRandom});
	registry->setBindings(QStringLiteral("edit.undo"), {customUndo});
	check(randomButton->text().contains(customRandom.toString(QKeySequence::NativeText)) &&
		randomize->toolTip().contains(customRandom.toString(QKeySequence::NativeText)),
		"custom Randomize binding updates both open options and toolbar");
	check(randomStatus->text().contains(customUndo.toString(QKeySequence::NativeText)),
		"custom Undo binding updates the options status");
	registry->setBindings(QStringLiteral("timeline.randomize"), {});
	registry->setBindings(QStringLiteral("edit.undo"), {});
	check(randomButton->text() == QStringLiteral("Randomize clip order") &&
		randomStatus->text() == QStringLiteral("Use Undo to bring the previous order back."),
		"unbound commands disappear from open option hints without disabling buttons");
	const QKeySequence customSnap(Qt::ALT | Qt::Key_N);
	registry->setBindings(QStringLiteral("timeline.snap"), {customSnap});
	snap->click();
	check(snap->toolTip().contains(customSnap.toString(QKeySequence::NativeText)) && !snap->toolTip().contains(QStringLiteral("(N)")),
		"snap on/off preserves its custom shortcut hint");
	const QKeySequence customConsole(Qt::ALT | Qt::Key_C);
	registry->setBindings(QStringLiteral("view.console"), {customConsole});
	check(console->toolTip().contains(customConsole.toString(QKeySequence::NativeText)),
		"console tooltip follows its configured binding");
	registry->resetAllToDefaults();
	check(randomButton->text() == registry->actionText(QStringLiteral("timeline.randomize")) &&
		randomStatus->text().contains(registry->displayText(QStringLiteral("edit.undo"))),
		"reset refreshes labels in an already open options window");
	check(!ShortcutRegistry::modifierText(Qt::ControlModifier).isEmpty() &&
		!ShortcutRegistry::modifierText(Qt::AltModifier).isEmpty(), "native mouse modifier names are available");
	check(registry->displayText(QStringLiteral("timeline.zoom")) ==
		ShortcutRegistry::mouseGesture(Qt::ControlModifier, QStringLiteral("Mouse wheel")),
		"mouse reference uses the native name of the modifier actually read by the handler");
#ifdef Q_OS_MAC
	check(!randomize->toolTip().contains(QStringLiteral("Ctrl+")) &&
		!registry->displayText(QStringLiteral("timeline.zoom")).contains(QStringLiteral("Ctrl")),
		"Mac hints use native Command notation rather than Windows Ctrl labels");
#endif
	window.close();
	std::printf("%s\n", failures ? "FAILURES" : "ALL PASSED");
	return failures ? 1 : 0;
}
