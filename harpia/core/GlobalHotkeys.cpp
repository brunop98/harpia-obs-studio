#include "GlobalHotkeys.hpp"

#include <QCoreApplication>
#include <QPointer>

#if defined(__APPLE__)
#include "../platform/mac/MacGlobalHotkeys.hpp"
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace harpia {

namespace {
// Mirrored Win32 modifier values (see the header for why they are spelled out).
constexpr unsigned kModAlt = 0x1;
constexpr unsigned kModControl = 0x2;
constexpr unsigned kModShift = 0x4;
constexpr unsigned kModWin = 0x8;
} // namespace

WinHotkey winHotkeyFor(const QKeySequence &seq)
{
	WinHotkey out;
	if (seq.isEmpty() || seq.count() != 1)
		return out; // one chord or nothing: RegisterHotKey knows no sequences

	const QKeyCombination combo = seq[0];
	const Qt::KeyboardModifiers m = combo.keyboardModifiers();
	if (m & Qt::ControlModifier)
		out.mods |= kModControl;
	if (m & Qt::AltModifier)
		out.mods |= kModAlt;
	if (m & Qt::ShiftModifier)
		out.mods |= kModShift;
	if (m & Qt::MetaModifier)
		out.mods |= kModWin;

	const int key = combo.key();
	if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
		out.vk = 0x70 + (key - Qt::Key_F1); // VK_F1..VK_F24
	} else if (key >= Qt::Key_A && key <= Qt::Key_Z) {
		out.vk = 'A' + (key - Qt::Key_A);
	} else if (key >= Qt::Key_0 && key <= Qt::Key_9) {
		out.vk = '0' + (key - Qt::Key_0);
	} else {
		switch (key) {
		case Qt::Key_Space:
			out.vk = 0x20;
			break;
		case Qt::Key_Pause:
			out.vk = 0x13;
			break;
		case Qt::Key_Insert:
			out.vk = 0x2D;
			break;
		case Qt::Key_Delete:
			out.vk = 0x2E;
			break;
		case Qt::Key_Home:
			out.vk = 0x24;
			break;
		case Qt::Key_End:
			out.vk = 0x23;
			break;
		case Qt::Key_PageUp:
			out.vk = 0x21;
			break;
		case Qt::Key_PageDown:
			out.vk = 0x22;
			break;
		default:
			return out; // no stable VK: refuse rather than guess
		}
	}
	out.valid = true;
	return out;
}

MacHotkey macHotkeyFor(const QKeySequence &seq)
{
	MacHotkey out;
	if (seq.isEmpty() || seq.count() != 1)
		return out;
	const QKeyCombination combo = seq[0];
	const Qt::KeyboardModifiers mods = combo.keyboardModifiers();
	const Qt::KeyboardModifiers supported = Qt::ControlModifier | Qt::MetaModifier |
		Qt::AltModifier | Qt::ShiftModifier;
	if (mods & ~supported)
		return out;
	// Qt deliberately calls Command "ControlModifier" on macOS, and the
	// physical Control key "MetaModifier". Portable Ctrl+ shortcuts stay Cmd+.
	if (mods & Qt::ControlModifier)
		out.modifiers |= 0x100; // cmdKey
	if (mods & Qt::ShiftModifier)
		out.modifiers |= 0x200; // shiftKey
	if (mods & Qt::AltModifier)
		out.modifiers |= 0x800; // optionKey
	if (mods & Qt::MetaModifier)
		out.modifiers |= 0x1000; // controlKey
	const int key = combo.key();
	if (key >= Qt::Key_F1 && key <= Qt::Key_F20) {
		// Mac function-key virtual codes are not contiguous.
		static constexpr unsigned codes[] = {
			122, 120, 99, 118, 100, 97, 98, 101, 109, 103,
			111, 105, 107, 113, 106, 64, 79, 80, 90, 91};
		out.keyCode = codes[key - Qt::Key_F1];
	} else if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9)) {
		out.character = unsigned(key);
	} else {
		switch (key) {
		case Qt::Key_Space: out.keyCode = 49; break;
		case Qt::Key_Return: out.keyCode = 36; break;
		case Qt::Key_Enter: out.keyCode = 76; break;
		case Qt::Key_Tab: out.keyCode = 48; break;
		case Qt::Key_Escape: out.keyCode = 53; break;
		case Qt::Key_Backspace: out.keyCode = 51; break;
		case Qt::Key_Delete: out.keyCode = 117; break;
		case Qt::Key_Home: out.keyCode = 115; break;
		case Qt::Key_End: out.keyCode = 119; break;
		case Qt::Key_PageUp: out.keyCode = 116; break;
		case Qt::Key_PageDown: out.keyCode = 121; break;
		case Qt::Key_Left: out.keyCode = 123; break;
		case Qt::Key_Right: out.keyCode = 124; break;
		case Qt::Key_Down: out.keyCode = 125; break;
		case Qt::Key_Up: out.keyCode = 126; break;
		default: return out;
		}
	}
	out.valid = true;
	return out;
}

GlobalHotkeys::GlobalHotkeys(QObject *parent) : QObject(parent)
{
#if defined(__APPLE__)
	QPointer<GlobalHotkeys> owner(this);
	mac_ = std::make_unique<MacGlobalHotkeys>(
		[owner](int id) {
			if (owner)
				QMetaObject::invokeMethod(owner, [owner, id]() {
					if (owner)
						emit owner->triggered(id);
				}, Qt::QueuedConnection);
		},
		[owner]() {
			if (owner)
				QMetaObject::invokeMethod(owner, [owner]() {
					if (owner)
						emit owner->bindingsChanged();
				}, Qt::QueuedConnection);
		});
#endif
}

GlobalHotkeys::~GlobalHotkeys()
{
	unbindAll();
}

bool GlobalHotkeys::bind(int id, const QKeySequence &seq)
{
#if defined(_WIN32)
	const WinHotkey hk = winHotkeyFor(seq);
	// Replace, never stack: RegisterHotKey with an id that is already
	// registered fails, so the old binding goes first. This happens BEFORE the
	// validity check, because "bind nothing to this slot" is a real request --
	// a preset that turns its zoom shortcut off must release the key, not
	// leave the previous preset's binding live and firing.
	UnregisterHotKey(nullptr, id);
	if (!hk.valid)
		return false;
	// MOD_NOREPEAT: holding the key must not machine-gun start/stop.
	if (!RegisterHotKey(nullptr, id, hk.mods | 0x4000 /*MOD_NOREPEAT*/, hk.vk))
		return false; // another app holds it; caller keeps its fallback
	if (!installed_) {
		QCoreApplication::instance()->installNativeEventFilter(this);
		installed_ = true;
	}
	return true;
#elif defined(__APPLE__)
	return mac_ && mac_->bind(id, macHotkeyFor(seq));
#else
	// No global-hotkey backend on this platform: the window-scoped QShortcuts
	// stay in charge, which is what this platform always had.
	Q_UNUSED(id);
	Q_UNUSED(seq);
	return false;
#endif
}

void GlobalHotkeys::unbindAll()
{
#if defined(_WIN32)
	// The ids this app ever uses are small and few; sweeping a fixed range is
	// simpler than bookkeeping and UnregisterHotKey on a free id is a no-op.
	for (int id = 1; id <= 8; ++id)
		UnregisterHotKey(nullptr, id);
#elif defined(__APPLE__)
	if (mac_)
		mac_->unbindAll();
#endif
	if (installed_ && QCoreApplication::instance()) {
		QCoreApplication::instance()->removeNativeEventFilter(this);
		installed_ = false;
	}
}

bool GlobalHotkeys::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
	Q_UNUSED(result);
#if defined(_WIN32)
	if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
		const MSG *msg = static_cast<const MSG *>(message);
		if (msg->message == WM_HOTKEY) {
			emit triggered(int(msg->wParam));
			return true;
		}
	}
#else
	Q_UNUSED(eventType);
	Q_UNUSED(message);
#endif
	return false;
}

} // namespace harpia
