#pragma once

// System-wide recording hotkeys.
//
// F9/F10 were window-scoped QShortcuts presented as recording hotkeys -- but
// during a recording Harpia is almost never the focused window, which is to
// say they mostly did not work at exactly the moment they were for. On
// Windows uses RegisterHotKey and macOS uses Carbon RegisterEventHotKey, so
// these fire whichever app is in front. Other platforms keep Qt shortcuts.
//
// The QKeySequence -> (modifiers, virtual-key) translation is a pure function
// with the Win32 constants mirrored locally, so the half of this that can rot
// silently -- the mapping -- is compiled and tested on every platform, while
// native registration stays in its platform backend. A refused registration
// keeps the caller's window-scoped QShortcut available.

#include <QAbstractNativeEventFilter>
#include <QKeySequence>
#include <QObject>
#include <memory>

namespace harpia {

// Win32 values, mirrored so the mapping is testable off-Windows. They are ABI
// constants, not headers' whims -- MOD_ALT has been 1 since Windows 3.1.
struct WinHotkey {
	unsigned mods = 0; // MOD_ALT=1 | MOD_CONTROL=2 | MOD_SHIFT=4 | MOD_WIN=8
	unsigned vk = 0;   // virtual-key code
	bool valid = false;
};

// The pure translation. Invalid when the sequence is empty, has more than one
// chord, or names a key with no stable virtual-key equivalent.
WinHotkey winHotkeyFor(const QKeySequence &seq);

// Carbon constants mirrored for portable mapping tests. Letter/digit bindings
// carry a character; the native backend resolves it using the active keyboard
// layout rather than assuming US physical key positions.
struct MacHotkey {
	unsigned modifiers = 0; // cmdKey=256 | shiftKey=512 | optionKey=2048 | controlKey=4096
	unsigned keyCode = 0;
	unsigned character = 0;
	bool valid = false;
};
MacHotkey macHotkeyFor(const QKeySequence &seq);

#if defined(__APPLE__)
class MacGlobalHotkeys;
#endif

class GlobalHotkeys : public QObject, public QAbstractNativeEventFilter {
	Q_OBJECT
public:
	explicit GlobalHotkeys(QObject *parent = nullptr);
	~GlobalHotkeys() override;

	// Register `seq` system-wide under `id` (1-based, stable per action).
	// Replaces any previous binding for the id. False when the key cannot be
	// mapped or the OS refuses (another app holds it) -- the caller should
	// keep its window-scoped fallback in that case.
	bool bind(int id, const QKeySequence &seq);
	void unbindAll();

	bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

signals:
	void triggered(int id);
	// The native keyboard layout changed. Rebind the configured actions so
	// character keys and registration/fallback hints follow the new layout.
	void bindingsChanged();

private:
	bool installed_ = false;
#if defined(__APPLE__)
	std::unique_ptr<MacGlobalHotkeys> mac_;
#endif
};

} // namespace harpia
