// The hotkey translation: QKeySequence -> Win32 (modifiers, virtual-key).
//
// The RegisterHotKey half of GlobalHotkeys only compiles on Windows, but the
// half that can rot silently is this mapping -- get a VK code wrong and the
// hotkey registers FINE and then fires for a different key, which no error
// path will ever report. The Win32 values are ABI constants, so they can be
// pinned here on any platform.
#include "core/GlobalHotkeys.hpp"

#include <QCoreApplication>

#include <cstdio>

using namespace harpia;
static int failures = 0;
static void ok(bool c, const char *w)
{
	std::printf("  %s %s\n", c ? "PASS" : "FAIL", w);
	if (!c)
		++failures;
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	std::printf("\n-- the defaults, byte for byte --\n");
	{
		const WinHotkey f9 = winHotkeyFor(QKeySequence(QStringLiteral("F9")));
		std::printf("     F9 -> mods=0x%X vk=0x%X\n", f9.mods, f9.vk);
		ok(f9.valid && f9.mods == 0 && f9.vk == 0x78, "F9 is VK_F9 (0x78), no modifiers");
		const WinHotkey f10 = winHotkeyFor(QKeySequence(QStringLiteral("F10")));
		ok(f10.valid && f10.vk == 0x79, "F10 is VK_F9+1 -- the F block is contiguous");
	}

	std::printf("\n-- modifiers combine, in Win32's own bit values --\n");
	{
		const WinHotkey k = winHotkeyFor(QKeySequence(QStringLiteral("Ctrl+Shift+R")));
		std::printf("     Ctrl+Shift+R -> mods=0x%X vk=0x%X\n", k.mods, k.vk);
		ok(k.valid, "a modified letter maps");
		ok(k.mods == (0x2 | 0x4), "Ctrl|Shift is MOD_CONTROL|MOD_SHIFT (0x6)");
		ok(k.vk == 'R', "and the letter is its own VK code");

		const WinHotkey alt = winHotkeyFor(QKeySequence(QStringLiteral("Alt+F5")));
		ok(alt.valid && alt.mods == 0x1 && alt.vk == 0x74, "Alt+F5: MOD_ALT with VK_F5");
	}

	std::printf("\n-- digits, and the named keys --\n");
	{
		ok(winHotkeyFor(QKeySequence(QStringLiteral("Ctrl+1"))).vk == '1', "digits are their own VKs");
		ok(winHotkeyFor(QKeySequence(QStringLiteral("Pause"))).vk == 0x13, "Pause is VK_PAUSE");
		ok(winHotkeyFor(QKeySequence(QStringLiteral("Ctrl+Home"))).vk == 0x24, "Home is VK_HOME");
	}

	std::printf("\n-- what must refuse rather than guess --\n");
	{
		ok(!winHotkeyFor(QKeySequence()).valid, "an empty sequence is invalid, not 'key 0'");
		// Two chords: RegisterHotKey has no notion of sequences; mapping only
		// the first would silently drop half of what the user typed.
		ok(!winHotkeyFor(QKeySequence(QStringLiteral("Ctrl+K, Ctrl+B"))).valid,
		   "a two-chord sequence is refused whole");
		// A key with no stable VK: better no global hotkey (the window-scoped
		// fallback still works) than a hotkey that fires for something else.
		ok(!winHotkeyFor(QKeySequence(QStringLiteral("Ctrl+µ"))).valid,
		   "an unmappable key refuses instead of guessing");
	}

	std::printf("\n-- Mac native codes and Qt's Command semantics --\n");
	{
		const MacHotkey f9 = macHotkeyFor(QKeySequence(QStringLiteral("F9")));
		const MacHotkey f10 = macHotkeyFor(QKeySequence(QStringLiteral("F10")));
		ok(f9.valid && f9.modifiers == 0 && f9.keyCode == 109 && f9.character == 0,
		   "Mac F9 is physical code 109, without a Windows virtual-key assumption");
		ok(f10.valid && f10.keyCode == 103, "Mac F10 uses its non-contiguous physical code");
		const MacHotkey zoom = macHotkeyFor(QKeySequence(QStringLiteral("Ctrl+Shift+Z")));
		ok(zoom.valid && zoom.modifiers == (0x100 | 0x200) && zoom.character == 'Z',
		   "portable Ctrl+Shift+Z becomes Command+Shift and a layout-resolved character");
		const MacHotkey physicalControl = macHotkeyFor(QKeySequence(Qt::META | Qt::ALT | Qt::Key_F4));
		ok(physicalControl.valid && physicalControl.modifiers == (0x1000 | 0x800) && physicalControl.keyCode == 118,
		   "Qt Meta means physical Control on Mac, and Alt means Option");
		ok(macHotkeyFor(QKeySequence(Qt::Key_F20)).valid && !macHotkeyFor(QKeySequence(Qt::Key_F21)).valid,
		   "only the Mac's defined F1 through F20 codes are accepted");
		ok(macHotkeyFor(QKeySequence(Qt::Key_Backspace)).keyCode == 51 &&
		   macHotkeyFor(QKeySequence(Qt::Key_Delete)).keyCode == 117,
		   "Backspace and forward Delete remain different native keys");
		ok(macHotkeyFor(QKeySequence(Qt::Key_Left)).keyCode == 123 &&
		   macHotkeyFor(QKeySequence(Qt::Key_Right)).keyCode == 124,
		   "arrow-key physical codes are stable");
		ok(macHotkeyFor(QKeySequence(QStringLiteral("Ctrl+1"))).character == '1',
		   "digit shortcuts resolve through the layout rather than US number-row positions");
		ok(!macHotkeyFor(QKeySequence()).valid &&
		   !macHotkeyFor(QKeySequence(QStringLiteral("Ctrl+K, Ctrl+B"))).valid,
		   "empty and multi-chord bindings cannot become partial Mac hotkeys");
		ok(!macHotkeyFor(QKeySequence(Qt::Key_Pause)).valid &&
		   !macHotkeyFor(QKeySequence(Qt::Key_ydiaeresis)).valid &&
		   !macHotkeyFor(QKeySequence(Qt::KeypadModifier | Qt::Key_1)).valid,
		   "unsupported keys and ambiguous keypad bindings keep the window fallback");
	}

	std::printf("\n-- unsupported native backends keep their fallback --\n");
	{
#if !defined(_WIN32) && !defined(__APPLE__)
		GlobalHotkeys hk;
		ok(!hk.bind(1, QKeySequence(QStringLiteral("F9"))),
		   "no global backend here: bind reports failure honestly");
#else
		std::printf("     (skipped: native registration needs an interactive desktop)\n");
#endif
	}

	std::printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED (0 failures)");
	return failures ? 1 : 0;
}
