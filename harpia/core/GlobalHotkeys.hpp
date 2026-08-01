#pragma once

// System-wide recording hotkeys.
//
// F9/F10 were window-scoped QShortcuts presented as recording hotkeys -- but
// during a recording Harpia is almost never the focused window, which is to
// say they mostly did not work at exactly the moment they were for. On
// Windows this registers them with RegisterHotKey, so they fire whichever app
// is in front; the WM_HOTKEY messages arrive through a native event filter.
//
// The QKeySequence -> (modifiers, virtual-key) translation is a pure function
// with the Win32 constants mirrored locally, so the half of this that can rot
// silently -- the mapping -- is compiled and tested on every platform, while
// the RegisterHotKey half stays behind #ifdef _WIN32. On other platforms
// bind() reports failure and the callers keep their window-scoped QShortcuts,
// which is exactly the behaviour those platforms had before.

#include <QAbstractNativeEventFilter>
#include <QKeySequence>
#include <QObject>

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

private:
	bool installed_ = false;
};

} // namespace harpia
